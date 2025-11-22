/*--------------------------------------------------------------------*/
/* job.c */
/* Author: Jongki Park, Kyoungsoo Park */
/*--------------------------------------------------------------------*/

#include "job.h"

extern struct job_manager *manager;
/*--------------------------------------------------------------------*/
void init_job_manager() {
    manager = (struct job_manager *)calloc(1, sizeof(struct job_manager));
    if(manager == NULL){
        fprintf(stderr, "[Error] job manager allocation failed\n");
        exit(EXIT_FAILURE);
    }

    manager->next_jid = 1;
}
/*--------------------------------------------------------------------*/
struct job *find_job_by_jid(int job_id) {
    int i;
    for(i = 0; i < MAX_JOBS; i++){
        if(manager->jobs[i] && manager->jobs[i]->job_id == job_id){
            return manager->jobs[i];
        }
    }
    return NULL;
}
/*--------------------------------------------------------------------*/
int remove_pid_from_job(struct job *job, pid_t pid) {
    int i;
    for(i = 0; i < job->n_pids; i++){
        if(job->pids[i] == pid){
            job->remaining_processes--;
            return 1;
        }
    }
    return 0;
}
/*--------------------------------------------------------------------*/
int delete_job(int job_id) {
    int i;
    struct job *j = find_job_by_jid(job_id);
    if(!j) return -1;

    for(i = 0; i < MAX_JOBS; i++){
        if(manager->jobs[i] == j){
            if(j->cmd_line_print) free(j->cmd_line_print);
            free(j);
            manager->jobs[i] = NULL;
            manager->n_jobs--;
            return 0;
        }
    }
    return -1;
}
/*--------------------------------------------------------------------*/
int add_job(struct job *new_job) {
    int i;
    if(manager->n_jobs >= MAX_JOBS) return -1;
    
    for(i = 0; i < MAX_JOBS; i++){
        if(manager->jobs[i] == NULL){
            new_job->job_id = manager->next_jid++;
            manager->jobs[i] = new_job;
            manager->n_jobs++;
            return new_job->job_id;
        }
    }
    return -1;
}