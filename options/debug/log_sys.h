#ifndef LOG_H
#define LOG_H
#ifdef DLOG 
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>

// Les différents niveau de log. Cela va de debug nous donne le plus de détails possible sur l'exécution, utile pour le debogage. Fatal lui est appelé que pendant des exécution fatal 
typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL
}LogLevel;


static const char *level[] = { "DEBUG", "INFO", "WARNING", "ERROR", "FATAL" };


/* lvl : niveau d'eerreur
   file : le fichier qu'on est en train de tracker
   line : le numéro de ligne ou en est dasn l'execution
   func : le nom de la fonction qui s'execute
   fmt : le message à afficher, on met fmt et non msg, car le message n'est pas
   complet, il peut attendre des %s, ou des %d, c'est plus un 'format', donc
   on met fmt. bref, en plus c'est comme ça qui font dans la doc de printf
   ... : des variables en plus, oui oui notre fonction est variadiques, la classe
   hein !?
   Pour la signature : "static" en gros sans ça sa compile pas, j'ai des
   définitions multiples, d'où le static. Une seule définition confiné à chaque
   fichier. Pour le inline, c'est un conseil de claude, à étudier plus tard...
 */
static inline void log_write(LogLevel lvl, const char *file, int line, const char *func, const char *fmt, ...) {
  struct timespec ts; //structure pour garder le temps, à la ns près
  clock_gettime(CLOCK_REALTIME, &ts); /*met le temps dans ts, beaucoup plus
					pécis que time(), utiliser CLOCK_MONOTONIC
					pour ne pas être biasé */
  va_list args; //Curseur qui va pointer sur la liste des arguments variadiques
  va_start(args,fmt); //On met fmt, parce que c'est le dernier argument avant ...
  fprintf(stderr, "[%ld.%09ld] [%-5s] %s:%d (%s) -",ts.tv_sec,ts.tv_nsec, level[lvl],file,line,func);
  vfprintf(stderr, fmt, args); //version de fprintf qui prend des des va_list
  fprintf(stderr, "\n");
  va_end(args);
  if(lvl == LOG_FATAL) abort();
}

//Les macros sont assez explicites je trouve, pour plus d'explication me (Yani) demander.

#define LOG_D(fmt, ...) log_write(LOG_DEBUG,   __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__) 
#define LOG_I(fmt, ...) log_write(LOG_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) log_write(LOG_WARNING, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) log_write(LOG_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_F(fmt, ...) log_write(LOG_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#else
#define LOG_D(fmt, ...) ((void)0)
#define LOG_I(fmt, ...) ((void)0)
#define LOG_W(fmt, ...) ((void)0)
#define LOG_E(fmt, ...) ((void)0)
#define LOG_F(fmt, ...) ((void)0)
#endif // DLOG
#endif // LOG_H

