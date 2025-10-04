#include "rr.h"
#include "msg.h"      // TICKS_MS, msg_t, PROCESS_REQUEST_DONE
#include <unistd.h>   // write
#include <stdlib.h>   // free

#ifndef RR_QUANTUM_MS
#define RR_QUANTUM_MS 500u   // 500 ms
#endif

// Liberta a tarefa atual se já foi marcada como STOPPED (o app já recebeu DONE)
static void maybe_free_finished(pcb_t **cpu_task){
    if (*cpu_task && (*cpu_task)->status == TASK_STOPPED){
        free(*cpu_task);
        *cpu_task = NULL;
    }
}

static void send_done_to_app(pcb_t *p, uint32_t now_ms){
    msg_t msg = {
        .pid = p->pid,
        .request = PROCESS_REQUEST_DONE,
        .time_ms = now_ms
    };
    // envia DONE à app; se falhar, apenas regista pelo stderr (não paramos o escalonador)
    (void)!write(p->sockfd, &msg, sizeof(msg));
}

void rr_scheduler(uint32_t now_ms, queue_t *rq, pcb_t **cpu_task){
    // Se a tarefa atual já estava marcada como STOPPED, liberta-a
    maybe_free_finished(cpu_task);

    // Se há tarefa em CPU, desconta tempo do tick atual
    if (*cpu_task){
        pcb_t *p = *cpu_task;

        // Evita descontar duas vezes no mesmo instante lógico
        if (p->last_update_time_ms < now_ms){
            // desconta o tempo de CPU consumido neste tick
            if (p->time_ms > TICKS_MS) {
                p->time_ms -= TICKS_MS;
            } else {
                p->time_ms = 0;
            }
            p->ellapsed_time_ms += TICKS_MS;
            p->last_update_time_ms = now_ms;
        }

        // Se terminou o CPU, envia DONE e marca como STOPPED
        if (p->time_ms == 0){
            p->status = TASK_STOPPED;
            send_done_to_app(p, now_ms);
            // Na próxima invocação, maybe_free_finished() liberta o PCB
            // e o CPU ficará livre para escolher nova tarefa.
        }
    }

    // Preempção por quantum (só se ainda não terminou)
    if (*cpu_task && (*cpu_task)->status == TASK_RUNNING){
        uint32_t ran = now_ms - (*cpu_task)->slice_start_ms;
        if (ran >= RR_QUANTUM_MS){

            enqueue_pcb(rq, *cpu_task);
            *cpu_task = NULL;
        }
    }

    // Se o CPU está livre, escolhe o próximo da ready queue
    if (*cpu_task == NULL){
        *cpu_task = dequeue_pcb(rq);
        if (*cpu_task){
            (*cpu_task)->status = TASK_RUNNING;
			(*cpu_task)->slice_start_ms = now_ms;
			(*cpu_task)->last_update_time_ms = now_ms;
            (*cpu_task)->slice_start_ms    = now_ms;   // início da nova fatia
            // alinhar relógio interno para não descontar duas vezes neste mesmo now_ms
            if ((*cpu_task)->last_update_time_ms < now_ms){
                (*cpu_task)->last_update_time_ms = now_ms;
            }
        }
    }
}

