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

struct evdi_gralloc_buf_stack {
	struct evdi_gralloc_buf_user buf;
	int installed_fds[EVDI_MAX_FDS];
};

static int evdi_queue_new_event(struct evdi_device *evdi,
				enum poll_event_type type, int poll_id,
				void *data, size_t size, struct drm_file *owner)
{
	struct evdi_event *event;

	if (poll_id < 0)
		poll_id = atomic_inc_return(&evdi->events.next_poll_id);

	event = evdi_event_alloc(evdi, type, poll_id, data, size, owner);
	if (!event)
		return -ENOMEM;

	evdi_event_queue(evdi, event);
	return 0;
}

static struct evdi_inflight_req *evdi_inflight_alloc(struct evdi_device *evdi,
						     struct drm_file *owner,
						     int type, int *out_id)
{
	struct evdi_inflight_req *req = evdi_inflight_req_alloc(evdi);
	int id;

	if (unlikely(!req))
		return NULL;

	req->type = type;
	req->owner = owner;

#ifdef EVDI_HAVE_XARRAY
	{
		u32 xid = READ_ONCE(evdi->inflight_next_id);

		if (unlikely(!xid))
			xid = 1;

#ifdef EVDI_HAVE_XA_ALLOC_CYCLIC
		if (xa_alloc_cyclic(&evdi->inflight_xa, &xid, req,
				    XA_LIMIT(1, INT_MAX),
				    &evdi->inflight_next_id, GFP_NOWAIT) != 0) {
			WRITE_ONCE(evdi->inflight_next_id, 1);
			xid = 1;
			if (xa_alloc_cyclic(&evdi->inflight_xa, &xid, req,
					    XA_LIMIT(1, INT_MAX),
					    &evdi->inflight_next_id,
					    GFP_NOWAIT) != 0)
				goto err;
		}
#else
		if (xa_alloc(&evdi->inflight_xa, &xid, req,
			     XA_LIMIT(xid, INT_MAX), GFP_NOWAIT) != 0) {
			if (xa_alloc(&evdi->inflight_xa, &xid, req,
				     XA_LIMIT(1, EVDI_MAX_INFLIGHT_REQUESTS),
				     GFP_NOWAIT) != 0)
				goto err;
		}
#endif
		evdi_inflight_req_get(req);
		id = (int)xid;
	}
#else
	spin_lock(&evdi->inflight_lock);
	id = idr_alloc(&evdi->inflight_idr, req, 1, EVDI_MAX_INFLIGHT_REQUESTS,
		       GFP_ATOMIC);
	spin_unlock(&evdi->inflight_lock);
	if (id < 0)
		goto err;
	evdi_inflight_req_get(req);
#endif
	*out_id = id;
	return req;

err:
	evdi_inflight_req_put(req);
	return NULL;
}

static struct evdi_inflight_req *evdi_inflight_take(struct evdi_device *evdi,
						    int id)
{
	struct evdi_inflight_req *req = NULL;

	if (unlikely(!evdi))
		return NULL;

#ifdef EVDI_HAVE_XARRAY
#ifdef EVDI_HAVE_ATOMIC_CMPXCHG_RELAXED
	req = xa_load(&evdi->inflight_xa, id);
	if (req &&
	    xa_cmpxchg(&evdi->inflight_xa, id, req, NULL, GFP_NOWAIT) != req)
		req = NULL;
#else
	unsigned long flags;

	xa_lock_irqsave(&evdi->inflight_xa, flags);
	req = xa_load(&evdi->inflight_xa, id);
	if (req)
		xa_erase(&evdi->inflight_xa, id);
	xa_unlock_irqrestore(&evdi->inflight_xa, flags);
#endif
#else
	spin_lock(&evdi->inflight_lock);
	req = idr_find(&evdi->inflight_idr, id);
	if (req)
		idr_remove(&evdi->inflight_idr, id);
	spin_unlock(&evdi->inflight_lock);
#endif
	return req;
}

void evdi_inflight_discard_owner(struct evdi_device *evdi,
				 struct drm_file *owner)
{
	struct evdi_inflight_req *req;

