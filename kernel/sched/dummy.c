/*
 * Dummy scheduling class, mapped to range a of 5 levels of SCHED_NORMAL policy
 */

#include "sched.h"

/*
 * Timeslice and age threshold are repsented in jiffies. Default timeslice
 * is 100ms. Both parameters can be tuned from /proc/sys/kernel.
 */

#define DUMMY_TIMESLICE		(100 * HZ / 1000)
#define DUMMY_AGE_THRESHOLD	(3 * DUMMY_TIMESLICE)
#define FIRST_PRIORITY		131
#define FIRST_PRIORITY_NICE	11
#define NR_PRIORITIES		5

unsigned int sysctl_sched_dummy_timeslice = DUMMY_TIMESLICE;
static inline unsigned int get_timeslice(void)
{
	return sysctl_sched_dummy_timeslice;
}

unsigned int sysctl_sched_dummy_age_threshold = DUMMY_AGE_THRESHOLD;
static inline unsigned int get_age_threshold(void)
{
	return sysctl_sched_dummy_age_threshold;
}

/*
 * Init
 */

void init_dummy_rq(struct dummy_rq *dummy_rq)
{
	printk_deferred("SKDEBUG: Initialization of rq\n");
	int i;
	for(i = 0 ; i < NR_PRIORITIES ; i++)
	{
		INIT_LIST_HEAD(&dummy_rq->p131+i);
	}
	printk_deferred("SKDEBUG: Finished initialization, going out\n");
}

/*
 * Helper functions
 */

static inline struct task_struct *dummy_task_of(struct sched_dummy_entity *dummy_se)
{
	return container_of(dummy_se, struct task_struct, dummy_se);
}

static inline void _enqueue_task_dummy(struct rq *rq, struct task_struct *p)
{
	printk_deferred("SKDEBUG: Enqueuing task %lf\n", (long)p->pid);
	struct sched_dummy_entity *dummy_se = &p->dummy_se;
	int priority = p->prio - FIRST_PRIORITY;
	struct list_head *queue = &rq->dummy.p131 + priority;
	list_add_tail(&dummy_se->run_list, queue);
	printk_deferred("SKDEBUG: Finished enqueuing task %lf\n", (long)p->pid);
}

static inline void _dequeue_task_dummy(struct task_struct *p)
{
	printk_deferred("SKDEBUG: Dequeuing task %lf\n", (long)p->pid);
	struct sched_dummy_entity *dummy_se = &p->dummy_se;
	list_del_init(&dummy_se->run_list);
	printk_deferred("SKDEBUG: Finished dequeuing task %lf\n", (long)p->pid);
}

/*
 * Scheduling class functions to implement
 */

static void enqueue_task_dummy(struct rq *rq, struct task_struct *p, int flags)
{
	_enqueue_task_dummy(rq, p);
	add_nr_running(rq,1);
}

static void dequeue_task_dummy(struct rq *rq, struct task_struct *p, int flags)
{
	_dequeue_task_dummy(p);
	sub_nr_running(rq,1);
}

static void reschedule(struct rq *rq, struct task_struct *p)
{
	printk_deferred("SKDEBUG: Rescheduling task %lf\n", (long)p->pid);
	dequeue_task_dummy(rq, p, 0);
	enqueue_task_dummy(rq, p, 0);
	printk_deferred("SKDEBUG: Finished rescheduling task %lf\n", (long)p->pid);
}

static void yield_task_dummy(struct rq *rq)
{
	printk_deferred("SKDEBUG: Yielding task %lf\n", (long)rq->curr->pid);
	reschedule(rq, rq->curr);
	resched_curr(rq);
	printk_deferred("SKDEBUG: Finished yielding task %lf\n", (long)rq->curr->pid);
}

static void check_preempt_curr_dummy(struct rq *rq, struct task_struct *p, int flags)
{
	printk_deferred("SKDEBUG: Checking preemption for task %lf replaced by %lf\n", (long)rq->curr->pid, (long)p->pid);
	if(rq->curr->prio > p->prio) {
		reschedule(rq, rq->curr);
		resched_curr(rq);
	}
	printk_deferred("SKDEBUG: Finished checking preemption %lf, %lf\n", (long)rq->curr->pid, (long)p->pid);
}

