/* job.h - a job in the queue */

#ifndef job_h
#define job_h

#include <time.h>

typedef struct job *job;

struct job {
    job prev, next; /* linked list of jobs */
    unsigned long long int id;
    unsigned int pri;
    int body_size;
    time_t deadline;
    char body[];
};

job allocate_job(int body_size);
job make_job(unsigned int pri, int body_size);

int job_cmp(job a, job b);

job job_copy(job j);

int job_list_any_p(job head);
job job_remove(job j);
void job_insert(job head, job j);

#endif /*job_h*/
