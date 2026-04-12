CC      = gcc
SCHED_CFLAGS_fifo = -DSCHED_FIFO
SCHED_CFLAGS_lifo = -DSCHED_LIFO
SCHED_CFLAGS_hybrid =
VALGRIND_FLAG ?= -DNVALGRIND
STACK_SIZE ?=
STACK_FLAG = $(if $(STACK_SIZE),-DSTACK_SIZE=$(STACK_SIZE),)
CFLAGS  = -Wall -Wextra -Werror -g -Ofast -flto -fPIC -fvisibility=hidden -march=native -mtune=native -I./src -I./debug $(SCHED_CFLAGS_$(SCHED_IMPL)) $(VALGRIND_FLAG) $(STACK_FLAG)

# Implémentation de pool à utiliser :
#   tab_pool            (tableau, défaut)
#   stailq_pool         (STAILQ + wrapper malloc par nœud)
#   stailq_pool_prealloc (STAILQ + nœuds pré-alloués, zéro malloc après init)
POOL_IMPL ?= ring_pool

# Politique d'ordonnancement :
#   hybrid (défaut) : yield=FIFO (équité) + join/exit=LIFO (DFS)
#   fifo            : tout FIFO (BFS)
#   lifo            : tout LIFO (DFS pur)
SCHED_IMPL ?= hybrid

LIB_SRC_C = src/thread.c src/scheduler.c
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
            #tests/61-mutex \
            #tests/62-mutex \
            #tests/63-mutex-equity \
            #tests/64-mutex-join 
            #tests/71-preemption 
            #tests/81-deadlock



TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

all: $(LIB_NAME) $(TEST_BINS)

#debug: CFLAGS += -DDLOG
#debug: all

$(LIB_NAME): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

src/%.o: src/%.c src/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/%: tests/%.c $(LIB_NAME)
	$(CC) $(CFLAGS) $< -o $@ -L. -lthread

#tests/71-preemption: tests/71-preemption.c $(LIB_SRC)
#	$(CC) $(CFLAGS) -DUSE_PREEMPTION $^ -o $@

pthreads: $(TEST_PTHREAD_BINS)

tests/%-pthread: tests/%.c
	$(CC) $(CFLAGS) -DUSE_PTHREAD $< -o $@ -lpthread

check: all
	LD_LIBRARY_PATH=. ./scripts/run_tests.sh

valgrind: clean
	$(MAKE) all VALGRIND_FLAG= CFLAGS="$(CFLAGS) -mno-avx512f"
	LD_LIBRARY_PATH=. ./scripts/run_valgrind.sh

graphs: all pthreads
	python3 scripts/generate_graphs.py

install: all pthreads
	mkdir -p install/lib install/bin
	cp $(LIB_NAME) install/lib/
	cp $(TEST_BINS) install/bin/
	cp $(TEST_PTHREAD_BINS) install/bin/

clean:
	rm -f src/*.o $(LIB_NAME)
	rm -f $(TEST_BINS) $(TEST_PTHREAD_BINS)
	rm -f install/lib/* install/bin/*
	rm -rf tests/*.dSYM

.PHONY: all debug clean valgrind check pthreads graphs install
