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
#include <linux/overflow.h>
#include <linux/file.h>

static inline void evdi_gem_object_put_local(struct drm_gem_object *obj)
{
	evdi_info("evdi_gem_object_put_local: entering with obj=%p", obj);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	evdi_info("evdi_gem_object_put_local: kernel >= 5.0.0, calling drm_gem_object_put");
	drm_gem_object_put(obj);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	evdi_info("evdi_gem_object_put_local: kernel >= 4.12.0, calling drm_gem_object_put_unlocked");
	drm_gem_object_put_unlocked(obj);
#else
	evdi_info("evdi_gem_object_put_local: kernel < 4.12.0, calling drm_gem_object_unreference_unlocked");
	drm_gem_object_unreference_unlocked(obj);
#endif
	evdi_info("evdi_gem_object_put_local: exiting");
}

static void evdi_fb_destroy(struct drm_framebuffer *fb)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	struct evdi_device *evdi = fb->dev ? fb->dev->dev_private : NULL;

	evdi_info("evdi_fb_destroy: entering with fb=%p", fb);
	evdi_info("evdi_fb_destroy: efb=%p, evdi=%p", efb, evdi);

	if (efb->obj) {
		evdi_info("evdi_fb_destroy: putting obj %p", efb->obj);
		evdi_gem_object_put_local(&efb->obj->base);
	} else {
		evdi_info("evdi_fb_destroy: no obj to put");
	}

	evdi_info("evdi_fb_destroy: calling drm_framebuffer_cleanup");
	drm_framebuffer_cleanup(fb);

	if (evdi && efb->gralloc_buf_id) {
		evdi_info("evdi_fb_destroy: queuing destroy event for gralloc_buf_id=%d", efb->gralloc_buf_id);
		evdi_queue_destroy_event(evdi, efb->gralloc_buf_id, efb->owner);
	} else {
		evdi_info("evdi_fb_destroy: no destroy event needed, evdi=%p, gralloc_buf_id=%d", evdi, efb->gralloc_buf_id);
	}

	evdi_info("evdi_fb_destroy: kfree efb=%p", efb);
	kfree(efb);
	evdi_info("evdi_fb_destroy: exiting");
}

static int evdi_fb_create_handle(struct drm_framebuffer *fb,
				 struct drm_file *file,
				 unsigned int *handle)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	int ret;

	evdi_info("evdi_fb_create_handle: entering with fb=%p, file=%p, handle=%p", fb, file, handle);
	evdi_info("evdi_fb_create_handle: efb=%p, efb->obj=%p", efb, efb->obj);

	if (!efb->obj) {
		evdi_info("evdi_fb_create_handle: no obj, returning -EINVAL");
		return -EINVAL;
	}

	evdi_info("evdi_fb_create_handle: calling drm_gem_handle_create");
	ret = drm_gem_handle_create(file, &efb->obj->base, handle);
	evdi_info("evdi_fb_create_handle: drm_gem_handle_create returned %d, handle=%u", ret, *handle);
	return ret;
}

const struct drm_framebuffer_funcs evdifb_funcs = {
	.destroy	= evdi_fb_destroy,
	.create_handle	= evdi_fb_create_handle,
};

static int evdi_fb_extract_gralloc_id(const struct drm_mode_fb_cmd2 *mode_cmd)
{
	int id;

	evdi_info("evdi_fb_extract_gralloc_id: entering with mode_cmd=%p", mode_cmd);

#if (KERNEL_VERSION(4, 15, 0) <= LINUX_VERSION_CODE)
	if (mode_cmd->modifier[0]) {
		id = (int)(mode_cmd->modifier[0] & 0x7fffffff);
		evdi_info("evdi_fb_extract_gralloc_id: extracted from modifier[0]=%llu, id=%d", mode_cmd->modifier[0], id);
		return id;
	}
#endif

	if (mode_cmd->handles[0] > 0xFFFF) {
		id = (int)mode_cmd->handles[0];
		evdi_info("evdi_fb_extract_gralloc_id: extracted from handles[0]=%u, id=%d", mode_cmd->handles[0], id);
		return id;
	}

	id = 0;
	evdi_info("evdi_fb_extract_gralloc_id: no gralloc id found, returning 0");
	return id;
}

