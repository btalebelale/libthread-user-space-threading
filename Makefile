CC      = gcc
VALGRIND_FLAG ?=
STACK_SIZE ?=
STACK_FLAG = $(if $(STACK_SIZE),-DSTACK_SIZE=$(STACK_SIZE),)
EXTRA_CFLAGS ?=
MULTICORE ?=

ifeq ($(MULTICORE),1)
  LIB_SRC_C       = src/thread_mn.c src/scheduler_mn.c src/thread_sync.c
  EXTRA_LDFLAGS   = -lpthread
  # USE_PREEMPTION dans CFLAGS permet à l'asm de NE PAS poser l'alias
  # thread_yield -> thread_yield_asm (notre C définit thread_yield).
  # Mais MULTICORE désactive la préemption SIGALRM en C (la sched_stack
  # n'aime pas être préemptée).
  MULTICORE_CFLAGS = -DMULTICORE -DUSE_PREEMPTION
  MULTICORE_ASFLAGS =
else
  LIB_SRC_C       = src/thread.c src/thread_sync.c
  EXTRA_LDFLAGS   =
  MULTICORE_CFLAGS =
  MULTICORE_ASFLAGS =
endif

CFLAGS  = -Wall -Wextra -Werror -g -Ofast -flto -fPIC -fvisibility=hidden -march=native -mtune=native -fno-plt -DNDEBUG -I./src $(VALGRIND_FLAG) $(STACK_FLAG) $(MULTICORE_CFLAGS) $(EXTRA_CFLAGS)

LIB_SRC_S = src/context_switch.S
LIB_OBJ   = $(LIB_SRC_C:.c=.o) $(LIB_SRC_S:.S=.o)
LIB_NAME = libthread.so

# Bascule mono <-> MULTICORE : on évalue le variant à la phase de parsing
# (avant calcul du DAG) pour pouvoir purger les .o stale et écrire le stamp
# AVANT que make ne décide quoi recompiler. Sans ça, context_switch.o (compilé
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
               tests/123-multicore-scalability

ifeq ($(MULTICORE),1)
TEST_BINS = $(BASE_TEST_BINS) $(MN_TEST_BINS)
else
TEST_BINS = $(BASE_TEST_BINS) $(MONO_TEST_BINS)
endif

# Liste exhaustive pour le clean (indépendante du mode)
ALL_TEST_BINS = $(BASE_TEST_BINS) $(MONO_TEST_BINS) $(MN_TEST_BINS)

TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

all: $(LIB_NAME) $(TEST_BINS)

$(LIB_NAME): $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(EXTRA_LDFLAGS)

src/%.o: src/%.c src/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

src/%.o: src/%.S
	$(CC) $(MULTICORE_CFLAGS) $(MULTICORE_ASFLAGS) -c $< -o $@

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

check-mn:
	$(MAKE) MULTICORE=1 all
	LD_LIBRARY_PATH=. ./scripts/run_tests_mn.sh

.PHONY: all clean valgrind check pthreads graphs install pgo multicore check-mn
