#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h> /* ne compile pas avec -std=c89 ou -std=c99 */
#include <unistd.h>
#ifndef NCTX
#define NCTX 2
#endif

ucontext_t uc[NCTX];
int current = 0;
int next = 1;
ucontext_t uc_main;
void func(int numero)
{
  printf("j'affiche le numéro %d\n", numero);
  if(current < NCTX - 1)
    swapcontext(&uc[current++],&uc[next++]);
  printf("je termine avec le numéro %d\n",numero);
  current--;
  next--;
  if(current >= 0)
    swapcontext(&uc[next],&uc[current]);
  else
    swapcontext(&uc[next],&uc_main);
}

int main() {
  for(int i = 0; i < NCTX; i++){
    getcontext(&(uc[i]));
    uc[i].uc_stack.ss_size = 64*1024;
    uc[i].uc_stack.ss_sp = malloc(uc[i].uc_stack.ss_size);
    if(i == NCTX - 1)
      uc[i].uc_link = &uc_main;
    else
      uc[i].uc_link = NULL;
    makecontext(&(uc[i]), (void (*)(void)) func, 1, 34 + i);
  }
  uc[0].uc_link = &uc_main;
  printf("je suis dans le main 1\n");
  swapcontext(&uc_main, uc);
  printf("je suis dans le main 2\n");
  for(int i = 0; i < NCTX; i++){
    free(uc[i].uc_stack.ss_sp);
  }
  return 0;
}
