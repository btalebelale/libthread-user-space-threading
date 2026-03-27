//On propose ici une implémentation de la pile de thread utilisant des tableaux.

#include <stdlib.h>
#include "thread_internal.h"
#include "pool.h"
#include "../debug/log_sys.h"

struct pool {
  thread_m **data;
  int cap; //Capacité maximale initialisé par l'utilisateur
  int size;
};

pool *pool_init(int cap){
  if (cap <= 0) {
    LOG_E("cap invalide (%d)", cap);
    return NULL;
  }
  pool *p = malloc(sizeof(*p));

  if(p == NULL){
    LOG_E("Echec de l'allocation d'une pool");
    return NULL;
  }
  p->data = malloc(sizeof(thread_m *) * cap);
  if(p->data == NULL){
    free(p);
    LOG_E("Echec de l'allocation de la data d'une pool");
    return NULL;
  }
  p->cap  = cap;
  p->size = 0;
  return p;
}

void pool_free(pool *p){
  if(p == NULL){
    LOG_E("p est NULL");
    return;
  }
  // La pool ne possède pas les objets thread_m, on libère uniquement le tableau et la struct.
  if(p -> data == NULL){
    LOG_E("p -> data est NULL");
    return;
  }
  free(p->data);
  free(p);
}

int pool_is_empty(pool *p)
{
  if(p == NULL){
    LOG_E("p est NULL");
    return -1;
  }
  return p->size == 0;
}

int pool_size(pool *p){
  if(p == NULL){
    LOG_E("p est NULL");
    return -1;
  }
  return p->size;
}

thread_m *pool_get_at(pool *p, int i){
  if(p == NULL){
    LOG_E("p est NULL");
    return NULL;
  }
  if(i < 0 || i >= p->size) {
    LOG_E("Accès à l'index %d invalide (size=%d)", i, p->size);
    return NULL;
  }
  if(p -> data == NULL){
    LOG_E("p -> data est NULL");
    return NULL;
  }
   if(p -> data[i] == NULL){
     LOG_E("p -> data[%d] est NULL",i);
    return NULL;
  }
  return p->data[i];
}

thread_m *pool_get_first(pool *p){
  if(p == NULL){
    LOG_E("p est NULL");
    return NULL;
  }
  return pool_get_at(p, 0);
}

thread_m *pool_get_last(pool *p){
  if(p == NULL){
    LOG_E("p est NULL");
    return NULL;
  }
  return pool_get_at(p, p->size - 1);
}

int pool_put_at(pool *p, thread_m *th, int idx){
  if(p == NULL){
    LOG_E("p est NULL");
    return -1;
  }
  if(th == NULL){
    LOG_E("th est NULL");
    return -1;
  }
  if(idx < 0 || idx > p->size){
    LOG_E("Index %d invalide (size=%d)", idx, p->size);
    return -1;
  }
  /*Remarque ici on choisie de ne pas augmenter la taille du
    tableau dans le cas où pool -> size == pool -> cap. Ce choix ce justifie car
    normalement la pool sera toujours assez grandes pour acceullir tout les threads
    et que dans le cas contraires cela résulte surement d'une erreur*/
  if(p->size >= p->cap){
    LOG_E("Pool pleine (cap=%d)", p->cap);
    return -1;
  }
   if(p -> data == NULL){
     LOG_E("p -> data est NULL");
     return -1;
  }
  for(int i = p->size; i > idx; i--)
    p->data[i] = p->data[i - 1];
  p->data[idx] = th;
  p->size++;
  return 0;
}

int pool_put_first(pool *p, thread_m *th){
  if(p == NULL){
    LOG_E("p est NULL");
    return -1;
  }
  if(th == NULL){
    LOG_E("th est NULL");
    return -1;
  }
  return pool_put_at(p, th, 0);
}

int pool_put_last(pool *p, thread_m *th){
  if(p == NULL){
    LOG_E("p est NULL");
    return -1;
  }
  if(th == NULL){
    LOG_E("th est NULL");
    return -1;
  }
  return pool_put_at(p, th, p->size);
}

thread_m *pool_remove_at(pool *p, int idx){
  if(p == NULL){
    LOG_E("p est NULL");
    return NULL;
  }
  if(idx < 0 || idx >= p->size){
    LOG_E("Index %d invalide (size=%d)", idx, p->size);
    return NULL;
  }
  thread_m *th = p->data[idx];
  p->size--;
 if(p -> data == NULL){
     LOG_E("p -> data est NULL");
    return NULL;
  }
  for(int i = idx; i < p->size; i++) //Ne pas oublier de free les thread déjà allouer !
    p->data[i] = p->data[i + 1];
  return th;
}

thread_m *pool_remove_first(pool *p){
  if(p == NULL){
    LOG_E("p est NULL");
    return NULL;
  }
  return pool_remove_at(p, 0);
}

thread_m *pool_remove_last(pool *p){
  if(p == NULL){
    LOG_E("p est NULL");
    return NULL;
  }
  return pool_remove_at(p, p->size - 1);
}
