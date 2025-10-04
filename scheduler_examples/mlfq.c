#include "mlfq.h"
#include "msg.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MLFQ_LEVELS 3
static const uint32_t QUANTUM_MS[MLFQ_LEVELS] = { 500u, 1000u, 2000u };  // L0, L1, L2
#define AGING_MS 3000u   // espera >= 3s na fila -> promoção de 1 nível

//Filas por nível
static int inited = 0;
static queue_t qlevel[MLFQ_LEVELS];

static inline uint32_t remaining_ms(const pcb_t *p) {
    return (p->time_ms > p->ellapsed_time_ms)
           ? (p->time_ms - p->ellapsed_time_ms) : 0u;
}

static void init_once(void) {
    if (!inited) {
        for (int i = 0; i < MLFQ_LEVELS; ++i) {
            qlevel[i].head = qlevel[i].tail = NULL;
        }
        inited = 1;
    }
}

static void siphon_from_incoming(queue_t *incoming, uint32_t now_ms) {
    pcb_t *p;
    while ((p = dequeue_pcb(incoming)) != NULL) {
        p->level = 0;                 // começa pelo nível 0
        p->wait_since_ms = now_ms;    // começa a "esperar" no nível 0
        enqueue_pcb(&qlevel[0], p);
    }
}

// processos que esperem muito na fila sobem de nível
static void apply_aging(uint32_t now_ms) {
    for (int lvl = 1; lvl < MLFQ_LEVELS; ++lvl) {   // promove somente para nível 1 e 2 no máximo
        queue_t tmp = {.head=NULL,.tail=NULL};
        queue_elem_t *e = qlevel[lvl].head;
        while (e) {
            pcb_t *p = e->pcb;
            queue_elem_t *next = e->next;

            remove_queue_elem(&qlevel[lvl], e);

            if (now_ms - p->wait_since_ms >= AGING_MS && lvl > 0) {
                p->level = (uint8_t)(lvl - 1);
                p->wait_since_ms = now_ms;
                enqueue_pcb(&qlevel[p->level], p);
            } else {
                enqueue_pcb(&tmp, p);
            }
            e = next;
        }
        qlevel[lvl] = tmp;
    }
}

// devolve o nível mais prioritário que tenha trabalho
static int pick_ready_level(void) {
    for (int i = 0; i < MLFQ_LEVELS; ++i)
        if (qlevel[i].head) return i;
    return -1;
}

void mlfq_scheduler(uint32_t now_ms, queue_t *incoming_ready, pcb_t **cpu_task) {
    init_once();

    // drena os novos prontos para nível 0 e aplica aging
    siphon_from_incoming(incoming_ready, now_ms);
    apply_aging(now_ms);

    //se há tarefa no CPU, avança 1 tick
    if (*cpu_task) {
        pcb_t *p = *cpu_task;

        if (p->last_update_time_ms < now_ms) {
            p->ellapsed_time_ms += TICKS_MS;
            p->last_update_time_ms = now_ms;
        }

        // verifica se acabou
        if (p->ellapsed_time_ms >= p->time_ms) {
            msg_t msg = {.pid=p->pid, .request=PROCESS_REQUEST_DONE, .time_ms=now_ms};
            (void)!write(p->sockfd, &msg, sizeof(msg));
            free(p);
            *cpu_task = NULL;
        } else {
            uint8_t lvl = (p->level < MLFQ_LEVELS) ? p->level : (MLFQ_LEVELS-1);
            uint32_t ran_in_slice = now_ms - p->slice_start_ms;
            if (ran_in_slice >= QUANTUM_MS[lvl]) {
                //desce 1 nível (até ao 2) e volta à fila
                if (p->level < MLFQ_LEVELS - 1) p->level++;
                p->wait_since_ms = now_ms;
                enqueue_pcb(&qlevel[p->level], p);
                *cpu_task = NULL;
            }
        }
    }

    //Se o CPU estiver livre, escolhe do nível mais alto disponível
    if (*cpu_task == NULL) {
        int lvl = pick_ready_level();
        if (lvl >= 0) {
            *cpu_task = dequeue_pcb(&qlevel[lvl]);
            if (*cpu_task) {
                (*cpu_task)->status = TASK_RUNNING;
                (*cpu_task)->slice_start_ms = now_ms;
                (*cpu_task)->wait_since_ms = now_ms;  // já não está à espera
                if ((*cpu_task)->last_update_time_ms < now_ms)
                    (*cpu_task)->last_update_time_ms = now_ms;
            }
        }
    }
}
