#include <stdio.h>
#include "syscalls_test.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "get_unique_id_test.h"

#define NUM_THREAD 10
#define NUM_ITER 4
#define NUM_ID ((NUM_THREAD) * (NUM_ITER))

void *fill_check(void *);

void test_parrallel_thread();

int *check;

void get_unique_id_tests()
{
    printf("\033[0;31m======== Get unique ID test suite : ========\033[0m\n");
    test_parrallel_thread();
    printf("\n\n");
}

void test_parrallel_thread()
{
    printf("Running %d thread of %d iterations should output %d different IDs : ", NUM_THREAD, NUM_ITER, NUM_ID);
    
    pthread_t tid[NUM_THREAD];
    check = calloc(NUM_ID, sizeof(int));
    
    for (int i = 0; i < NUM_THREAD; i++) {
        pthread_create(&tid[i], NULL, &fill_check, NULL);
    }
    for (int i = 0; i < NUM_THREAD; i++) {
        pthread_join(tid[i], NULL);
    }
    {
        int i;
        for (i = 0; i < NUM_ID; i++) {
            if (check[i] != 1) {
                printf("failed : ID number %d wasn't generated\n", i);
                free(check);
                return 0;
            }
        }
    }
    
    printf("passed !\n");
    free(check);
    return 0;
}

void *fill_check(void *arg)
{
    long res = -1;
    int number;
    for (int i = 0; i < NUM_ITER; i++) {
        res = get_unique_id(&number);
        if(res != 0) {
            printf("failed ! Return value should be 0 but was %ld.\n", res);
        } else {
            check[number % NUM_ID] = 1;
        }
    }
}