	if (unlikely(!evdi || !owner))
		return;

#ifdef EVDI_HAVE_XARRAY
	{
		struct evdi_inflight_req *batch[16];
		unsigned long index;
		int nr, i;

		do {
			nr = 0;
			xa_lock(&evdi->inflight_xa);
			xa_for_each(&evdi->inflight_xa, index, req) {
				if (req->owner == owner) {
					__xa_erase(&evdi->inflight_xa, index);
					batch[nr++] = req;
					if (nr == 16)
						break;
				}
			}
			xa_unlock(&evdi->inflight_xa);
			for (i = 0; i < nr; i++) {
				complete_all(&batch[i]->done);
				evdi_inflight_req_put(batch[i]);
				cond_resched();
			}
		} while (nr == 16);
	}
#else
	{
		struct evdi_inflight_req *batch[16];
		int nr, i, id;

		do {
			nr = id = 0;
			spin_lock(&evdi->inflight_lock);
			while (nr < 16) {
				req = idr_get_next(&evdi->inflight_idr, &id);
				if (!req)
					break;
				if (req->owner == owner) {
					idr_remove(&evdi->inflight_idr, id);
					batch[nr++] = req;
				}
				id++;
			}
			spin_unlock(&evdi->inflight_lock);
			for (i = 0; i < nr; i++) {
				complete_all(&batch[i]->done);
				evdi_inflight_req_put(batch[i]);
				cond_resched();
			}
		} while (nr == 16);
	}
#endif
}

int evdi_ioctl_connect(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_connect *cmd = data;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[0]);

	if (cmd->display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	if (!cmd->connected) {
		atomic_set(&evdi->events.stopping, 1);
		wake_up_interruptible(&evdi->events.wait_queue);
		WRITE_ONCE(evdi->displays[cmd->display_id].connected, false);
		{
			int i, any = 0;

			for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
				any |= READ_ONCE(evdi->displays[i].connected);
			if (!any)
				WRITE_ONCE(evdi->drm_client, NULL);
		}
		evdi_info("Device %d disconnected", evdi->dev_index);
		return 0;
	}

	if (evdi->drm_client && evdi->drm_client != file) {
		evdi_warn("Device %d forcefully disconnecting previous client",
			  evdi->dev_index);
		atomic_set(&evdi->events.stopping, 1);
		wake_up_interruptible(&evdi->events.wait_queue);
	}

	WRITE_ONCE(evdi->displays[cmd->display_id].connected, true);
	WRITE_ONCE(evdi->displays[cmd->display_id].width, cmd->width);
	WRITE_ONCE(evdi->displays[cmd->display_id].height, cmd->height);
	WRITE_ONCE(evdi->displays[cmd->display_id].refresh_rate,
		   cmd->refresh_rate);

	WRITE_ONCE(evdi->drm_client, file);
	evdi_info("Device %d connected: %ux%u@%uHz id:%u", evdi->dev_index,
		  cmd->width, cmd->height, cmd->refresh_rate, cmd->display_id);
	atomic_set(&evdi->events.stopping, 0);

	return 0;
}

int evdi_ioctl_poll(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_poll *cmd = data;
	struct evdi_event *event;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[1]);

	while (!(event = evdi_event_dequeue(evdi))) {
		int ret = evdi_event_wait(evdi, file);

		if (ret)
			return ret;
	}

	cmd->event = event->type;
	cmd->poll_id = event->poll_id;

	if (event->data_size > 0 && cmd->data) {
		if (copy_to_user(cmd->data, event->inline_data,
				 event->data_size)) {
			evdi_event_free(event);
			return -EFAULT;
		}
	}

	evdi_event_free(event);
	return 0;
}

