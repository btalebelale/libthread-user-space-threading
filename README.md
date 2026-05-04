# libthread

Bibliothèque de threads en espace utilisateur (modèle 1:N coopératif, avec options multicoeur et préemption).

## Compilation

```bash
make          # version monocoeur (défaut)
make MULTICORE=1          # version multicoeur M:N
make MULTICORE=1 EXTRA_CFLAGS=-DLIBTHREAD_MN_PREEMPT  # multicoeur + préemption
```

Les options avancées se compilent via des flags :

| Flag                        | Effet                              |
|-----------------------------|------------------------------------|
| `MULTICORE=1`               | Ordonnanceur M:N (work-stealing)   |
| `-DUSE_PREEMPTION`          | Préemption par SIGALRM (monocoeur) |
| `-DLIBTHREAD_MN_PREEMPT`    | Préemption en mode multicoeur      |
| `-DUSE_PRIORITY`            | Ordonnancement par priorité        |

## Lancer les tests

```bash
make check                  # tests monocoeur
make check-preempt          # tests avec préemption
make check-mn               # tests multicoeur
make check-mn-preempt       # tests multicoeur + préemption
```

Les tests sont dans `tests/`. Chaque binaire s'exécute directement :

```bash
LD_LIBRARY_PATH=. ./tests/61-mutex 100
```

## Valgrind

```bash
make valgrind
```

## Graphes de performance

```bash
make graphs       # génère les PNG dans graphs/
```


## Micro-benchmarks

```bash
./scripts/bench_functions.sh
```

Mesure le coût de chaque fonction de l'API (thread_create, thread_yield, mutex, sémaphore...).

---

## Version de base

La version de base (premiers objectifs, sans options avancées) se trouve dans `base_version/`.

```bash
cd base_version
make
make check
```

Elle est compilée et testée de la même façon que la version principale, sans les flags `MULTICORE`, `USE_PREEMPTION` ni `USE_PRIORITY`.
