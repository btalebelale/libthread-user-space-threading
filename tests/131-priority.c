#include "thread.h"
#include <stdio.h>                                                                                                                    
#include <stdlib.h>
#include <assert.h>

  #define N 4                                                                                                                           
  
  static int execution_order[N];                                                                                                        
  static int exec_idx = 0;

  static void *worker(void *arg)                                                                                                        
  {
      int id = (int)(long)arg;                                                                                                          
      execution_order[exec_idx++] = id;
      return NULL;                                                                                                                      
  }
                                                                                                                                        
int main(void)  
{
    thread_t t[N];
    for (int i = 0; i < N; i++)
        thread_create(&t[i], worker, (void *)(long)i);
                                                                                                                                      
    /* t[0]=1  t[1]=7  t[2]=5  t[3]=3 */                                                                                              
    thread_setpriority(t[0], 1);                                                                                                      
    thread_setpriority(t[1], 7);                                                                                                      
    thread_setpriority(t[2], 5);                                                                                                      
    thread_setpriority(t[3], 3);
                                                                                                                                      
    /* main cède la main => le scheduler choisit par priorité */                                                                       
    thread_yield();
                                                                                                                                      
    for (int i = 0; i < N; i++)
        thread_join(t[i], NULL);                                                                                                      
                
    printf("Execution order:");
    for (int i = 0; i < exec_idx; i++)
        printf(" t[%d](prio %d)", execution_order[i],                                                                                 
               (int[]){1, 7, 5, 3}[execution_order[i]]);
    printf("\n");                                                                                                                     
                
    /* ordre attendu : t[1](7), t[2](5), t[3](3), t[0](1) */                                                                          
    assert(execution_order[0] == 1);
    assert(execution_order[1] == 2);                                                                                                  
    assert(execution_order[2] == 3);                                                                                                  
    assert(execution_order[3] == 0);
                                                                                                                                      
    printf("[OK] priority scheduling\n");                                                                                             
    return 0;
}