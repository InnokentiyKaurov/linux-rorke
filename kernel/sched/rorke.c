#include <linux/ktime.h>
#include <linux/cpumask.h>
#include <linux/sched/topology.h>
#include <linux/mmu_context.h>
#include "sched.h"
#include <linux/sched/rorke.h>

#define DEBUG(rq, fmt, ...) \
	do {} while (0)
	// printk(KERN_ERR "rorke[cpu=%d] " fmt, cpu_of(rq), ##__VA_ARGS__) \

void lock_dsq(struct rk_rq *rk_rq);
void unlock_dsq(struct rk_rq *rk_rq);

struct rk_dsq *alloc_init_rk_dsq(void)
{
	struct rk_dsq *dsq;

	dsq = kmalloc(sizeof(*dsq), GFP_KERNEL);
	if (!dsq)
		BUG();

	spin_lock_init(&dsq->lock);
    INIT_LIST_HEAD(&dsq->list);
	dsq->nr_queued = 0;
	dsq->nr_running = 0;
	dsq->timeslice = RORKE_DEFAULT_TIMESLICE;

	return dsq;
}

void init_rk_rq(struct rk_rq *rk_rq, struct rk_dsq *dsq)
{
	rk_rq->dsq = dsq;
	rk_rq->curr = NULL;
	rk_rq->timeslice = RORKE_DEFAULT_TIMESLICE;
}

void init_rk_entity(struct sched_rk_entity *entity)
{
	INIT_LIST_HEAD(&entity->run_node);
	entity->on_rq = 0;
	entity->on_dsq = 0;
	entity->migrating = 0;
	entity->rq = NULL;
	entity->start_exec_ns = 0;
}

void lock_dsq(struct rk_rq *rk_rq)
{
	struct rk_dsq *rk_dsq = rk_rq->dsq;

	spin_lock(&rk_dsq->lock);
	rk_rq->timeslice = rk_dsq->timeslice; // piggyback timeslice update
}

void unlock_dsq(struct rk_rq *rk_rq)
{
	struct rk_dsq *rk_dsq = rk_rq->dsq;

	spin_unlock(&rk_dsq->lock);
}

bool rk_can_stop_tick(struct rq *rq)
{
	struct rk_rq *rk_rq = &rq->rk;
	struct rk_dsq *rk_dsq = rk_rq->dsq;
	bool no_rorke_tasks;

	lock_dsq(rk_rq);
	no_rorke_tasks = (rk_dsq->nr_running == 0);
	unlock_dsq(rk_rq);

	return no_rorke_tasks;
}

static void enqueue_task_rk(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rk_entity *se = &p->rk;
	struct rk_rq *rk_rq = &rq->rk;
	struct rk_dsq *rk_dsq = rk_rq->dsq;

	se->on_rq = 1;
	se->rq = rq;

	if (!se->migrating) {
		lock_dsq(rk_rq);
		list_add_tail(&se->run_node, &rk_dsq->list);
		se->on_dsq = 1;
		rk_dsq->nr_queued++;
		rk_dsq->nr_running++;
		unlock_dsq(rk_rq);
	}

	add_nr_running(rq, 1);

	DEBUG(rq, "enqueue pid=%d\n",
	          task_pid_nr(p));
}

static bool dequeue_task_rk(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rk_entity *se = &p->rk;
	struct rk_rq *rk_rq = &rq->rk;
	struct rk_dsq *rk_dsq = rk_rq->dsq;

	if (!se->on_rq)
		return false;

	se->on_rq = 0;
	se->rq = NULL;

	if (!se->migrating) {
		lock_dsq(rk_rq);
		if (se->on_dsq) {
			list_del_init(&se->run_node);
			rk_dsq->nr_queued--;
		}
		rk_dsq->nr_running--;
		se->on_dsq = 0;
		if (p == rk_rq->curr)
			rk_rq->curr = NULL;
		unlock_dsq(rk_rq);
	}

	sub_nr_running(rq, 1);

	DEBUG(rq, "dequeue pid=%d\n",
	          task_pid_nr(p));
	return true;
}

static void wakeup_preempt_rk(struct rq *rq, struct task_struct *p, int flags)
{
	/* not implemented */
}

/* Assumes dst is locked. Keeps it locked on exit. */
static struct task_struct *move_task_to_rq(struct task_struct *p, struct rq *dst)
{
	struct sched_rk_entity *se = &p->rk;
	struct rq *src = se->rq;

	if (src == dst)
		return p;

	if (src == NULL)
		return NULL;

	raw_spin_rq_unlock(dst);
	raw_spin_rq_lock(src);

	// Sanity checks
	if (!se->on_rq || se->rq != src) {
		raw_spin_rq_unlock(src);
		raw_spin_rq_lock(dst);
		return NULL;
	}

	se->migrating = 1;
	deactivate_task(src, p, 0);
	set_task_cpu(p, cpu_of(dst));

	raw_spin_rq_unlock(src);
	raw_spin_rq_lock(dst);

