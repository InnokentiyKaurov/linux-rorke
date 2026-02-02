#include <linux/ktime.h>
#include <linux/cpumask.h>
#include <linux/sched/topology.h>
#include <linux/mmu_context.h>
#include "sched.h"
#include <linux/sched/rorke.h>

#define DEBUG(rq, fmt, ...) \
	do {} while (0)
	// printk(KERN_ERR "rorke[cpu=%d] " fmt, cpu_of(rq), ##__VA_ARGS__) \

void init_rk_rq(struct rk_rq *rk_rq)
{
	INIT_LIST_HEAD(&rk_rq->queue);
	rk_rq->curr = NULL;
	rk_rq->nr_running = 0;
}

void init_rk_entity(struct sched_rk_entity *entity)
{
	INIT_LIST_HEAD(&entity->run_node);
	entity->on_rq = 0;
	entity->start_exec_ns = 0;
}

static void enqueue_task_rk(struct rq *rq, struct task_struct *p, int flags)
{
	struct rk_rq *rk_rq = &rq->rk;
	struct sched_rk_entity *se = &p->rk;

	list_add_tail(&p->rk.run_node, &rk_rq->queue);

	se->on_rq = 1;
	rk_rq->nr_running++;
	add_nr_running(rq, 1);

	DEBUG(rq, "enqueue pid=%d nr_running=%u\n",
	          task_pid_nr(p), rq->rk.nr_running);
}

static bool dequeue_task_rk(struct rq *rq, struct task_struct *p, int flags)
{
	struct rk_rq *rk_rq = &rq->rk;
	struct sched_rk_entity *se = &p->rk;

	if (!se->on_rq)
		return false;

	/* Currently running task is not stored on the queue */
	if (p == rq->rk.curr)
		rq->rk.curr = NULL;
	else {
		if (WARN_ON_ONCE(list_empty(&se->run_node))) {}
		list_del_init(&se->run_node);
	}

	se->on_rq = 0;
	rk_rq->nr_running--;
	sub_nr_running(rq, 1);

	DEBUG(rq, "dequeue pid=%d nr_running=%u\n",
	          task_pid_nr(p), rq->rk.nr_running);
	return true;
}

static void wakeup_preempt_rk(struct rq *rq, struct task_struct *p, int flags)
{
	/* not implemented */
}

static struct task_struct *pick_task_rk(struct rq *rq, struct rq_flags *rf)
{
	struct rk_rq *rk_rq = &rq->rk;
	struct task_struct *p;

	if (rk_rq->curr && rk_rq->curr->rk.start_exec_ns + RORKE_TIMESLICE > rq_clock_task(rq))
		return rk_rq->curr;

	if (list_empty(&rk_rq->queue))
		return NULL;

	p = list_first_entry(&rk_rq->queue, struct task_struct, rk.run_node);

	DEBUG(rq, "pick_task candidate pid=%d nr_running=%u\n",
					task_pid_nr(p), rq->rk.nr_running);

	return p;
}

static void update_curr_rk(struct rq *rq)
{
	/* not implemented */
}

static void set_next_task_rk(struct rq *rq, struct task_struct *p, bool first)
{
	DEBUG(rq, "set_next pid=%d from_curr=%d nr_running=%u\n",
          task_pid_nr(p), rq->rk.curr == p, rq->rk.nr_running);

	struct sched_rk_entity *se = &p->rk;

	se->start_exec_ns = rq_clock_task(rq);

	if (rq->rk.curr != p) {
		list_del_init(&se->run_node); 
		rq->rk.curr = p;
	}
}

static void put_prev_task_rk(struct rq *rq, struct task_struct *p,
                              struct task_struct *next)
{
	DEBUG(rq, "put_prev pid=%d next=%d curr=%d on_rq=%d nr_running=%u\n",
		task_pid_nr(p), next ? task_pid_nr(next) : -1, rq->rk.curr ? task_pid_nr(rq->rk.curr) : -1,
		p->rk.on_rq, rq->rk.nr_running);

	if (!p->rk.on_rq)
		return;

	/* If p was running, requeue it */
	list_add_tail(&p->rk.run_node, &rq->rk.queue);
    if (rq->rk.curr == p) {
		rq->rk.curr = NULL;
    }
}

static void task_tick_rk(struct rq *rq, struct task_struct *p, int queued)
{
	struct rk_rq *rk_rq = &rq->rk;

	if (rk_rq->curr == p && rk_rq->nr_running > 1 && p->rk.start_exec_ns + RORKE_TIMESLICE < rq_clock_task(rq)) {
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