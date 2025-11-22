/*--------------------------------------------------------------------*/
/* execute.c */
/* Author: Jongki Park, Kyoungsoo Park */
/*--------------------------------------------------------------------*/

#include "dynarray.h"
#include "token.h"
#include "util.h"
#include "lexsyn.h"
#include "snush.h"
#include "execute.h"
#include "job.h"

extern struct job_manager *manager;
extern volatile sig_atomic_t sigchld_flag;
extern volatile sig_atomic_t sigint_flag;

/*--------------------------------------------------------------------*/
/* Helper Functions for Modularity of fork_exec, iter_pipe_fork_exec */
/*--------------------------------------------------------------------*/
/* Helper 0: Reconstruct command string for UI print */
static char *get_command_string(DynArray_T oTokens) {
    int i;
    int len = dynarray_get_length(oTokens);
    char *cmd_str = (char *)calloc(1, 1024); 
    if (!cmd_str) return NULL;

    for (i = 0; i < len; i++) {
        struct Token *t = dynarray_get(oTokens, i);
        
        if (i > 0) strcat(cmd_str, " ");

        switch (t->token_type) {
            case TOKEN_WORD:
                strcat(cmd_str, t->token_value);
                break;
            case TOKEN_PIPE:
                strcat(cmd_str, "|");
                break;
            case TOKEN_REDIN:
                strcat(cmd_str, "<");
                break;
            case TOKEN_REDOUT:
                strcat(cmd_str, ">");
                break;
            case TOKEN_BG:
                strcat(cmd_str, "&");
                break;
        }
    }
    return cmd_str;
}
/*--------------------------------------------------------------------*/
/* Helper Function 1, Allocate and initialize new job */
static struct job *init_job_struct(int is_background, int num_processes, DynArray_T oTokens) {
    struct job *new_job = (struct job *)calloc(1, sizeof(struct job));
    if (new_job == NULL) {
        error_print("Cannot allocate memory for job", FPRINTF);
        exit(EXIT_FAILURE);
    }
    new_job->state = is_background ? BACKGROUND : FOREGROUND;
    new_job->remaining_processes = num_processes;
    new_job->n_pids = num_processes;
    new_job->cmd_line_print = get_command_string(oTokens);
    
    return new_job;
}
/*--------------------------------------------------------------------*/
/* Helper Function 2, run inside the child process 
 * Handles signals, redirections (file & pipe), execution.
 * Does NOT return (calls execvp or exit).
 */
static void execute_child_process(DynArray_T oTokens, int start, int end, 
                                  int in_fd, int out_fd, 
                                  pid_t pgid, sigset_t *prev_mask) {
    char *args[MAX_ARGS_CNT];

    sigprocmask(SIG_SETMASK, prev_mask, NULL);

    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);

    if (in_fd != -1) {
        if (dup2(in_fd, STDIN_FILENO) < 0) {
            perror("dup2 in_fd error");
            exit(EXIT_FAILURE);
        }
        close(in_fd);
    }

    if (out_fd != -1) {
        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            perror("dup2 out_fd error");
            exit(EXIT_FAILURE);
        }
        close(out_fd);
    }

    setpgid(0, pgid);
    build_command_partial(oTokens, start, end, args);
    if (execvp(args[0], args) < 0) {
        fprintf(stderr, "%s: %s\n", args[0], strerror(errno));
        exit(EXIT_FAILURE);
    }
}
/*--------------------------------------------------------------------*/
/* Helper Function 3, Parent to finish job setup and wait,print */
static int handle_parent_job_control(struct job *new_job, pid_t pgid, 
                                     int is_background, sigset_t *prev_mask) {
    int job_id;

    new_job->pgid = pgid;

    job_id = add_job(new_job);


    if (!is_background) {
        tcsetpgrp(STDIN_FILENO, pgid);
    }

    sigprocmask(SIG_SETMASK, prev_mask, NULL);

    if (!is_background) {
        wait_fg(job_id);
        tcsetpgrp(STDIN_FILENO, getpgrp());
    } else {
        print_job(job_id, pgid);
    }

    return job_id;
}