	activate_task(dst, p, 0);
	se->migrating = 0;

	return p;
}

// Assumes dsq is locked
static struct task_struct *find_movable_task_on_dsq(struct rk_dsq *rk_dsq, int cpu)
{
	struct task_struct *p;

	list_for_each_entry(p, &rk_dsq->list, rk.run_node) {
		// Test CPU affinity
		if (cpumask_test_cpu(cpu, p->cpus_ptr))
			return p;
	}

	return NULL;
}

static struct task_struct *pick_task_rk(struct rq *rq, struct rq_flags *rf)
{
	struct rk_rq *rk_rq = &rq->rk;
	struct rk_dsq *rk_dsq = rk_rq->dsq;
	struct task_struct *p;

	if (rk_rq->curr && rk_rq->curr->rk.start_exec_ns + rk_rq->timeslice > rq_clock_task(rq))
		return rk_rq->curr; // timeslice not expired

	lock_dsq(rk_rq);
	if (list_empty(&rk_dsq->list)) {
		unlock_dsq(rk_rq);
		return NULL;
	}
	p = find_movable_task_on_dsq(rk_dsq, cpu_of(rq));
	if (!p) {
		unlock_dsq(rk_rq);
		return NULL;
	}
	list_del_init(&p->rk.run_node); // it's a bit sketchy that we remove it here: if set_next_task isn't triggered, then p is lost
	rk_dsq->nr_queued--;
	p->rk.on_dsq = 0;
	unlock_dsq(rk_rq);

	p = move_task_to_rq(p, rq);

	if (p) {
		DEBUG(rq, "pick_task candidate pid=%d\n",
						task_pid_nr(p));
	} else {
		DEBUG(rq, "pick_task no candidate\n");
	}

	return p;
}

static void update_curr_rk(struct rq *rq)
{
	/* not implemented */
}

static void set_next_task_rk(struct rq *rq, struct task_struct *p, bool first)
{
	DEBUG(rq, "set_next pid=%d from_curr=%d\n",
          task_pid_nr(p), rq->rk.curr == p);

	struct sched_rk_entity *se = &p->rk;

	rq->rk.curr = p;
	se->start_exec_ns = rq_clock_task(rq);	
}

static void put_prev_task_rk(struct rq *rq, struct task_struct *p,
                              struct task_struct *next)
{
	DEBUG(rq, "put_prev pid=%d next=%d curr=%d on_rq=%d\n",
		task_pid_nr(p), next ? task_pid_nr(next) : -1, rq->rk.curr ? task_pid_nr(rq->rk.curr) : -1,
		p->rk.on_rq);

	struct sched_rk_entity *se = &p->rk;
	struct rk_rq *rk_rq = &rq->rk;
	struct rk_dsq *rk_dsq = rk_rq->dsq;

	if (!se->on_rq || se->migrating)
		return;

	if (rk_rq->curr == p) {
		rk_rq->curr = NULL;
    }

	se->on_dsq = 1;
	lock_dsq(rk_rq);
	list_add_tail(&se->run_node, &rk_dsq->list);
	rk_dsq->nr_queued++;
	unlock_dsq(rk_rq);
}

static void task_tick_rk(struct rq *rq, struct task_struct *p, int queued)
{
	struct rk_rq *rk_rq = &rq->rk;
	struct rk_dsq *rk_dsq = rk_rq->dsq;
	bool have_queued_tasks;

	if (rk_rq->curr != p)
		return;

	if (p->rk.start_exec_ns + rk_rq->timeslice > rq_clock_task(rq))
		return; // not expired yet

	lock_dsq(rk_rq);
	have_queued_tasks = (rk_dsq->nr_queued > 0);
	unlock_dsq(rk_rq);

	if (have_queued_tasks) {
		DEBUG(rq, "task_tick resched pid=%d\n", task_pid_nr(p));
		resched_curr(rq);
	}
}

static void prio_changed_rk(struct rq *rq, struct task_struct *p, long long unsigned int oldprio)
{
	/* not implemented */
}

static void switched_to_rk(struct rq *rq, struct task_struct *p)
{
	if (p->on_rq && rq->curr != p)
        resched_curr(rq);
}

static int balance_rk(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	/* not implemented */
	return 0;
}

static int select_task_rq_rk(struct task_struct *p, int cpu_hint, int flags)
{
	return task_cpu(p);
}

DEFINE_SCHED_CLASS(rorke) = {
	.enqueue_task		= enqueue_task_rk,
	.dequeue_task		= dequeue_task_rk,

	.wakeup_preempt		= wakeup_preempt_rk,

	.pick_task			= pick_task_rk,
	.put_prev_task		= put_prev_task_rk,
	.set_next_task		= set_next_task_rk,

	.balance            = balance_rk,
	.select_task_rq     = select_task_rq_rk,
	.set_cpus_allowed	= set_cpus_allowed_common,

	.task_tick			= task_tick_rk,

	.prio_changed		= prio_changed_rk,
	.switched_to		= switched_to_rk,
	.update_curr		= update_curr_rk,
};