// SPDX-License-Identifier: GPL-2.0

/*
 * Test module for page_frag cache
 *
 * Copyright: linyunsheng@huawei.com
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/completion.h>
#include <linux/kthread.h>

#define OBJPOOL_NR_OBJECT_MAX	BIT(24)

struct objpool_slot {
	u32 head;
	u32 tail;
	u32 last;
	u32 mask;
	void *entries[];
} __packed;

struct objpool_head {
	int nr_cpus;
	int capacity;
	struct objpool_slot **cpu_slots;
};

/* initialize percpu objpool_slot */
static void objpool_init_percpu_slot(struct objpool_head *pool,
				     struct objpool_slot *slot)
{
	/* initialize elements of percpu objpool_slot */
	slot->mask = pool->capacity - 1;
}

/* allocate and initialize percpu slots */
static int objpool_init_percpu_slots(struct objpool_head *pool,
				     int nr_objs, gfp_t gfp)
{
	int i;

	for (i = 0; i < pool->nr_cpus; i++) {
		struct objpool_slot *slot;
		int size;

		/* skip the cpu node which could never be present */
		if (!cpu_possible(i))
			continue;

		size = struct_size(slot, entries, pool->capacity);

		/*
		 * here we allocate percpu-slot & objs together in a single
		 * allocation to make it more compact, taking advantage of
		 * warm caches and TLB hits. in default vmalloc is used to
		 * reduce the pressure of kernel slab system. as we know,
		 * minimal size of vmalloc is one page since vmalloc would
		 * always align the requested size to page size
		 */
		if (gfp & GFP_ATOMIC)
			slot = kmalloc_node(size, gfp, cpu_to_node(i));
		else
			slot = __vmalloc_node(size, sizeof(void *), gfp,
					      cpu_to_node(i),
					      __builtin_return_address(0));
		if (!slot)
			return -ENOMEM;

		memset(slot, 0, size);
		pool->cpu_slots[i] = slot;

		objpool_init_percpu_slot(pool, slot);
	}

	return 0;
}

/* cleanup all percpu slots of the object pool */
static void objpool_fini_percpu_slots(struct objpool_head *pool)
{
	int i;

	if (!pool->cpu_slots)
		return;

	for (i = 0; i < pool->nr_cpus; i++)
		kvfree(pool->cpu_slots[i]);
	kfree(pool->cpu_slots);
}

/* initialize object pool and pre-allocate objects */
static int objpool_init(struct objpool_head *pool, int nr_objs, gfp_t gfp)
{
	int rc, capacity, slot_size;

	/* check input parameters */
	if (nr_objs <= 0 || nr_objs > OBJPOOL_NR_OBJECT_MAX)
		return -EINVAL;

	/* calculate capacity of percpu objpool_slot */
	capacity = roundup_pow_of_two(nr_objs);
	if (!capacity)
		return -EINVAL;

	gfp = gfp & ~__GFP_ZERO;

	/* initialize objpool pool */
	memset(pool, 0, sizeof(struct objpool_head));
	pool->nr_cpus = nr_cpu_ids;
	pool->capacity = capacity;
	slot_size = pool->nr_cpus * sizeof(struct objpool_slot *);
	pool->cpu_slots = kzalloc(slot_size, gfp);
	if (!pool->cpu_slots)
		return -ENOMEM;

	/* initialize per-cpu slots */
	rc = objpool_init_percpu_slots(pool, nr_objs, gfp);
	if (rc)
		objpool_fini_percpu_slots(pool);

	return rc;
}

/* adding object to slot, abort if the slot was already full */
static int objpool_try_add_slot(void *obj, struct objpool_head *pool, int cpu)
{
	struct objpool_slot *slot = pool->cpu_slots[cpu];
	u32 head, tail;

	/* loading tail and head as a local snapshot, tail first */
	tail = READ_ONCE(slot->tail);

	do {
		head = READ_ONCE(slot->head);
		/* fault caught: something must be wrong */
		if (unlikely(tail - head >= pool->capacity))
			return -ENOSPC;
	} while (!try_cmpxchg_acquire(&slot->tail, &tail, tail + 1));

	/* now the tail position is reserved for the given obj */
	WRITE_ONCE(slot->entries[tail & slot->mask], obj);
	/* update sequence to make this obj available for pop() */
	smp_store_release(&slot->last, tail + 1);

	return 0;
}