/*--------------------------------------------------------------------*/
void block_signal(int sig, int block) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);

    if (sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL) < 0) {
        fprintf(stderr, 
            "[Error] block_signal: sigprocmask(%s, sig=%d) failed: %s\n",
            block ? "SIG_BLOCK" : "SIG_UNBLOCK", sig, strerror(errno));
        exit(EXIT_FAILURE);
    }
}
/*--------------------------------------------------------------------*/
void handle_sigchld(void) {

    /*
     * TODO: Implement handle_sigchld() in execute.c
     * Call waitpid() to wait for the child process to terminate.
     * If the child process terminates, handle the job accordingly.
     * Be careful to handle the SIGCHLD signal flag and unblock SIGCHLD.
    */
    int status;
    pid_t pid;
    struct job *job;
    int saved_errno = errno;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        int i, j;
        for (i = 0; i < MAX_JOBS; i++) {
            if (manager->jobs[i]) {
                job = manager->jobs[i];
                if (remove_pid_from_job(job, pid)) {
                    if (job->remaining_processes == 0 && job->state == BACKGROUND) {
                        job->state = FINISHED; 
                    } else if (job->remaining_processes == 0 && job->state == FOREGROUND) {
                        
                    }
                    break;
                }
            }
        }
    }
    errno = saved_errno;

}
/*--------------------------------------------------------------------*/
void handle_sigint(void) {
    
    /*
     * TODO: Implement handle_sigint() in execute.c
     * Find the foreground job and send signal to every process in the
     * process group.
     * Be careful to handle the SIGINT signal flag and unblock SIGINT.
     */
    int i;
    int saved_errno = errno;
    
    for (i = 0; i < MAX_JOBS; i++) {
        if (manager->jobs[i] && manager->jobs[i]->state == FOREGROUND) {
            kill(-manager->jobs[i]->pgid, SIGINT);
            break;
        }
    }
    errno = saved_errno;
    
}
/*--------------------------------------------------------------------*/
void dup2_e(int oldfd, int newfd, const char *func, const int line) {
    int ret;

    ret = dup2(oldfd, newfd);
    if (ret < 0) {
        fprintf(stderr, 
            "Error dup2(%d, %d): %s(%s) at (%s:%d)\n", 
            oldfd, newfd, strerror(errno), errno_name(errno), func, line);
        exit(EXIT_FAILURE);
    }
}
/*--------------------------------------------------------------------*/
/* Do not modify this function. It is used to check the signals and 
 * handle them accordingly. It is called in the main loop of snush.c.
 */
void check_signals(void) {
    handle_sigchld();
    handle_sigint();
}
/*--------------------------------------------------------------------*/
void redout_handler(char *fname) {
    int fd;

    fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        error_print(NULL, PERROR); 
        exit(EXIT_FAILURE);
    }

    dup2_e(fd, STDOUT_FILENO, __func__, __LINE__);
    close(fd);
}
/*--------------------------------------------------------------------*/
void redin_handler(char *fname) {
    int fd;

    fd = open(fname, O_RDONLY);
    if (fd < 0) {
        error_print(NULL, PERROR);
        exit(EXIT_FAILURE);
    }

    dup2_e(fd, STDIN_FILENO, __func__, __LINE__);
    close(fd);
}
/*--------------------------------------------------------------------*/
void build_command_partial(DynArray_T oTokens, int start, 
                        int end, char *args[]) {
    int i, redin = FALSE, redout = FALSE, cnt = 0;
    struct Token *t;

    /* Build command */
    for (i = start; i < end; i++) {
        t = dynarray_get(oTokens, i);

        if (t->token_type == TOKEN_WORD) {
            if (redin == TRUE) {
                redin_handler(t->token_value);
                redin = FALSE;
            }
            else if (redout == TRUE) {
                redout_handler(t->token_value);
                redout = FALSE;
            }
            else {
                args[cnt++] = t->token_value;
            }
        }
        else if (t->token_type == TOKEN_REDIN)
            redin = TRUE;
        else if (t->token_type == TOKEN_REDOUT)
            redout = TRUE;
    }

    if (cnt >= MAX_ARGS_CNT) 
        fprintf(stderr, "[BUG] args overflow! cnt=%d\n", cnt);

    args[cnt] = NULL;

#ifdef DEBUG
    for (i = 0; i < cnt; i++) {
        if (args[i] == NULL)
            printf("CMD: NULL\n");
        else
            printf("CMD: %s\n", args[i]);
    }
    printf("END\n");
#endif
}
/*--------------------------------------------------------------------*/
void build_command(DynArray_T oTokens, char *args[]) {
    build_command_partial(oTokens, 0, 
                        dynarray_get_length(oTokens), 
                        args);
}
/*--------------------------------------------------------------------*/
int execute_builtin_partial(DynArray_T toks, int start, int end,
                            enum BuiltinType btype, int in_child) {
    
    int argc = end - start;
    struct Token *t1;
    int ret;
    char *dir;

    switch (btype) {
    case B_EXIT:
        if (in_child) return 0;
        
        if (argc == 1) {
            dynarray_map(toks, free_token, NULL);
            dynarray_free(toks);
            exit(EXIT_SUCCESS);
        }
        else {
            error_print("exit does not take any parameters", FPRINTF);
            return -1;
        }

    case B_CD: {
        if (argc == 1) {
            dir = getenv("HOME");
            if (!dir) {
                error_print("cd: HOME variable not set", FPRINTF);
                return -1;
            }
        } 
        else if (argc == 2) {
            t1 = dynarray_get(toks, start + 1);
            if (t1 && t1->token_type == TOKEN_WORD) 
                dir = t1->token_value;
        } 
        else {
            error_print("cd: Too many parameters", FPRINTF);
            return -1;
        }

        ret = chdir(dir);
        if (ret < 0) {
            error_print(NULL, PERROR);
            return -1;
        }
        return 0;
    }

    default:
        error_print("Bug found in execute_builtin_partial", FPRINTF);
        return -1;
    }
}
/*--------------------------------------------------------------------*/
int execute_builtin(DynArray_T oTokens, enum BuiltinType btype) {
    return execute_builtin_partial(oTokens, 0, 
                                dynarray_get_length(oTokens), btype, FALSE);
}
/*--------------------------------------------------------------------*/
/* 
 * You need to finish implementing job related APIs. (find_job_by_jid(),
 * remove_pid_from_job(), delete_job()) in job.c to handle the job.
 * Feel free to modify the format of the job API according to your design.
 */
