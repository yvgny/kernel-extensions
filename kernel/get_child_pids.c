#include <linux/linkage.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/uaccess.h>

long recursive_children(struct task_struct*, pid_t*, size_t, size_t*);

asmlinkage long sys_get_child_pids(pid_t *list, size_t limit, size_t *num_children)
{
	size_t n_children = 0;
	long ret, error;
	pid_t temp_list[limit];
	if(list == NULL && limit != 0) {
		return -EFAULT;
	}

	read_lock(&tasklist_lock);
	ret = recursive_children(current, temp_list, limit, &n_children);
	read_unlock(&tasklist_lock);

	error = put_user(n_children, num_children);
	{
		int i;
		for(i = 0 ; i < limit ; i++) {
			if(put_user(temp_list[i], &list[i])) {
				return -EFAULT;
			}
		}
	}
	if(limit != 0 && *num_children > limit) {
		return -ENOBUFS;
	}

	return ret | error;
}

long recursive_children(struct task_struct* process, pid_t *list, size_t limit, size_t *n_children)
{
	long ret = 0;
	struct task_struct* pos;
	struct task_struct* next_process;
	list_for_each_entry(pos, &process->children, sibling) {
		next_process = list_entry(&pos->children, struct task_struct, children);
		ret = recursive_children(next_process, list, limit, n_children);
		if(*n_children <= limit) {
			list[(*n_children)++] = pos->pid;
		} else {
			(*n_children)++;
		}
	}

	return ret;
}
