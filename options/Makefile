CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -g -fPIC -I./src -I./debug

LIB_SRC = src/thread.c src/scheduler.c
LIB_OBJ = $(LIB_SRC:.c=.o)
LIB_NAME = libthread.so

TESTS_DIR = tests

TEST_BINS = $(TESTS_DIR)/01-main \
            $(TESTS_DIR)/02-switch \
            $(TESTS_DIR)/03-equity \
            $(TESTS_DIR)/11-join \
            $(TESTS_DIR)/12-join-main \
            $(TESTS_DIR)/13-join-switch \
            $(TESTS_DIR)/21-create-many \
            $(TESTS_DIR)/22-create-many-recursive \
            $(TESTS_DIR)/23-create-many-once \
            $(TESTS_DIR)/31-switch-many \
            $(TESTS_DIR)/32-switch-many-join \
            $(TESTS_DIR)/33-switch-many-cascade \
            $(TESTS_DIR)/51-fibonacci



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
	rm -f src/*.o $(LIB_NAME)
	rm -f $(TEST_BINS) $(TEST_PTHREAD_BINS)
	rm -f install/lib/* install/bin/*
	rm -rf tests/*.dSYM

.PHONY: all debug clean valgrind check pthreads graphs install
