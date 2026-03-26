CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -g -fPIC -I./src

LIB_SRC = $(wildcard src/*.c)
LIB_OBJ = $(LIB_SRC:.c=.o)
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
            tests/51-fibonacci 
            #tests/61-mutex \
            #tests/62-mutex \
            #tests/63-mutex-equity \
            #tests/64-mutex-join \
            #tests/71-preemption \
            #tests/81-deadlock

TEST_PTHREAD_BINS = $(addsuffix -pthread,$(TEST_BINS))

all: $(LIB_NAME) $(TEST_BINS)

$(LIB_NAME): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

src/%.o: src/%.c src/thread.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/%: tests/%.c $(LIB_NAME)
	$(CC) $(CFLAGS) $< -o $@ -L. -lthread

pthreads: $(TEST_PTHREAD_BINS)

tests/%-pthread: tests/%.c
	$(CC) $(CFLAGS) -DUSE_PTHREAD $< -o $@ -lpthread

check: all
	LD_LIBRARY_PATH=. ./scripts/run_tests.sh

valgrind: all
	LD_LIBRARY_PATH=. ./scripts/run_valgrind.sh

graphs: all pthreads
	python3 scripts/generate_graphs.py

install: all pthreads
	mkdir -p install/lib install/bin
	cp $(LIB_NAME) install/lib/
	cp $(TEST_BINS) install/bin/
	cp $(TEST_PTHREAD_BINS) install/bin/

clean:
	rm -f $(LIB_OBJ) $(LIB_NAME)
	rm -f $(TEST_BINS) $(TEST_PTHREAD_BINS)
	rm -f install/lib/* install/bin/*
	rm -rf tests/*.dSYM

.PHONY: all clean valgrind check pthreads graphs install