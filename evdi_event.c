// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include "evdi_drv.h"

struct kmem_cache *evdi_event_cache, *evdi_inflight_cache;
DEFINE_STATIC_KEY_FALSE(evdi_perf_key);
bool evdi_perf_on;
struct evdi_perf_counters evdi_perf;

int evdi_event_system_init(void)
{
	memset(&evdi_perf, 0, sizeof(evdi_perf));

	evdi_event_cache = kmem_cache_create("evdi_event",
					     sizeof(struct evdi_event), 0,
					     SLAB_HWCACHE_ALIGN, NULL);

	evdi_inflight_cache = kmem_cache_create(
		"evdi_inflight", sizeof(struct evdi_inflight_req), 0,
		SLAB_HWCACHE_ALIGN, NULL);

	return (evdi_event_cache && evdi_inflight_cache) ? 0 : -ENOMEM;
}

void evdi_event_system_cleanup(void)
{
	kmem_cache_destroy(evdi_event_cache);
	kmem_cache_destroy(evdi_inflight_cache);

	if (evdi_perf_on) {
		evdi_info("Event system cleaned up - Peak: %d",
			  atomic_read(&evdi_perf.event_peak_usage));
	}
}

int evdi_event_init(struct evdi_device *evdi)
{
	if (!evdi)
		return -EINVAL;

	spin_lock_init(&evdi->events.lock);
	init_waitqueue_head(&evdi->events.wait_queue);
	init_llist_head(&evdi->events.lockfree_head);
	atomic_set(&evdi->events.next_poll_id, 1);

	return 0;
}

void evdi_event_cleanup(struct evdi_device *evdi)
{
	struct evdi_event *event, *next;

	if (!evdi)
		return;

	atomic_set(&evdi->events.stopping, 1);
	wake_up_all(&evdi->events.wait_queue);

	spin_lock(&evdi->events.lock);
	event = evdi->events.head;
	evdi->events.head = evdi->events.tail = NULL;
	spin_unlock(&evdi->events.lock);

	while (event) {
		next = event->next;
		evdi_event_free(event);
		event = next;
	}
}

struct evdi_event *evdi_event_alloc(struct evdi_device *evdi,
				    enum poll_event_type type, int poll_id,
				    void *data, size_t size,
				    struct drm_file *owner)
{
	struct evdi_event *event;
	int current_allocated, peak;

	if (size > EVDI_PAYLOAD_MAX)
		return NULL;

	event = kmem_cache_alloc(evdi_event_cache, GFP_ATOMIC);
	if (!event)
		return NULL;

	EVDI_PERF_INC64(&evdi_perf.pool_alloc);

	event->type = type;
	event->poll_id = poll_id;
	event->data_size = size;

	if (data && size)
		memcpy(event->inline_data, data, size);

	event->next = NULL;
	event->owner = owner;
	atomic_set(&event->freed, 0);

	current_allocated = atomic_inc_return(&evdi_perf.event_allocated);
	while (current_allocated >
	       (peak = atomic_read(&evdi_perf.event_peak_usage))) {
		if (atomic_cmpxchg(&evdi_perf.event_peak_usage, peak,
				   current_allocated) == peak)
			break;
	}

	return event;
}

static void evdi_inflight_req_release(struct kref *kref)
{
	struct evdi_inflight_req *req =
		container_of(kref, struct evdi_inflight_req, refcount);
	int i;

	if (req->type == get_buf) {
		for (i = 0; i < req->reply.gralloc.numFds; i++) {
			if (req->reply.gralloc.data_files[i])
				fput(req->reply.gralloc.data_files[i]);
		}
	}

	kmem_cache_free(evdi_inflight_cache, req);
}

void evdi_inflight_req_get(struct evdi_inflight_req *req)
{
	if (req)
		kref_get(&req->refcount);
}

void evdi_inflight_req_put(struct evdi_inflight_req *req)
{
	if (req)
		kref_put(&req->refcount, evdi_inflight_req_release);
}

struct evdi_inflight_req *evdi_inflight_req_alloc(struct evdi_device *evdi)
{
	struct evdi_inflight_req *req;

	req = kmem_cache_zalloc(evdi_inflight_cache, GFP_ATOMIC);
	if (!req)
		return NULL;

	kref_init(&req->refcount);
	init_completion(&req->done);

	return req;
}

void evdi_event_free(struct evdi_event *event)
{
	if (event && !atomic_xchg(&event->freed, 1)) {
		atomic_dec(&evdi_perf.event_allocated);
		kmem_cache_free(evdi_event_cache, event);
	}
}