int evdi_ioctl_gbm_get_buff(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_get_buff *cmd = data, evt_params;
	struct evdi_inflight_req *req;
	struct evdi_gralloc_buf_stack stack;
	struct evdi_gralloc_data *gralloc;
	int poll_id, i, ret;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[7]);

	req = evdi_inflight_alloc(evdi, file, get_buf, &poll_id);
	if (!req)
		return -ENOMEM;

	evt_params.id = cmd->id;
	evt_params.native_handle = NULL;

	if (evdi_queue_new_event(evdi, get_buf, poll_id, &evt_params,
				 sizeof(evt_params), file)) {
		struct evdi_inflight_req *tmp =
			evdi_inflight_take(evdi, poll_id);

		if (tmp)
			evdi_inflight_req_put(tmp);
		evdi_inflight_req_put(req);
		return -ENOMEM;
	}

	if (!wait_for_completion_interruptible_timeout(&req->done,
						       EVDI_WAIT_TIMEOUT)) {
		evdi_inflight_req_put(req);
		return -ETIMEDOUT;
	}

	gralloc = &req->reply.get_buf.gralloc_buf.gralloc;
	stack.buf.version = gralloc->version;
	stack.buf.numFds = gralloc->numFds;
	stack.buf.numInts = gralloc->numInts;

	if (gralloc->numInts > 0)
		memcpy(&stack.buf.data[gralloc->numFds], gralloc->data_ints,
		       sizeof(int) * gralloc->numInts);

	for (i = 0; i < gralloc->numFds; i++) {
		int fd = get_unused_fd_flags(O_RDWR);

		if (fd < 0) {
			while (--i >= 0)
				put_unused_fd(stack.installed_fds[i]);
			evdi_inflight_req_put(req);
			return fd;
		}
		stack.installed_fds[i] = stack.buf.data[i] = fd;
	}

	ret = copy_to_user(cmd->native_handle, &stack.buf,
			   sizeof(int) *
				   (3 + gralloc->numFds + gralloc->numInts)) ?
		      -EFAULT :
		      0;

	for (i = 0; i < gralloc->numFds; i++) {
		if (ret) {
			put_unused_fd(stack.installed_fds[i]);
			if (gralloc->data_files[i]) {
				fput(gralloc->data_files[i]);
				gralloc->data_files[i] = NULL;
			}
		} else if (gralloc->data_files[i]) {
			fd_install(stack.installed_fds[i],
				   gralloc->data_files[i]);
			gralloc->data_files[i] = NULL;
		}
	}

	evdi_inflight_req_put(req);
	return ret;
}

int evdi_ioctl_gbm_create_buff(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_create_buff *cmd = data, evt_params;
	struct evdi_inflight_req *req;
	int poll_id;
	long wret;

	if ((cmd->id && !evdi_access_ok(cmd->id, sizeof(int))) ||
	    (cmd->stride && !evdi_access_ok(cmd->stride, sizeof(int))))
		return -EFAULT;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[2]);

	req = evdi_inflight_alloc(evdi, file, create_buf, &poll_id);
	if (!req)
		return -ENOMEM;

	evt_params = *cmd;
	evt_params.id = evt_params.stride = NULL;

	if (evdi_queue_new_event(evdi, create_buf, poll_id, &evt_params,
				 sizeof(evt_params), file)) {
		struct evdi_inflight_req *tmp =
			evdi_inflight_take(evdi, poll_id);

		if (tmp)
			evdi_inflight_req_put(tmp);
		evdi_inflight_req_put(req);
		return -ENOMEM;
	}

	wret = wait_for_completion_interruptible_timeout(&req->done,
							 EVDI_WAIT_TIMEOUT);
	if (!wret) {
		evdi_inflight_req_put(req);
		return -ETIMEDOUT;
	}
	if (wret < 0) {
		evdi_inflight_req_put(req);
		return (int)wret;
	}

	if (cmd->id &&
	    copy_to_user(cmd->id, &req->reply.create.id, sizeof(int))) {
		evdi_inflight_req_put(req);
		return -EFAULT;
	}

	if (cmd->id && file->driver_priv) {
		struct evdi_file_priv *priv = file->driver_priv;
		struct evdi_buffer_entry *entry =
			kmalloc(sizeof(*entry), GFP_ATOMIC);

		if (entry) {
			entry->id = req->reply.create.id;
			llist_add(&entry->node, &priv->buffers);
		}
	}

	if (cmd->stride &&
	    copy_to_user(cmd->stride, &req->reply.create.stride, sizeof(int))) {
		evdi_inflight_req_put(req);
		return -EFAULT;
	}

	evdi_inflight_req_put(req);
	return 0;
}

