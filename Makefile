CC      = gcc
VALGRIND_FLAG ?=
STACK_SIZE ?=
STACK_FLAG = $(if $(STACK_SIZE),-DSTACK_SIZE=$(STACK_SIZE),)
EXTRA_CFLAGS ?=
MULTICORE ?=

# Détection de l'archi cible. Le hot path optimisé (asm context_switch,
# pthread_setaffinity_np, MAP_POPULATE/MAP_STACK) n'est valide que sur
# x86_64 + Linux + glibc — ailleurs on bascule sur le fallback ucontext
# défini dans src/ctx_ucontext.c. La macro PORT_FAST_PATH (cf.
# src/portable.h) reflète cette détection au niveau C.
#
# Override possible : `make PORT_FAST_PATH=0` force le fallback même sur
# x86_64+Linux (utile pour tester la portabilité depuis la machine de dev).
ifndef PORT_FAST_PATH
ARCH := $(shell uname -m)
KERNEL := $(shell uname -s)
ifeq ($(ARCH)$(KERNEL),x86_64Linux)
  PORT_FAST_PATH := 1
else
  PORT_FAST_PATH := 0
endif
endif

ifeq ($(PORT_FAST_PATH),1)
  LIB_SRC_S = src/context_switch.S
  LIB_SRC_FALLBACK_C =
  PORT_CFLAGS =
else
  LIB_SRC_S =
  LIB_SRC_FALLBACK_C = src/ctx_ucontext.c
  # _XOPEN_SOURCE=600 expose getcontext/makecontext/swapcontext (déprécié
  # POSIX 2008 mais toujours présent sur glibc et macOS).
  # PORT_FORCE_FALLBACK : signale au C qu'on veut le fallback même si
  # __x86_64__/__linux__ sont définis (= test du fallback sur machine fast).
  PORT_CFLAGS = -D_XOPEN_SOURCE=600 -DPORT_FORCE_FALLBACK
endif

ifeq ($(MULTICORE),1)
  LIB_SRC_C       = src/thread_mn.c src/scheduler_mn.c src/thread_sync.c $(LIB_SRC_FALLBACK_C)
  EXTRA_LDFLAGS   = -lpthread
  # USE_PREEMPTION dans CFLAGS permet à l'asm de NE PAS poser l'alias
  # thread_yield -> thread_yield_asm (notre C définit thread_yield).
  # LIBTHREAD_MN_PREEMPT activé par défaut : la préemption SIGALRM est
  # robuste grâce à l'invariant "SIGALRM masqué sur sched_stack". Pour
  # désactiver : make MULTICORE=1 EXTRA_CFLAGS=-DLIBTHREAD_MN_NO_PREEMPT.
  ifeq ($(filter -DLIBTHREAD_MN_NO_PREEMPT,$(EXTRA_CFLAGS)),)
    MULTICORE_CFLAGS = -DMULTICORE -DUSE_PREEMPTION -DLIBTHREAD_MN_PREEMPT
  else
    MULTICORE_CFLAGS = -DMULTICORE -DUSE_PREEMPTION
  endif
  MULTICORE_ASFLAGS =
else
  LIB_SRC_C       = src/thread.c src/thread_sync.c $(LIB_SRC_FALLBACK_C)
  EXTRA_LDFLAGS   =
  MULTICORE_CFLAGS =
  MULTICORE_ASFLAGS =
endif

# -march=native est gardé seulement sur fast path (x86_64). Sur fallback,
# on laisse le compilateur choisir un baseline portable.
ifeq ($(PORT_FAST_PATH),1)
  ARCH_CFLAGS = -march=native -mtune=native -fno-plt
else
  ARCH_CFLAGS =
endif

CFLAGS  = -Wall -Wextra -Werror -g -Ofast -flto -fPIC -fvisibility=hidden $(ARCH_CFLAGS) -DNDEBUG -I./src $(PORT_CFLAGS) $(VALGRIND_FLAG) $(STACK_FLAG) $(MULTICORE_CFLAGS) $(EXTRA_CFLAGS)

