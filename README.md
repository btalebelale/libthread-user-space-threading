# libthread — User-Space Thread Library

A POSIX-like thread library implemented entirely in user space, built as a systems programming project at ENSEIRB-MATMECA.

## Features

| Mode | Description |
|---|---|
| **1:N cooperative** | Single kernel thread, cooperative scheduling with `thread_yield` |
| **M:N multicore** | Multiple kernel threads with work-stealing scheduler |
| **Preemption** | Timer-based preemption via `SIGALRM` |
| **Priority** | Fixed-priority scheduling |

## API

```c
thread_t thread_self();
int thread_create(thread_t *newthread, void *(*func)(void *), void *funcarg);
int thread_yield();
int thread_join(thread_t thread, void **retval);
int thread_exit(void *retval);

// Mutex
int thread_mutex_init(thread_mutex_t *mutex);
int thread_mutex_destroy(thread_mutex_t *mutex);
int thread_mutex_lock(thread_mutex_t *mutex);
int thread_mutex_unlock(thread_mutex_t *mutex);

// Semaphore
int thread_sem_init(thread_sem_t *sem, unsigned int value);
int thread_sem_wait(thread_sem_t *sem);
int thread_sem_post(thread_sem_t *sem);
int thread_sem_destroy(thread_sem_t *sem);
```

## Build

```bash
make                                              # 1:N cooperative (default)
make MULTICORE=1                                  # M:N multicore
make MULTICORE=1 EXTRA_CFLAGS=-DLIBTHREAD_MN_PREEMPT  # M:N + preemption
```

| Flag | Effect |
|---|---|
| `MULTICORE=1` | M:N scheduler with work-stealing |
| `-DUSE_PREEMPTION` | Preemption via SIGALRM (single-core) |
| `-DLIBTHREAD_MN_PREEMPT` | Preemption in multicore mode |
| `-DUSE_PRIORITY` | Fixed-priority scheduling |

## Tests

```bash
make check              # single-core tests
make check-preempt      # single-core + preemption
make check-mn           # multicore tests
make check-mn-preempt   # multicore + preemption
```

Run a single test directly:

```bash
LD_LIBRARY_PATH=. ./tests/61-mutex 100
```

## Benchmarks & Profiling

```bash
./scripts/bench_functions.sh   # cost of each API call
make graphs                    # generate performance PNGs in graphs/
make valgrind                  # memory check
```

## Base Version

The baseline implementation (first milestones, no advanced options) lives in `base_version/`:

```bash
cd base_version && make && make check
```

## Academic Context

Systems project (EIN8) — ENSEIRB-MATMECA, 2025–2026.
