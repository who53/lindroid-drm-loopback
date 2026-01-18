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

atomic_t evdi_device_count = ATOMIC_INIT(0);

static int evdi_driver_open(struct drm_device *dev, struct drm_file *file)
{
	struct evdi_file_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	init_llist_head(&priv->buffers);
	file->driver_priv = priv;

	return 0;
}

static void evdi_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct evdi_file_priv *priv = file->driver_priv;
	struct llist_node *node;
	struct evdi_buffer_entry *entry;

	if (unlikely(!evdi))
		return;

	if (READ_ONCE(evdi->drm_client) == file)
		WRITE_ONCE(evdi->drm_client, NULL);

	evdi_inflight_discard_owner(evdi, file);
	evdi_event_cleanup_file(evdi, file);

	if (priv) {
		node = llist_del_all(&priv->buffers);
		while (node) {
			entry = llist_entry(node, struct evdi_buffer_entry,
					    node);
			node = node->next;
			if (file != evdi->drm_client && evdi->drm_client)
				evdi_queue_destroy_event(evdi, entry->id,
							 evdi->drm_client);
			kfree(entry);
		}
		kfree(priv);
	}
}

static int evdi_prime_fd_to_handle(struct drm_device *dev,
				   struct drm_file *file_priv, int prime_fd,
				   uint32_t *handle)
{
	if (!handle)
		return -EINVAL;

	*handle = (uint32_t)prime_fd;
	return 0;
}

static const struct file_operations evdi_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
};

#define EVDI_IOCTL_FLAGS (DRM_UNLOCKED | DRM_RENDER_ALLOW)

static const struct drm_ioctl_desc evdi_ioctls[] = {
	DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_ioctl_connect, EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_POLL, evdi_ioctl_poll, EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_CREATE_BUFF, evdi_ioctl_gbm_create_buff,
			  EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_GET_BUFF, evdi_ioctl_gbm_get_buff,
			  EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GET_BUFF_CALLBACK, evdi_ioctl_get_buff_callback,
			  EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_DESTROY_BUFF_CALLBACK,
			  evdi_ioctl_destroy_buff_callback, EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_SWAP_CALLBACK, evdi_ioctl_swap_callback,
			  EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_CREATE_BUFF_CALLBACK,
			  evdi_ioctl_create_buff_callback, EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_DEL_BUFF, evdi_ioctl_gbm_del_buff,
			  EVDI_IOCTL_FLAGS),
};

static struct drm_driver evdi_driver = {
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = evdi_prime_fd_to_handle,
	.open = evdi_driver_open,
	.postclose = evdi_driver_postclose,
	.fops = &evdi_fops,
	.ioctls = evdi_ioctls,
	.num_ioctls = ARRAY_SIZE(evdi_ioctls),
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME
#if EVDI_HAVE_ATOMIC_HELPERS
			   | DRIVER_ATOMIC
#endif
};

int evdi_device_init(struct evdi_device *evdi, struct platform_device *pdev)
{
	int i;

	evdi->dev_index = atomic_inc_return(&evdi_device_count) - 1;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		evdi->displays[i].connected = false;
		evdi->displays[i].width = 1920;
		evdi->displays[i].height = 1080;
		evdi->displays[i].refresh_rate = 60;
		evdi->connector[i] = NULL;
		evdi->displays[i].last_queued_buf_id = -1;
	}

#ifdef EVDI_HAVE_XARRAY
	xa_init_flags(&evdi->inflight_xa, XA_FLAGS_ALLOC);
	evdi->inflight_next_id = 1;
#else
	idr_init(&evdi->inflight_idr);
	spin_lock_init(&evdi->inflight_lock);
#endif

	evdi->pdev = pdev;

	return evdi_event_init(evdi);
}

void evdi_device_cleanup(struct evdi_device *evdi)
{
	struct evdi_inflight_req *req;

	if (!evdi)
		return;

	atomic_set(&evdi->events.stopping, 1);
	WRITE_ONCE(evdi->drm_client, NULL);

#ifdef EVDI_HAVE_XARRAY
	{
		unsigned long index;
		xa_for_each(&evdi->inflight_xa, index, req) {
			xa_erase(&evdi->inflight_xa, index);
			complete_all(&req->done);
			evdi_inflight_req_put(req);
		}
		xa_destroy(&evdi->inflight_xa);
	}
#else
	{
		int id;
		spin_lock(&evdi->inflight_lock);
		idr_for_each_entry(&evdi->inflight_idr, req, id) {
			idr_remove(&evdi->inflight_idr, id);
			complete_all(&req->done);
			evdi_inflight_req_put(req);
		}
		spin_unlock(&evdi->inflight_lock);
		idr_destroy(&evdi->inflight_idr);
	}
#endif

	evdi_event_cleanup(evdi);
}

static int evdi_platform_probe(struct platform_device *pdev)
{
	struct evdi_device *evdi;
	struct drm_device *ddev;
	int ret;

	evdi = kzalloc(sizeof(*evdi), GFP_KERNEL);
	if (!evdi)
		return -ENOMEM;

	ret = evdi_device_init(evdi, pdev);
	if (ret) {
		kfree(evdi);
		return ret;
	}

	ddev = drm_dev_alloc(&evdi_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		ret = PTR_ERR(ddev);
		goto err_init;
	}

	ddev->dev_private = evdi;
	evdi->ddev = ddev;

	ret = evdi_modeset_init(ddev);
	if (ret)
		goto err_drm;

#if EVDI_HAVE_ATOMIC_HELPERS
	drm_mode_config_reset(ddev);
#endif

	drm_vblank_init(ddev, LINDROID_MAX_CONNECTORS);

	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto err_modeset;

	platform_set_drvdata(pdev, evdi);
	return 0;

err_modeset:
	evdi_modeset_cleanup(ddev);
err_drm:
	drm_dev_put(ddev);
err_init:
	evdi_device_cleanup(evdi);
	kfree(evdi);
	return ret;
}

static int evdi_platform_remove(struct platform_device *pdev)
{
	struct evdi_device *evdi = platform_get_drvdata(pdev);

	drm_dev_unregister(evdi->ddev);

#if EVDI_HAVE_ATOMIC_HELPERS
	drm_atomic_helper_shutdown(evdi->ddev);
#endif

	evdi_modeset_cleanup(evdi->ddev);
	evdi_device_cleanup(evdi);
	drm_dev_put(evdi->ddev);
	kfree(evdi);

	return 0;
}

static struct platform_driver evdi_platform_driver = {
	.probe = evdi_platform_probe,
	.remove = evdi_platform_remove,
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init evdi_init(void)
{
	int ret;

	ret = evdi_event_system_init();
	if (ret)
		return ret;

	ret = platform_driver_register(&evdi_platform_driver);
	if (ret)
		goto err_event;

	ret = evdi_sysfs_init();
	if (ret)
		goto err_plat;

	return 0;

err_plat:
	platform_driver_unregister(&evdi_platform_driver);
err_event:
	evdi_event_system_cleanup();
	return ret;
}

static void __exit evdi_exit(void)
{
	evdi_sysfs_cleanup();
	platform_driver_unregister(&evdi_platform_driver);
	evdi_event_system_cleanup();
}

module_init(evdi_init);
module_exit(evdi_exit);

MODULE_AUTHOR("EVDI-Lindroid Project");
MODULE_DESCRIPTION("High-performance virtual display driver for Lindroid");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");
