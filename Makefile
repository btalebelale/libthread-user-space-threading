CC      = gcc
VALGRIND_FLAG ?=
STACK_SIZE ?=
STACK_FLAG = $(if $(STACK_SIZE),-DSTACK_SIZE=$(STACK_SIZE),)
EXTRA_CFLAGS ?=
MULTICORE ?=

ifeq ($(MULTICORE),1)
  LIB_SRC_C       = src/thread_mn.c src/scheduler_mn.c src/thread_sync.c
  EXTRA_LDFLAGS   = -lpthread
  # USE_PREEMPTION dans CFLAGS permet Ã  l'asm de NE PAS poser l'alias
  # thread_yield -> thread_yield_asm (notre C dÃ©finit thread_yield).
  # Mais MULTICORE dÃ©sactive la prÃ©emption SIGALRM en C (la sched_stack
  # n'aime pas Ãªtre prÃ©emptÃ©e).
  MULTICORE_CFLAGS = -DMULTICORE -DUSE_PREEMPTION
  MULTICORE_ASFLAGS =
else
  LIB_SRC_C       = src/thread.c src/thread_sync.c
  EXTRA_LDFLAGS   =
  MULTICORE_CFLAGS =
  MULTICORE_ASFLAGS =
endif

CFLAGS  = -Wall -Wextra -Werror -g -Ofast -flto -fPIC -fvisibility=hidden -march=native -mtune=native -fno-plt -DNDEBUG -I./src $(VALGRIND_FLAG) $(STACK_FLAG) $(MULTICORE_CFLAGS) $(EXTRA_CFLAGS)

# SÃ©lection arch : asm x86_64 sinon fallback ucontext (Linux/POSIX).
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
  LIB_SRC_S = src/context_switch.S
  LIB_OBJ_ARCH = src/context_switch.o
else
  LIB_SRC_S = src/context_switch_portable.c
  LIB_OBJ_ARCH = src/context_switch_portable.o
endif
LIB_OBJ   = $(LIB_SRC_C:.c=.o) $(LIB_OBJ_ARCH)
LIB_NAME = libthread.so

# Bascule mono <-> MULTICORE : on Ã©value le variant Ã  la phase de parsing
# (avant calcul du DAG) pour pouvoir purger les .o stale et Ã©crire le stamp
# AVANT que make ne dÃ©cide quoi recompiler. Sans Ã§a, context_switch.o (compilÃ©
# sans MULTICORE_CFLAGS) garde l'alias thread_yield -> thread_yield_asm et
# entre en collision avec le thread_yield C de thread_mn.c.
BUILD_VARIANT := $(if $(MULTICORE_CFLAGS),mn,mono)
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
               tests/123-multicore-scalability \
               tests/72-multicore-preemption

ifeq ($(MULTICORE),1)
TEST_BINS = $(BASE_TEST_BINS) $(MN_TEST_BINS)
else
TEST_BINS = $(BASE_TEST_BINS) $(MONO_TEST_BINS)
endif

# Liste exhaustive pour le clean (indÃ©pendante du mode)
ALL_TEST_BINS = $(BASE_TEST_BINS) $(MONO_TEST_BINS) $(MN_TEST_BINS)

TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

all: $(LIB_NAME) $(TEST_BINS)

$(LIB_NAME): $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(EXTRA_LDFLAGS)

src/%.o: src/%.c src/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.S
	$(CC) $(MULTICORE_CFLAGS) $(MULTICORE_ASFLAGS) -c $< -o $@

NOFP_TEST_BINS = tests/61-mutex tests/62-mutex tests/63-mutex-equity \
                 tests/64-mutex-join tests/64-sem tests/65-sem-prodcons

$(NOFP_TEST_BINS): tests/%: tests/%.c $(LIB_NAME)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable $< -o $@ -L. -lthread

FP_WARN = -Wno-unused-but-set-variable -Wno-maybe-uninitialized

tests/%: tests/%.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) $(FP_WARN) -DFP $^ -o $@ $(EXTRA_LDFLAGS)

tests/23-create-many-once: tests/23-create-many-once.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) $(FP_WARN) -DFP $^ -o $@ $(EXTRA_LDFLAGS)

tests/71-preemption: tests/71-preemption.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) $(FP_WARN) -DFP -DUSE_PREEMPTION $^ -o $@ $(EXTRA_LDFLAGS)

# 72-multicore-preemption : statique pour forcer USE_PREEMPTION + LIBTHREAD_MN_PREEMPT
# même quand `make MULTICORE=1` ne définit pas LIBTHREAD_MN_PREEMPT par défaut.
tests/72-multicore-preemption: tests/72-multicore-preemption.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) $(FP_WARN) -DUSE_PREEMPTION -DLIBTHREAD_MN_PREEMPT $^ -o $@ $(EXTRA_LDFLAGS)

tests/131-priority: tests/131-priority.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) $(FP_WARN) -DFP -DUSE_PRIORITY $^ -o $@

pthreads: $(TEST_PTHREAD_BINS)

tests/%-pthread: tests/%.c
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DUSE_PTHREAD $< -o $@ -lpthread

check: all
	LD_LIBRARY_PATH=. ./scripts/run_tests.sh

valgrind: clean
	$(MAKE) all EXTRA_CFLAGS="-mno-avx512f"
	LD_LIBRARY_PATH=. ./scripts/run_valgrind.sh

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

# Mode multi-cÅ“ur AVEC prÃ©emption SIGALRM. Le tick Ã  50ms (cf
# thread_mn.c::PREEMPT_INTERVAL_US) Ã©vite une fenÃªtre de race subtile entre
# le store current=t/on_sched=1 et l'arrivÃ©e du code user â€” voir commentaire.
# Note : le test 03-equity peut Ã©chouer (mesure fairness coop pure ; la
# prÃ©emption ajoute du bruit au compte de yields, mÃªme rare).
multicore-preempt:
	$(MAKE) MULTICORE=1 EXTRA_CFLAGS="-DLIBTHREAD_MN_PREEMPT" all

check-mn:
	$(MAKE) MULTICORE=1 all
	LD_LIBRARY_PATH=. ./scripts/run_tests_mn.sh

check-mn-preempt:
	$(MAKE) MULTICORE=1 EXTRA_CFLAGS="-DLIBTHREAD_MN_PREEMPT" all
	LD_LIBRARY_PATH=. ./scripts/run_tests_mn_preempt.sh

# Lance uniquement les tests dédiés à la préemption.
# Les deux sont compilés en statique avec leurs propres -D, donc indépendants
# du mode global ; mais le Makefile clean les .o à la bascule MN ↔ mono, donc
# on compile en deux phases séparées :
#   - 71-preemption       : mode mono (USE_PREEMPTION)
#   - 72-multicore-preemption : mode MN  (LIBTHREAD_MN_PREEMPT)
check-preempt:
	$(MAKE) tests/71-preemption
	$(MAKE) MULTICORE=1 tests/72-multicore-preemption
	@echo "===== TEST 71 (mono + préemption) ====="
	./tests/71-preemption 8
	@echo "===== TEST 72 (MN + préemption) ====="
	./tests/72-multicore-preemption

.PHONY: all clean valgrind check pthreads graphs install pgo multicore multicore-preempt check-mn check-mn-preempt check-preempt