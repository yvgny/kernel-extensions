#include <stdio.h>
#include "syscalls_test.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "get_child_pids_test.h"

#define MAX_LIMIT ((size_t) 3)
#define SUCCESS 0
#define BAD_ADDRESS ((void*) 47424742)

void test_too_small_array();

void test_null_limit();

void test_children_count_number();

void test_incorrect_memory_address();

void run_processes_with_limit(int, long, size_t);

void get_child_pids_tests()
{
	printf("\033[0;31m======== Get child PIDs syscall test suite : ========\n\033[0m");
	test_too_small_array();
	test_null_limit();
	test_children_count_number();
	test_incorrect_memory_address();
	printf("\n\n");
}

void test_too_small_array()
{
	printf("Passing a smaller array should return an error: ");
	run_processes_with_limit(MAX_LIMIT - 1, ENOBUFS, MAX_LIMIT);
}

void test_null_limit()
{
	printf("Passing an array of limit 0 should only return number of children: ");
	run_processes_with_limit(0, SUCCESS, MAX_LIMIT);
}

void test_children_count_number()
{
	printf("Running three subprocesses (including recursion) should output 3: ");
	run_processes_with_limit(MAX_LIMIT, SUCCESS, MAX_LIMIT);
}

void test_incorrect_memory_address()
{
	long res;
	size_t *num_children;
	num_children = malloc(sizeof(size_t));
	pid_t ids[MAX_LIMIT];

	printf("Giving a null address for ids when limit != should output an error: ");
	res = get_child_pids(NULL, MAX_LIMIT, num_children);
	if (res != EFAULT) {
		printf("failed: return value is %ld.\n", res);
	} else {
		printf("passed !\n");
	}

	printf("Giving a null address for ids when limit == 0 should be accepted: ");
	res = get_child_pids(NULL, 0, num_children);
	if (res != 0) {
		printf("failed: return value is %ld.\n", res);
	} else {
		printf("passed !\n");
	}

	printf("Giving a bad address for ids should output an error: ");
	res = get_child_pids(BAD_ADDRESS, MAX_LIMIT, num_children);
	if (res != EFAULT) {
		printf("failed: return value is %ld.\n", res);
	} else {
		printf("passed !\n");
	}

	printf("Giving a bad address for num_children should output an error: ");
	res = get_child_pids(ids, MAX_LIMIT, BAD_ADDRESS);
	if (res != EFAULT) {
		printf("failed: return value is %ld.\n", res);
	} else {
		printf("passed !\n");
	}

	printf("Giving a bad address for num_children should output an error: ");
	res = get_child_pids(ids, MAX_LIMIT, BAD_ADDRESS);
	if (res != EFAULT) {
		printf("failed: return value is %ld.\n", res);
	} else {
		printf("passed !\n");
	}

	free(num_children);
}

void run_processes_with_limit(int limit, long expected_return_value,
			      size_t expected_num_children)
{
	if (fork() != 0) {
		if (fork() == 0) {
			if (fork() != 0) {
				sleep(2);
				_Exit(0);
			} else {
				sleep(2);
				_Exit(0);
			}
		} else {
			sleep(1);
			long res = -1;
			pid_t ids[limit];
			size_t *num_children;
			num_children = malloc(sizeof(size_t));
			res = get_child_pids(ids, limit, num_children);
			if (expected_return_value != res
			    || expected_num_children != *num_children) {
				printf("failed : (return_value, num_children) should be (%ld, %zu) but was (%ld, %zu)\n",
				       expected_return_value,
				       expected_num_children, res,
				       *num_children);
			} else {
				printf("passed !\n");
			}
			free(num_children);
			{
				int i;
				for (i = 0; i < MAX_LIMIT; i++) {
					wait(NULL);
				}
			}
		}
	} else {
		sleep(2);
		_Exit(0);
	}
}
