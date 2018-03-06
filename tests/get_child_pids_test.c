#include <stdio.h>
#include "syscalls_test.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#define NUM_THREAD 3
#define LIMIT 10

void* do_something(void*);

int main()
{
	pthread_t tid[NUM_THREAD];
	for(int i = 0 ; i < NUM_THREAD ; i++) {
		pthread_create(&tid[i], NULL, &do_something, NULL);
	}
	for(int i = 0 ; i < NUM_THREAD ; i++) {	
		pthread_join(tid[i], NULL);
	}
	return 0;
}

void* do_something(void* arg)
{
	if(fork() != 0) {
		if(fork() == 0) {
			sleep(5);
		} else {
			if(fork() != 0){
				long res = -1;
				pid_t ids[LIMIT];
				size_t *num_children;
				num_children = malloc(sizeof(size_t));
				res = get_child_pids(ids, LIMIT, num_children);
				printf("%ld children have been found.\n", (long)*num_children);
				for(int i = 0 ; i < LIMIT && i < *num_children ; i++) {
					printf("Child %d had process id : %ld\n", i, (long)ids[i]);
				}
				printf("Return value : %ld\n", res);
			} else {
				sleep(5);
			}
		}
	} else {
		sleep(5);
	}
}
