#ifndef syscalls_test_h
#define syscalls_test_h

#include <errno.h>
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>

#if __x86_64__
#define __NR_get_unique_id 333
#define __NR_get_child_pids 334
#else
#define __NR_get_unique_id 385
#define __NR_get_child_pids 386
#endif

static __inline__ long get_unique_id(int *uuid)
{
	return syscall(__NR_get_unique_id, uuid) ? errno : 0;
}

static __inline__ long get_child_pids(pid_t *list, size_t limit,
				      size_t *num_children)
{
	return syscall(__NR_get_child_pids, list, limit, num_children) ? errno
								       : 0;
}
#endif
