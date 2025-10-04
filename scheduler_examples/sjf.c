#include "sjf.h"
#include "msg.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>

static inline uint32_t remaining_ms(const pcb_t *p){
    return (p->time_ms > p->ellapsed_time_ms) ? (p->time_ms - p->ellapsed_time_ms) : 0u;
}

static pcb_t* dequeue_shortest_pcb(queue_t *rq){
    pcb_t *best = NULL, *p;
    queue_t tmp = {.head=NULL,.tail=NULL};
    while ((p = dequeue_pcb(rq)) != NULL){
        if (!best || remaining_ms(p) < remaining_ms(best) ||
            (remaining_ms(p) == remaining_ms(best) && p->pid < best->pid)){
            if (best) enqueue_pcb(&tmp, best);
            best = p;
            } else {
                enqueue_pcb(&tmp, p);
            }
    }
    while ((p = dequeue_pcb(&tmp)) != NULL) enqueue_pcb(rq, p);
    return best;
}

void sjf_scheduler(uint32_t current_time_ms, queue_t *rq, pcb_t **cpu_task){
    if (*cpu_task){
        pcb_t *p = *cpu_task;
        if (p->last_update_time_ms < current_time_ms){
            p->ellapsed_time_ms += TICKS_MS;
            p->last_update_time_ms = current_time_ms;
        }
        if (p->ellapsed_time_ms >= p->time_ms){
            msg_t msg = {.pid=p->pid, .request=PROCESS_REQUEST_DONE, .time_ms=current_time_ms};
            (void)!write(p->sockfd, &msg, sizeof(msg));
            free(p);
            *cpu_task = NULL;
        }
    }
    if (*cpu_task == NULL){
        *cpu_task = dequeue_shortest_pcb(rq);
        if (*cpu_task){
            (*cpu_task)->status = TASK_RUNNING;
			(*cpu_task)->last_update_time_ms = current_time_ms;
            if ((*cpu_task)->last_update_time_ms < current_time_ms)
                (*cpu_task)->last_update_time_ms = current_time_ms;
        }
    }
}