void evdi_event_queue(struct evdi_device *evdi, struct evdi_event *event)
{
	if (!evdi || !event)
		return;

	if (likely(!atomic_read(&evdi->events.stopping))) {
		if (llist_add(&event->llist, &evdi->events.lockfree_head)) {
			atomic_inc(&evdi->events.queue_size);
			EVDI_PERF_INC64(&evdi_perf.event_queue_ops);
			if (atomic_cmpxchg(&evdi->events.wake_pending, 0, 1) ==
			    0) {
				wake_up_interruptible(&evdi->events.wait_queue);
				EVDI_PERF_INC64(&evdi_perf.wakeup_count);
			}
			return;
		}
		atomic_inc(&evdi->events.queue_size);
		EVDI_PERF_INC64(&evdi_perf.event_queue_ops);
		return;
	}

	evdi_event_free(event);
}

static struct evdi_event *evdi_pop_locked(struct evdi_device *evdi)
{
	struct evdi_event *event = evdi->events.head;

	if (event) {
		evdi->events.head = event->next;
		if (!evdi->events.head)
			evdi->events.tail = NULL;
	}
	return event;
}

struct evdi_event *evdi_event_dequeue(struct evdi_device *evdi)
{
	struct evdi_event *event = NULL;
	struct llist_node *node, *next;

	if (!evdi)
		return NULL;

	spin_lock(&evdi->events.lock);

	if (!(event = evdi_pop_locked(evdi))) {
		node = llist_del_all(&evdi->events.lockfree_head);
		if (node) {
			node = llist_reverse_order(node);
			while (node) {
				struct evdi_event *current_event = llist_entry(
					node, struct evdi_event, llist);
				next = node->next;
				current_event->next = NULL;
				if (!evdi->events.head)
					evdi->events.head = current_event;
				else
					evdi->events.tail->next = current_event;
				evdi->events.tail = current_event;
				node = next;
			}
			event = evdi_pop_locked(evdi);
		}
	}

	if (event) {
		atomic_dec(&evdi->events.queue_size);
		EVDI_PERF_INC64(&evdi_perf.event_dequeue_ops);
		atomic_set(&evdi->events.wake_pending, 0);
	}

	spin_unlock(&evdi->events.lock);
	return event;
}

void evdi_event_cleanup_file(struct evdi_device *evdi, struct drm_file *file)
{
	struct evdi_event *event, *next, *new_head = NULL, *new_tail = NULL;
	struct llist_node *llnode, *llnext;

	if (!evdi || !file)
		return;

	llnode = llist_del_all(&evdi->events.lockfree_head);
	while (llnode) {
		llnext = llnode->next;
		event = llist_entry(llnode, struct evdi_event, llist);
		if (event->owner == file) {
			atomic_dec(&evdi->events.queue_size);
			evdi_event_free(event);
		} else {
			llist_add(&event->llist, &evdi->events.lockfree_head);
		}
		llnode = llnext;
	}

	spin_lock(&evdi->events.lock);
	event = evdi->events.head;
	while (event) {
		next = event->next;
		if (event->owner == file) {
			atomic_dec(&evdi->events.queue_size);
			evdi_event_free(event);
		} else {
			event->next = NULL;
			if (!new_head)
				new_head = event;
			else
				new_tail->next = event;
			new_tail = event;
		}
		event = next;
	}
	evdi->events.head = new_head;
	evdi->events.tail = new_tail;
	spin_unlock(&evdi->events.lock);

	wake_up_interruptible(&evdi->events.wait_queue);
}

int evdi_event_wait(struct evdi_device *evdi, struct drm_file *file)
{
	DEFINE_WAIT(wait);

	EVDI_PERF_INC64(&evdi_perf.poll_cycles);

	for (;;) {
		prepare_to_wait(&evdi->events.wait_queue, &wait,
				TASK_INTERRUPTIBLE);
		if (atomic_read(&evdi->events.queue_size) > 0)
			break;
		if (atomic_read(&evdi->events.stopping)) {
			finish_wait(&evdi->events.wait_queue, &wait);
			return -ENODEV;
		}
		if (signal_pending(current)) {
			finish_wait(&evdi->events.wait_queue, &wait);
			return -ERESTARTSYS;
		}
		schedule();
	}
	finish_wait(&evdi->events.wait_queue, &wait);

	return 0;
}
