CC      = gcc
VALGRIND_FLAG ?=
STACK_SIZE ?=
STACK_FLAG = $(if $(STACK_SIZE),-DSTACK_SIZE=$(STACK_SIZE),)
EXTRA_CFLAGS ?=
CFLAGS  = -Wall -Wextra -Werror -g -Ofast -flto -fPIC -fvisibility=hidden -march=native -mtune=native -fno-plt -DNDEBUG -I./src $(VALGRIND_FLAG) $(STACK_FLAG) $(EXTRA_CFLAGS)

LIB_SRC_C = src/thread.c src/scheduler.o
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
            tests/81-deadlock

TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

all: $(LIB_NAME) $(TEST_BINS)

$(LIB_NAME): $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^

src/%.o: src/%.c src/thread.h 
	$(CC) $(CFLAGS) -c $< -o $@

tests/%: tests/%.c $(LIB_NAME)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable $< -o $@ -L. -lthread

tests/51-fibonacci: tests/51-fibonacci.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DFP $^ -o $@

tests/71-preemption: tests/71-preemption.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DUSE_PREEMPTION $^ -o $@

tests/72-preemption-mutex-bug: tests/72-preemption-mutex-bug.c $(LIB_SRC_C) $(LIB_SRC_S)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -DUSE_PREEMPTION $^ -o $@

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
	rm -f src/*.o $(LIB_NAME)
	rm -f $(TEST_BINS) $(TEST_PTHREAD_BINS)
	rm -rf install/lib/* install/bin/*
	rm -rf tests/*.dSYM
	rm -f src/*.gcda src/*.gcno tests/*.gcda tests/*.gcno
	rm -f graphs/*.png
	rm -f perf.data perf.data.old

.PHONY: all clean valgrind check pthreads graphs install pgo
