LDLIBS = -pthread

all: run_tests

run_tests.o: get_child_pids_test.o get_unique_id_test.o

get_child_pids_test.o: get_child_pids_test.h get_child_pids_test.c

get_unique_id_test.o: get_unique_id_test.h get_unique_id_test.c

run_tests: run_tests.o get_child_pids_test.o get_unique_id_test.o
