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

static void evdi_fb_destroy(struct drm_framebuffer *fb)
{
	drm_framebuffer_cleanup(fb);
}

const struct drm_framebuffer_funcs evdifb_funcs = {
	.destroy = evdi_fb_destroy,
};

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
	const struct drm_format_info *info =
		drm_format_info(mode_cmd->pixel_format);
	if (!info)
		return -EINVAL;
#endif

	fb->dev = dev;
	fb->width = mode_cmd->width;
	fb->height = mode_cmd->height;
	fb->pitches[0] =
		mode_cmd->pitches[0] ?
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

#if defined(DRM_FORMAT_MOD_LINEAR) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	fb->modifier = mode_cmd->modifier[0];
#else
	fb->modifier[0] = mode_cmd->modifier[0];
#endif
#endif

	fb->flags = 0;
	fb->funcs = &evdifb_funcs;

	ret = drm_framebuffer_init(dev, fb, &evdifb_funcs);
	return ret;
}

struct drm_framebuffer *
evdi_fb_user_fb_create(struct drm_device *dev, struct drm_file *file,
		       const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct evdi_framebuffer *efb;
	int ret, id = 0;
	struct file *memfd_file;
	loff_t pos = 0;
	ssize_t bytes_read;

	efb = kzalloc(sizeof(*efb), GFP_ATOMIC);
	if (!efb) {
		return ERR_PTR(-ENOMEM);
	}

	efb->owner = file;
	efb->active = true;
	memfd_file = fget(mode_cmd->handles[0]);
	if (memfd_file) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		bytes_read = kernel_read(memfd_file, &id, sizeof(id), &pos);
#else
		bytes_read = kernel_read(memfd_file, pos, (char *)&id,
					 (unsigned long)sizeof(id));
#endif
		if (bytes_read == sizeof(id))
			efb->gralloc_buf_id = id;
		fput(memfd_file);
	}

	ret = evdi_fb_init_core(dev, efb, mode_cmd);
	if (ret) {
		kfree(efb);
		return ERR_PTR(ret);
	}
	return &efb->base;
}
