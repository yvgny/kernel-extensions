#include <stdio.h>
#include "syscalls_test.h"
#include <pthread.h>
#include <stdlib.h>

#define NUM_THREAD 10
#define NUM_ITER 4

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
	long res = -1;
	int* ptrs[NUM_ITER];
	for(int i = 0 ; i < NUM_ITER ; i++)
	{
		ptrs[i] = malloc(sizeof(int));
		res = get_unique_id(ptrs[i]);
		printf("Get unique id return value: %d\n", *ptrs[i]);
	}
}