static int evdi_fb_calc_size(const struct drm_mode_fb_cmd2 *mode_cmd,
			     u32 *out_pitch, size_t *out_size)
{
	u32 pitch, cpp;
	size_t last_lines, total, tail;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	const struct drm_format_info *info = drm_format_info(mode_cmd->pixel_format);
	if (!info || info->num_planes != 1)
		return -EINVAL;
	cpp = info->cpp[0];
#else
	cpp = drm_format_plane_cpp(mode_cmd->pixel_format, 0);
#endif

	if (!cpp)
		return -EINVAL;

	if (!mode_cmd->width || !mode_cmd->height)
		return -EINVAL;

#if defined(DRM_FORMAT_MOD_LINEAR)
	if (mode_cmd->modifier[0] && mode_cmd->modifier[0] != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
#endif

	if (mode_cmd->pitches[0]) {
		pitch = mode_cmd->pitches[0];
		if (pitch < mode_cmd->width * cpp)
			return -EINVAL;
	} else {
		pitch = mode_cmd->width * cpp;
	}

	if (mode_cmd->height == 0)
		return -EINVAL;

	if (check_mul_overflow((size_t)(mode_cmd->height - 1), (size_t)pitch, &last_lines))
		return -EOVERFLOW;

	tail = (size_t)mode_cmd->width * cpp;
	if (check_add_overflow((size_t)mode_cmd->offsets[0], last_lines, &total))
		return -EOVERFLOW;

	if (check_add_overflow(total, tail, &total))
		return -EOVERFLOW;

	*out_pitch = pitch;
	*out_size = total;
	return 0;
}

static struct evdi_gem_object *evdi_fb_acquire_bo(struct drm_device *dev,
						  struct drm_file *file,
						  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	size_t size;
	u32 validated_pitch;
	int ret;

	ret = evdi_fb_calc_size(mode_cmd, &validated_pitch, &size);
	if (ret)
		return NULL;

	return evdi_gem_alloc_object(dev, size);
}

static unsigned int evdi_fb_cpp(u32 format)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	const struct drm_format_info *info = drm_format_info(format);
	if (!info || info->num_planes != 1)
		return 0;
	return info->cpp[0];
#else
	return drm_format_plane_cpp(format, 0);
#endif
}

static int evdi_fb_init_core(struct drm_device *dev,
			     struct evdi_framebuffer *efb,
			     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_framebuffer *fb = &efb->base;
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	const struct drm_format_info *info = drm_format_info(mode_cmd->pixel_format);
	if (!info)
		return -EINVAL;
#endif

	fb->dev = dev;
	fb->width  = mode_cmd->width;
	fb->height = mode_cmd->height;
	fb->pitches[0] = mode_cmd->pitches[0] ?
		mode_cmd->pitches[0] :
		evdi_fb_cpp(mode_cmd->pixel_format) * mode_cmd->width;
	fb->offsets[0] = mode_cmd->offsets[0];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	fb->format = info;
#else
	fb->pixel_format = mode_cmd->pixel_format;
	fb->bits_per_pixel = evdi_fb_cpp(mode_cmd->pixel_format) * 8;
	fb->depth = 0;
#endif

#if defined(DRM_FORMAT_MOD_LINEAR) || (LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	fb->modifier = mode_cmd->modifier[0];
#else
	fb->modifier[0] = mode_cmd->modifier[0];
#endif
#endif

	fb->flags = 0;
	fb->funcs = &evdifb_funcs;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	fb->obj[0] = &efb->obj->base;
#endif

	ret = drm_framebuffer_init(dev, fb, &evdifb_funcs);
	return ret;
}

struct drm_framebuffer *evdi_fb_user_fb_create(struct drm_device *dev,
					       struct drm_file *file,
					       const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct evdi_framebuffer *efb;
	struct evdi_gem_object *bo;
	int ret, id = 0;
	struct file *memfd_file;
	loff_t pos = 0;
	ssize_t bytes_read;

	bo = evdi_fb_acquire_bo(dev, file, mode_cmd);
	if (!bo)
		return ERR_PTR(-ENOENT);

	efb = kzalloc(sizeof(*efb), GFP_KERNEL);
	if (!efb) {
		evdi_gem_object_put_local(&bo->base);
		return ERR_PTR(-ENOMEM);
	}

	efb->obj = bo;
	efb->owner = file;
	efb->active = true;
	memfd_file = fget(mode_cmd->handles[0]);
	if (memfd_file) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		bytes_read = kernel_read(memfd_file, &id, sizeof(id), &pos);
#else
		bytes_read = kernel_read(memfd_file, pos, (char *)&id, (unsigned long)sizeof(id));
#endif
		if (bytes_read == sizeof(id))
			efb->gralloc_buf_id = id;
		fput(memfd_file);
	}
	if (!efb->gralloc_buf_id)
		efb->gralloc_buf_id = evdi_fb_extract_gralloc_id(mode_cmd);

	ret = evdi_fb_init_core(dev, efb, mode_cmd);
	if (ret) {
		evdi_gem_object_put_local(&bo->base);
		kfree(efb);
		return ERR_PTR(ret);
	}
	return &efb->base;
}