void wait_fg(int job_id) {
    pid_t pid;
    int status;

     // Find the job structure by job ID
    struct job *job = find_job_by_jid(job_id);
    if (!job) {
        fprintf(stderr, "Job: %d not found\n", job_id);
        return;
    }

    while (1) {
        pid = waitpid(-job->pgid, &status, 0);

        if (pid > 0) {
            // Remove the finished process from the job's pid list
            if (!remove_pid_from_job(job, pid)) {
                fprintf(stderr, "Pid %d not found in the job: %d list\n", 
                    pid, job->job_id);
            }

            if (job->remaining_processes == 0) break;
        }

        if (pid == 0) continue;

        if (pid < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            error_print("Unknown error waitpid() in wait_fg()", PERROR);
        }
    }

    // Clean up job table entry if all processes are done
    if (job->remaining_processes == 0)
        delete_job(job->job_id);
}
/*--------------------------------------------------------------------*/
void print_job(int job_id, pid_t pgid) {
    fprintf(stdout, 
        "[%d] Process group: %d running in the background\n", job_id, pgid);
}
/*--------------------------------------------------------------------*/
int fork_exec(DynArray_T oTokens, int is_background) {
    /*
     * TODO: Implement fork_exec() in execute.c
     * To run a newly forked process in the foreground, call wait_fg() 
     * to wait for the process to finish.  
     * To run it in the background, call print_job() to print job id and
     * process group id.  
     * All terminated processes must be handled by sigchld_handler() in * snush.c. 
     */

    pid_t pid;
    struct job *new_job;
    int len = dynarray_get_length(oTokens);
    sigset_t mask_one, prev_one;

    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask_one, &prev_one);

    new_job = init_job_struct(is_background, 1, oTokens);

    pid = fork();
    if (pid < 0) {
        perror("fork error");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) { 
        execute_child_process(oTokens, 0, len, -1, -1, 0, &prev_one);
    }

    setpgid(pid, pid); 
    new_job->pids[0] = pid;

    return handle_parent_job_control(new_job, pid, is_background, &prev_one);
}
/*--------------------------------------------------------------------*/
int iter_pipe_fork_exec(int n_pipe, DynArray_T oTokens, int is_background) {
    /*
     * TODO: Implement iter_pipe_fork_exec() in execute.c
     * To run a newly forked process in the foreground, call wait_fg() 
     * to wait for the process to finish.  
     * To run it in the background, call print_job() to print job id and
     * process group id.  
     * All terminated processes must be handled by sigchld_handler() in * snush.c. 
     */

    int i;
    int num_cmds = n_pipe + 1;
    int pipe_fd[2];
    int prev_read_fd = -1;
    pid_t pid;
    pid_t pgid = 0;
    struct job *new_job;
    int start_idx = 0;
    int len = dynarray_get_length(oTokens);
    sigset_t mask_one, prev_one;

    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask_one, &prev_one);
    new_job = init_job_struct(is_background, num_cmds, oTokens);

    for (i = 0; i < num_cmds; i++) {
        int end_idx = start_idx;
        struct Token *t;
        int current_in_fd = prev_read_fd;
        int current_out_fd = -1;

        while (end_idx < len) {
            t = dynarray_get(oTokens, end_idx);
            if (t->token_type == TOKEN_PIPE) break;
            end_idx++;
        }

        if (i < num_cmds - 1) {
            if (pipe(pipe_fd) < 0) {
                perror("pipe error");
                exit(EXIT_FAILURE);
            }
            current_out_fd = pipe_fd[1];
        }

        pid = fork();
        if (pid < 0) {
            perror("fork error");
            exit(EXIT_FAILURE);
        }

        if (pid == 0) { 
            if (i < num_cmds - 1) close(pipe_fd[0]); 
            
            execute_child_process(oTokens, start_idx, end_idx, 
                                  current_in_fd, current_out_fd, 
                                  pgid, &prev_one);
        }

        if (i == 0) pgid = pid; 
        setpgid(pid, pgid);
        
        new_job->pids[i] = pid;
        if (prev_read_fd != -1) close(prev_read_fd);
        if (i < num_cmds - 1) {
            close(pipe_fd[1]); 
            prev_read_fd = pipe_fd[0]; 
        }

        start_idx = end_idx + 1;
    }

    return handle_parent_job_control(new_job, pgid, is_background, &prev_one);
}
/*--------------------------------------------------------------------*/