int evdi_ioctl_get_buff_callback(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct drm_evdi_get_buff_callabck *cb = data;
	struct evdi_inflight_req *req =
		evdi_inflight_take(dev->dev_private, cb->poll_id);
	struct evdi_gralloc_data *gralloc;
	int i, fds_local[EVDI_MAX_FDS];

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[3]);

	if (!req)
		goto out;

	if (cb->numFds < 0 || cb->numInts < 0 || cb->numFds > EVDI_MAX_FDS ||
	    cb->numInts > EVDI_MAX_INTS)
		goto out_complete;

	gralloc = &req->reply.get_buf.gralloc_buf.gralloc;
	gralloc->version = cb->version;
	gralloc->numFds = gralloc->numInts = 0;

	if (cb->numInts && !copy_from_user(gralloc->data_ints, cb->data_ints,
					   sizeof(int) * cb->numInts))
		gralloc->numInts = cb->numInts;

	if (cb->numFds &&
	    !copy_from_user(fds_local, cb->fd_ints, sizeof(int) * cb->numFds)) {
		for (i = 0; i < cb->numFds; i++) {
			if (!(gralloc->data_files[i] = fget(fds_local[i]))) {
				while (--i >= 0)
					fput(gralloc->data_files[i]);
				goto out_complete;
			}
		}
		gralloc->numFds = cb->numFds;
	}

out_complete:
	complete_all(&req->done);
	evdi_inflight_req_put(req);
out:
	wake_up_interruptible(
		&((struct evdi_device *)dev->dev_private)->events.wait_queue);
	return 0;
}

int evdi_ioctl_destroy_buff_callback(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[4]);
	EVDI_PERF_INC64(&evdi_perf.callback_completions);
	wake_up_interruptible(
		&((struct evdi_device *)dev->dev_private)->events.wait_queue);
	return 0;
}

int evdi_ioctl_swap_callback(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[5]);
	EVDI_PERF_INC64(&evdi_perf.callback_completions);
	wake_up_interruptible(
		&((struct evdi_device *)dev->dev_private)->events.wait_queue);
	return 0;
}

int evdi_ioctl_create_buff_callback(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct drm_evdi_create_buff_callabck *cb = data;
	struct evdi_inflight_req *req =
		evdi_inflight_take(dev->dev_private, cb->poll_id);

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[6]);

	if (req) {
		req->reply.create.id = max_t(int, 0, cb->id);
		req->reply.create.stride = max_t(uint32_t, 0, cb->stride);
		complete_all(&req->done);
		evdi_inflight_req_put(req);
	}
	return 0;
}

int evdi_ioctl_gbm_del_buff(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	return evdi_queue_new_event(dev->dev_private, destroy_buf, -1,
				    &((struct drm_evdi_gbm_del_buff *)data)->id,
				    sizeof(int), file);
}

int evdi_queue_swap_event(struct evdi_device *evdi, int id, int display_id,
			  struct drm_file *owner)
{
	struct evdi_swap data = { .id = id, .display_id = display_id };

	return evdi_queue_new_event(evdi, swap_to, -1, &data, sizeof(data),
				    owner);
}

int evdi_queue_destroy_event(struct evdi_device *evdi, int id,
			     struct drm_file *owner)
{
	return evdi_queue_new_event(evdi, destroy_buf, -1, &id, sizeof(int),
				    owner);
}

int evdi_queue_crtc_state_event(struct evdi_device *evdi, int display_id,
				int mode, struct drm_file *owner)
{
	struct drm_evdi_crtc_state data = { .display_id = display_id,
					    .mode = mode };

	return evdi_queue_new_event(evdi, crtc_state, -1, &data, sizeof(data),
				    owner);
}
