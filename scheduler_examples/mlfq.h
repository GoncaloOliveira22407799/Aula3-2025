#ifndef MLFQ_H
#define MLFQ_H

#include <stdint.h>
#include "queue.h"

// Escalonador MLFQ com 3 níveis e RR por nível.
// - Entra sempre no nível 0.
// - Quantum por nível: L0=500ms, L1=1000ms, L2=2000ms.
// - Aging: se espera >= 3000ms na fila, sobe um nível.
// - Usa a ready_queue "normal" (incoming_ready) apenas como entrada:
//   o mlfq drena dessa fila para as filas internas por nível.
void mlfq_scheduler(uint32_t now_ms, queue_t *incoming_ready, pcb_t **cpu_task);

#endif