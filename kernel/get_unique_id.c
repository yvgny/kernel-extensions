#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

DEFINE_SPINLOCK(lock);

asmlinkage long sys_get_unique_id(int *uuid)
{
	static int counter = 0;
	int curr = -1;

	spin_lock(&lock);
	curr = counter++;
	spin_unlock(&lock);

	return put_user(curr, uuid);
}