/* reclaim an object to object pool */
static int objpool_push(void *obj, struct objpool_head *pool)
{
	unsigned long flags;
	int rc;

	/* disable local irq to avoid preemption & interruption */
	raw_local_irq_save(flags);
	rc = objpool_try_add_slot(obj, pool, raw_smp_processor_id());
	raw_local_irq_restore(flags);

	return rc;
}

/* try to retrieve object from slot */
static void *objpool_try_get_slot(struct objpool_head *pool, int cpu)
{
	struct objpool_slot *slot = pool->cpu_slots[cpu];
	/* load head snapshot, other cpus may change it */
	u32 head = smp_load_acquire(&slot->head);

	while (head != READ_ONCE(slot->last)) {
		void *obj;

		/*
		 * data visibility of 'last' and 'head' could be out of
		 * order since memory updating of 'last' and 'head' are
		 * performed in push() and pop() independently
		 *
		 * before any retrieving attempts, pop() must guarantee
		 * 'last' is behind 'head', that is to say, there must
		 * be available objects in slot, which could be ensured
		 * by condition 'last != head && last - head <= nr_objs'
		 * that is equivalent to 'last - head - 1 < nr_objs' as
		 * 'last' and 'head' are both unsigned int32
		 */
		if (READ_ONCE(slot->last) - head - 1 >= pool->capacity) {
			head = READ_ONCE(slot->head);
			continue;
		}

		/* obj must be retrieved before moving forward head */
		obj = READ_ONCE(slot->entries[head & slot->mask]);

		/* move head forward to mark it's consumption */
		if (try_cmpxchg_release(&slot->head, &head, head + 1))
			return obj;
	}

	return NULL;
}

/* allocate an object from object pool */
static void *objpool_pop(struct objpool_head *pool)
{
	void *obj = NULL;
	unsigned long flags;
	int i, cpu;

	/* disable local irq to avoid preemption & interruption */
	raw_local_irq_save(flags);

	cpu = raw_smp_processor_id();
	for (i = 0; i < num_possible_cpus(); i++) {
		obj = objpool_try_get_slot(pool, cpu);
		if (obj)
			break;
		cpu = cpumask_next_wrap(cpu, cpu_possible_mask, -1, 1);
	}
	raw_local_irq_restore(flags);

	return obj;
}

/* release whole objpool forcely */
static void objpool_free(struct objpool_head *pool)
{
	if (!pool->cpu_slots)
		return;

	/* release percpu slots */
	objpool_fini_percpu_slots(pool);
}

static struct objpool_head ptr_pool;
static int nr_objs = 512;
static atomic_t nthreads;
static struct completion wait;
struct rw_semaphore start_test;

static int nr_test = 5120000;
module_param(nr_test, int, 0);
MODULE_PARM_DESC(nr_test, "number of iterations to test");

static void print_cpumask(cpumask_var_t cpumask, const char *str)
{
	char *buf;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	bitmap_print_to_pagebuf(false, buf, cpumask_bits(cpumask),
				nr_cpumask_bits);

	pr_info("%s: %s", str, buf);
	kfree(buf);
}

