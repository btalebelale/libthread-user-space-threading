CC      = gcc
VALGRIND_FLAG ?=
STACK_SIZE ?=
STACK_FLAG = $(if $(STACK_SIZE),-DSTACK_SIZE=$(STACK_SIZE),)
RING_BITS ?=
RING_FLAG = $(if $(RING_BITS),-DRING_BITS=$(RING_BITS),)
EXTRA_CFLAGS ?=
CFLAGS  = -Wall -Wextra -Werror -g -Ofast -flto -fPIC -fvisibility=hidden -march=native -mtune=native -DNDEBUG -I./src $(VALGRIND_FLAG) $(STACK_FLAG) $(RING_FLAG) $(EXTRA_CFLAGS)

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
            #tests/64-mutex-join \
            #tests/71-preemption \
            #tests/81-deadlock

TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

BENCH_BINS = bench/bench_all
BENCH_PTHREAD_BINS = $(addsuffix -pthread,$(BENCH_BINS))

all: $(LIB_NAME) $(TEST_BINS)

$(LIB_NAME): $(LIB_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^

src/%.o: src/%.c src/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/%: tests/%.c $(LIB_NAME)
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable $< -o $@ -L. -lthread

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

bench/bench_all: bench/bench_all.c $(LIB_NAME)
	$(CC) $(CFLAGS) $< -o $@ -L. -lthread

bench/bench_all-pthread: bench/bench_all.c
	$(CC) $(CFLAGS) -DUSE_PTHREAD $< -o $@ -lpthread

bench: $(BENCH_BINS) $(BENCH_PTHREAD_BINS)
	@echo "=== libthread ==="
	LD_LIBRARY_PATH=. ./bench/bench_all 2>bench_thread.csv
	@echo ""
	@echo "=== pthread ==="
	./bench/bench_all-pthread 2>bench_pthread.csv
	@echo ""
	@echo "Results saved: bench_thread.csv, bench_pthread.csv"

pgo: clean
	$(MAKE) all EXTRA_CFLAGS="-fprofile-generate"
	LD_LIBRARY_PATH=. ./scripts/run_tests.sh
	$(MAKE) clean
	$(MAKE) all EXTRA_CFLAGS="-fprofile-use -fprofile-correction"

clean:
	rm -f src/*.o $(LIB_NAME)
	rm -f $(TEST_BINS) $(TEST_PTHREAD_BINS)
	rm -f $(BENCH_BINS) $(BENCH_PTHREAD_BINS)
	rm -f bench_thread.csv bench_pthread.csv bench_aos.csv bench_soa.csv bench_swiss.csv
	rm -rf install/lib/* install/bin/*
	rm -rf tests/*.dSYM
	rm -f src/*.gcda src/*.gcno tests/*.gcda tests/*.gcno
	rm -f graphs/*.png
	rm -f perf.data perf.data.old

.PHONY: all clean valgrind check pthreads graphs install pgo bench