static struct task_struct *pick_next_task_dummy(struct rq *rq, struct task_struct* prev, struct rq_flags* rf)
{
	printk_deferred("SKDEBUG: Picking next task\n");
	struct dummy_rq *dummy_rq = &rq->dummy;
	struct sched_dummy_entity *next;
	struct task_struct *task;
	int i;
	for(i = 0 ; i < NR_PRIORITIES ; i++)
	{
		if(!list_empty(&dummy_rq->p131 + i))
		{
			next = list_first_entry(&dummy_rq->p131 + i, struct sched_dummy_entity, run_list);
	                put_prev_task(rq, prev);
			task = dummy_task_of(next);
			task->prio = task->static_prio;
			printk_deferred("SKDEBUG: Finished picking and returning task %lf\n", (long)task->pid);
			return task;
		}
	}
	printk_deferred("SKDEBUG: Finished picking without finding any task\n");
	return NULL;
}

static void put_prev_task_dummy(struct rq *rq, struct task_struct *prev)
{
}

static void set_curr_task_dummy(struct rq *rq)
{
}

static void task_tick_dummy(struct rq *rq, struct task_struct *curr, int queued)
{
	printk_deferred("SKDEBUD: New tick occuring with task %lf as curr\n", (long)curr->pid);
	int i;
	struct list_head *dummy_rq = &rq->dummy.p131;
	struct sched_dummy_entity *entity;
	struct task_struct *task;
	for(i = 0 ; i < NR_PRIORITIES ; i++)
	{
		list_for_each_entry(entity, dummy_rq + i, run_list)
		{
			task = dummy_task_of(entity);
			task->dummy_se.age++;
			if(task->dummy_se.age >= DUMMY_AGE_THRESHOLD)
			{
				task->dummy_se.age = 0;
				int prio = task->prio;
				if(prio > FIRST_PRIORITY)
				{
					task->prio = prio - FIRST_PRIORITY + FIRST_PRIORITY_NICE - 1;
					reschedule(rq, task);
				}
			}
		}
	}
	curr->dummy_se.time_slice++;
	if(curr->dummy_se.time_slice >= DUMMY_TIMESLICE) {
		reschedule(rq, curr);
		resched_curr(rq);
	}
	printk_deferred("SKDEBUG: Finished tick with task %lf as curr\n", (long)curr->pid);
}

static void switched_from_dummy(struct rq *rq, struct task_struct *p)
{
}

static void switched_to_dummy(struct rq *rq, struct task_struct *p)
{
}

static void prio_changed_dummy(struct rq*rq, struct task_struct *p, int oldprio)
{
	printk_deferred("SKDEBUG: Changing prio for task %lf\n", (long)p->pid);
	if(rq->curr->prio > p->prio)
	{
		reschedule(rq, rq->curr);
		resched_curr(rq);
	}
	printk_deferred("SKDEBUG: Finished changing prio for task %lf\n", (long)p->pid);
}

static unsigned int get_rr_interval_dummy(struct rq* rq, struct task_struct *p)
{
	return get_timeslice();
}
#ifdef CONFIG_SMP
/*
 * SMP related functions	
 */

static inline int select_task_rq_dummy(struct task_struct *p, int cpu, int sd_flags, int wake_flags)
{
	int new_cpu = smp_processor_id();
	
	return new_cpu; //set assigned CPU to zero
}


static void set_cpus_allowed_dummy(struct task_struct *p,  const struct cpumask *new_mask)
{
}
#endif
/*
 * Scheduling class
 */
static void update_curr_dummy(struct rq*rq)
{
}
const struct sched_class dummy_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_dummy,
	.dequeue_task		= dequeue_task_dummy,
	.yield_task		= yield_task_dummy,

	.check_preempt_curr	= check_preempt_curr_dummy,
	
	.pick_next_task		= pick_next_task_dummy,
	.put_prev_task		= put_prev_task_dummy,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_dummy,
	.set_cpus_allowed	= set_cpus_allowed_dummy,
#endif

	.set_curr_task		= set_curr_task_dummy,
	.task_tick		= task_tick_dummy,

	.switched_from		= switched_from_dummy,
	.switched_to		= switched_to_dummy,
	.prio_changed		= prio_changed_dummy,

	.get_rr_interval	= get_rr_interval_dummy,
	.update_curr		= update_curr_dummy,
};
