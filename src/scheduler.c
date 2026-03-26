#include "scheduler.h"

#include <stdlib.h>

STAILQ_HEAD(thread_queue, thread);

static struct thread_queue ready_queue;

thread_m *current = NULL;

void sched_init() {
    STAILQ_INIT(&ready_queue);
}

void sched_enqueue(thread_m *t){
    STAILQ_INSERT_TAIL(&ready_queue, t, link);
}

thread_m *sched_dequeue(void){
    if(STAILQ_EMPTY(&ready_queue)){
        return NULL;
    } 
    thread_m *first = STAILQ_FIRST(&ready_queue);
    STAILQ_REMOVE_HEAD(&ready_queue, link);
    return first;
}

int sched_empty(void) {
    return STAILQ_EMPTY(&ready_queue);
}