<<<<<<< Updated upstream
LIB_SRC_C = src/thread.c src/thread_sync.c
LIB_SRC_S = src/context_switch.S
LIB_OBJ   = $(LIB_SRC_C:.c=.o) $(LIB_SRC_S:.S=.o)
LIB_NAME = libthread.so

TEST_BINS = tests/01-main \
            tests/02-switch \
            tests/03-equity \
            tests/11-join \
            tests/12-join-main \
            tests/13-join-switch \
            tests/21-create-many \
            tests/22-create-many-recursive \
            tests/23-create-many-once \
            tests/31-switch-many \
            tests/32-switch-many-join \
            tests/33-switch-many-cascade \
            tests/51-fibonacci \
            tests/61-mutex \
            tests/62-mutex \
            tests/63-mutex-equity \
            tests/64-mutex-join \
			tests/64-sem \
			tests/65-sem-prodcons \
            tests/71-preemption  \
            tests/81-deadlock \
			tests/124-signals \
			tests/131-priority

=======
LIB_OBJ   = $(LIB_SRC_C:.c=.o) $(LIB_SRC_S:.S=.o)
LIB_NAME = libthread.so

# Bascule mono <-> MULTICORE : on évalue le variant à la phase de parsing
# (avant calcul du DAG) pour pouvoir purger les .o stale et écrire le stamp
# AVANT que make ne décide quoi recompiler. Sans ça, context_switch.o (compilé
# sans MULTICORE_CFLAGS) garde l'alias thread_yield -> thread_yield_asm et
# entre en collision avec le thread_yield C de thread_mn.c.
BUILD_VARIANT := $(if $(MULTICORE_CFLAGS),mn,mono)-$(if $(filter 1,$(PORT_FAST_PATH)),fast,fallback)
BUILD_STAMP   := src/.build-variant
PREV_VARIANT  := $(shell cat $(BUILD_STAMP) 2>/dev/null)
ifneq ($(PREV_VARIANT),$(BUILD_VARIANT))
$(shell rm -f src/*.o $(LIB_NAME); mkdir -p src; echo $(BUILD_VARIANT) > $(BUILD_STAMP))
endif

BASE_TEST_BINS = tests/01-main \
                 tests/02-switch \
                 tests/03-equity \
                 tests/11-join \
                 tests/12-join-main \
                 tests/13-join-switch \
                 tests/21-create-many \
                 tests/22-create-many-recursive \
                 tests/23-create-many-once \
                 tests/31-switch-many \
                 tests/32-switch-many-join \
                 tests/33-switch-many-cascade \
                 tests/51-fibonacci \
                 tests/61-mutex \
                 tests/62-mutex \
                 tests/63-mutex-equity \
                 tests/64-mutex-join \
                 tests/64-sem \
                 tests/65-sem-prodcons \
                 tests/71-preemption \
                 tests/81-deadlock \
                 tests/124-signals

MONO_TEST_BINS = tests/131-priority

MN_TEST_BINS = tests/101-multicore-counter \
               tests/102-multicore-concurrent-work \
               tests/103-multicore-sem-prodcons \
               tests/104-multicore-mutex-ordering \
               tests/105-multicore-join-values \
               tests/106-multicore-cascade-mutex \
               tests/107-multicore-yield-fairness \
               tests/111-multicore-stress-create-join \
               tests/112-multicore-stress-mutex \
               tests/113-multicore-memory-stress \
               tests/121-multicore-speedup \
               tests/122-multicore-lock-overhead \
               tests/123-multicore-scalability

ifeq ($(MULTICORE),1)
TEST_BINS = $(BASE_TEST_BINS) $(MN_TEST_BINS)
else
TEST_BINS = $(BASE_TEST_BINS) $(MONO_TEST_BINS)
endif

# Liste exhaustive pour le clean (indépendante du mode)
ALL_TEST_BINS = $(BASE_TEST_BINS) $(MONO_TEST_BINS) $(MN_TEST_BINS)
>>>>>>> Stashed changes

TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

all: $(LIB_NAME) $(TEST_BINS)

$(LIB_NAME): $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(EXTRA_LDFLAGS)

src/%.o: src/%.c src/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.S
	$(CC) $(MULTICORE_CFLAGS) $(MULTICORE_ASFLAGS) $(STACK_FLAG) -c $< -o $@

tests/%: tests/%.c $(LIB_NAME)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable $< -o $@ -L. -lthread

# 51-fibonacci compile la lib en statique avec -DFP activé : la déduplication
# (dedup.h) memoïse les threads par (func, arg). C'est sémantiquement correct
# uniquement pour des workloads purement fonctionnels (fibonacci récursif), pas
# pour les tests où chaque thread doit produire un effet de bord distinct (61-mutex).
# C'est pour ça que FP n'est PAS dans CFLAGS global.
tests/51-fibonacci: tests/51-fibonacci.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DFP $^ -o $@ $(EXTRA_LDFLAGS)

tests/23-create-many-once: tests/23-create-many-once.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable $^ -o $@ $(EXTRA_LDFLAGS)

tests/71-preemption: tests/71-preemption.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DUSE_PREEMPTION $^ -o $@ $(EXTRA_LDFLAGS)

tests/131-priority: tests/131-priority.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DUSE_PRIORITY $^ -o $@

pthreads: $(TEST_PTHREAD_BINS)

tests/%-pthread: tests/%.c
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DUSE_PTHREAD $< -o $@ -lpthread

check:
	$(MAKE) clean
	$(MAKE) all
	LD_LIBRARY_PATH=. ./scripts/run_tests.sh
	$(MAKE) clean
	$(MAKE) MULTICORE=1 all
	LD_LIBRARY_PATH=. ./scripts/run_tests_mn.sh

valgrind:
	$(MAKE) clean
	$(MAKE) all EXTRA_CFLAGS="-mno-avx512f"
	LD_LIBRARY_PATH=. ./scripts/run_valgrind.sh
	$(MAKE) clean
	$(MAKE) MULTICORE=1 all EXTRA_CFLAGS="-mno-avx512f"
	LD_LIBRARY_PATH=. ./scripts/run_valgrind_mn.sh

graphs: all pthreads
	python3 scripts/generate_graphs.py

install: all pthreads
	mkdir -p install/lib install/bin
	cp $(LIB_NAME) install/lib/
	cp $(TEST_BINS) install/bin/
	cp $(TEST_PTHREAD_BINS) install/bin/

pgo: clean
	$(MAKE) all EXTRA_CFLAGS="-fprofile-generate"
	LD_LIBRARY_PATH=. ./scripts/run_tests.sh
	$(MAKE) clean
	$(MAKE) all EXTRA_CFLAGS="-fprofile-use -fprofile-correction"

clean:
	rm -f src/*.o $(LIB_NAME) $(BUILD_STAMP)
	rm -f $(ALL_TEST_BINS) $(addsuffix -pthread,$(ALL_TEST_BINS))
	rm -rf install/lib/* install/bin/*
	rm -rf tests/*.dSYM
	rm -f src/*.gcda src/*.gcno src/*.gcov
	rm -f tests/*.gcda tests/*.gcno tests/*.gcov
	rm -f graphs/*.png
	rm -f perf.data perf.data.old
	rm -f core core.*

multicore:
	$(MAKE) MULTICORE=1 all

check-mn:
	$(MAKE) MULTICORE=1 all
	LD_LIBRARY_PATH=. ./scripts/run_tests_mn.sh

.PHONY: all clean valgrind check pthreads graphs install pgo multicore check-mn
