#ifndef RR_H
#define RR_H
#include <stdint.h>
#include "queue.h"
void rr_scheduler(uint32_t now_ms, queue_t *rq, pcb_t **cpu_task);
#endif


#ifndef SCHEDULER_EXAMPLES_RR_H
#define SCHEDULER_EXAMPLES_RR_H

#endif //SCHEDULER_EXAMPLES_RR_H