static cpumask_var_t pop_cpumask;
static int pop_cpumask_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	if (!cpumask_available(pop_cpumask) &&
	    !alloc_cpumask_var(&pop_cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = bitmap_parse(arg, strlen(arg), cpumask_bits(pop_cpumask),
			   nr_cpumask_bits);
	if (ret)
		return ret;

	print_cpumask(pop_cpumask, "pop cpumask");

	if (cpumask_empty(pop_cpumask))
		return -EINVAL;

	return 0;
}

static const struct kernel_param_ops pop_cpumask_ops = {
	.set = pop_cpumask_set,
};

module_param_cb(pop_cpumask, &pop_cpumask_ops, NULL, 0);
MODULE_PARM_DESC(pop_cpumask, "Mask of CPUs to use for popping.");

static cpumask_var_t push_cpumask;
static int push_cpumask_set(const char *arg, const struct kernel_param *kp)
{
	int ret;

	if (!cpumask_available(push_cpumask) &&
	    !alloc_cpumask_var(&push_cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = bitmap_parse(arg, strlen(arg), cpumask_bits(push_cpumask),
			   nr_cpumask_bits);
	if (ret)
		return ret;

	print_cpumask(push_cpumask, "push cpumask");

	if (cpumask_empty(push_cpumask))
		return -EINVAL;

	return 0;
}

static const struct kernel_param_ops push_cpumask_ops = {
	.set = push_cpumask_set,
};

module_param_cb(push_cpumask, &push_cpumask_ops, NULL, 0);
MODULE_PARM_DESC(push_cpumask, "Mask of CPUs to use for pushing.");


static int page_frag_pop_thread(void *arg)
{
	struct objpool_head *pool = arg;
	int nr = nr_test;

	down_read(&start_test);
	up_read(&start_test);

	pr_info("page_frag pop test thread begins on cpu %d\n",
		smp_processor_id());

	while (nr > 0) {
		void *obj = objpool_pop(pool);

		if (obj) {
			nr--;
		} else {
			cond_resched();
		}
	}

	if (atomic_dec_and_test(&nthreads))
		complete(&wait);

	pr_info("page_frag pop test thread exits on cpu %d\n",
		smp_processor_id());

	return 0;
}

static int page_frag_push_thread(void *arg)
{
	struct objpool_head *pool = arg;
	int nr = nr_test;

	down_read(&start_test);
	up_read(&start_test);

	pr_info("page_frag push test thread begins on cpu %d\n",
		smp_processor_id());

	while (nr > 0) {
		void *va = (void *)0xdeadbeef;
		int ret;

		ret = objpool_push(va, pool);
		if (ret) {
			cond_resched();
		} else {
			nr--;
		}
	}

	pr_info("page_frag push test thread exits on cpu %d\n",
		smp_processor_id());

	if (atomic_dec_and_test(&nthreads))
		complete(&wait);

	return 0;
}

static int __init page_frag_test_init(void)
{
	struct task_struct *tsk;
	ktime_t start;
	u64 duration;
	int cpu, ret;

	if (!cpumask_available(pop_cpumask)) {
		if (!alloc_cpumask_var(&pop_cpumask, GFP_KERNEL)) {
			ret = -ENOMEM;
			goto err_cpumask;
		}

		cpumask_copy(pop_cpumask, cpu_online_mask);
	}

	if (!cpumask_available(push_cpumask)) {
		if (!alloc_cpumask_var(&push_cpumask, GFP_KERNEL)) {
			ret = -ENOMEM;
			goto err_cpumask;
		}

		cpumask_copy(push_cpumask, cpu_online_mask);
	}

	if (!cpumask_subset(pop_cpumask, cpu_online_mask) ||
	    !cpumask_subset(push_cpumask, cpu_online_mask) ||
	    cpumask_weight(pop_cpumask) != cpumask_weight(push_cpumask)) {
		ret = -EINVAL;
		goto err_cpumask;
	}

	ret = objpool_init(&ptr_pool, nr_objs, GFP_KERNEL);
	if (ret)
		goto err_cpumask;

	atomic_set(&nthreads, 0);
	init_completion(&wait);
	init_rwsem(&start_test);

	/* grab rwsem to block testing threads */
	down_write(&start_test);

	for_each_cpu(cpu, push_cpumask) {
		tsk = kthread_create_on_cpu(page_frag_push_thread, &ptr_pool,
					    cpu, "push.*%u");
		if (IS_ERR(tsk))
			break;

		wake_up_process(tsk);
		atomic_inc(&nthreads);
	}

	for_each_cpu(cpu, pop_cpumask) {
		tsk = kthread_create_on_cpu(page_frag_pop_thread, &ptr_pool,
					    cpu, "pop.*%u");
		if (IS_ERR(tsk))
			break;

		wake_up_process(tsk);
		atomic_inc(&nthreads);
	}

	/* wait a while to make sure all threads waiting at start line */
	msleep(20);
	start = ktime_get();
	up_write(&start_test);

	pr_info("waiting for test to complete\n");
	wait_for_completion(&wait);

	duration = (u64)ktime_us_delta(ktime_get(), start);
	pr_info("%d of iterations for testing took: %lluus\n", nr_test,
		duration);

	objpool_free(&ptr_pool);
	return -EAGAIN;

err_cpumask:
	if (cpumask_available(pop_cpumask))
		free_cpumask_var(pop_cpumask);

	if (cpumask_available(push_cpumask))
		free_cpumask_var(push_cpumask);

	return ret;
}

static void __exit page_frag_test_exit(void)
{
}

module_init(page_frag_test_init);
module_exit(page_frag_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yunsheng Lin <linyunsheng@huawei.com>");
MODULE_DESCRIPTION("Test module for page_frag");
