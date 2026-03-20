#include "thread.h"

thread_t thread_self(void){
    return (thread_t)0;
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg){
    (void)newthread; (void)func; 
    (void)funcarg;
    return 0;
}

int thread_yield(void){
    return 0;
}

int thread_join(thread_t thread, void **retval){
    (void)thread; (void)retval;
    return 0;
}

void thread_exit(void *retval) {
    (void)retval;
    
    while (1) {
    }
}

int thread_mutex_init(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

int thread_mutex_lock(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

int thread_mutex_unlock(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}