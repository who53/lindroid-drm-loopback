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

static const struct drm_mode_config_funcs evdi_mode_config_funcs = {
	.fb_create = evdi_fb_user_fb_create,
#if EVDI_HAVE_ATOMIC_HELPERS
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
#endif
};

static const uint32_t evdi_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static void evdi_do_pipe_update(struct drm_simple_display_pipe *pipe)
{
	struct drm_framebuffer *fb = pipe->plane.fb;
	struct evdi_device *evdi = pipe->plane.dev->dev_private;
	struct evdi_framebuffer *efb;

	if (!fb)
		return;

	efb = to_evdi_fb(fb);
	if (efb && efb->owner && efb->gralloc_buf_id)
		evdi_queue_swap_event(
			evdi, efb->gralloc_buf_id,
			evdi_connector_slot(evdi, pipe->connector), efb->owner);
}

static int evdi_crtc_page_flip(struct drm_crtc *crtc,
			       struct drm_framebuffer *fb,
			       struct drm_pending_vblank_event *event,
			       uint32_t flags)
{
	struct evdi_device *evdi = crtc->dev->dev_private;
	struct drm_plane *plane = crtc->primary;
	int i;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
		if (&evdi->pipe[i].crtc == crtc)
			break;

	if (i >= LINDROID_MAX_CONNECTORS)
		return -ENODEV;

	plane->fb = fb;

	evdi_do_pipe_update(&evdi->pipe[i]);

	if (event) {
		unsigned long flags;
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}

	return 0;
}

#if KERNEL_VERSION(4, 14, 0) <= LINUX_VERSION_CODE
static void evdi_pipe_update(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	if (state && old_state && old_state->fb == state->fb)
		return;
	evdi_do_pipe_update(pipe);
}
#else
static void evdi_pipe_update(struct drm_simple_display_pipe *pipe)
{
	evdi_do_pipe_update(pipe);
}
#endif

static void evdi_crtc_commit(struct drm_crtc *crtc)
{
	struct evdi_device *evdi = crtc->dev->dev_private;
	int i;
	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
		if (&evdi->pipe[i].crtc == crtc)
			break;

	if (i < LINDROID_MAX_CONNECTORS)
		evdi_pipe_update(&evdi->pipe[i]
#if KERNEL_VERSION(4, 14, 0) <= LINUX_VERSION_CODE
				 ,
				 NULL
#endif
		);
}

static void evdi_crtc_enable(struct drm_crtc *crtc)
{
	struct evdi_device *evdi = crtc->dev->dev_private;
	int i;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
		if (&evdi->pipe[i].crtc == crtc)
			break;

	if (i < LINDROID_MAX_CONNECTORS) {
		evdi_queue_crtc_state_event(evdi, i, 1, evdi->drm_client);
	}
}

static void evdi_crtc_disable(struct drm_crtc *crtc)
{
	struct evdi_device *evdi = crtc->dev->dev_private;
	int i;

	if (!crtc->dev->master) {
		return;
	}

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
		if (&evdi->pipe[i].crtc == crtc)
			break;

	if (i < LINDROID_MAX_CONNECTORS) {
		evdi_queue_crtc_state_event(evdi, i, 0, evdi->drm_client);
	}
}

static int evdi_crtc_set_config(struct drm_mode_set *set)
{
	if (!set->fb || set->num_connectors == 0) {
		evdi_crtc_disable(set->crtc);
		return 0;
	}

	evdi_crtc_enable(set->crtc);

	return 0;
}

static int evdi_cursor_set2(struct drm_crtc *crtc, struct drm_file *file,
			    uint32_t handle, uint32_t width, uint32_t height,
			    int32_t hot_x, int32_t hot_y)
{
	return 0;
}

static int evdi_cursor_move(struct drm_crtc *crtc, int x, int y)
{
	return 0;
}

static int evdi_crtc_gamma_set(struct drm_crtc *crtc, u16 *r, u16 *g, u16 *b,
			       uint32_t size)
{
	return 0;
}

static const struct drm_crtc_helper_funcs evdi_crtc_helper_funcs = {
	.commit = evdi_crtc_commit,
	.enable = evdi_crtc_enable,
	.disable = evdi_crtc_disable,
};

static const struct drm_crtc_funcs evdi_crtc_funcs = {
	.reset = NULL,
	.destroy = drm_crtc_cleanup,
	.set_config = evdi_crtc_set_config,
	.page_flip = evdi_crtc_page_flip,
	.cursor_set2 = evdi_cursor_set2,
	.cursor_move = evdi_cursor_move,
	.gamma_set = evdi_crtc_gamma_set,
};

static const struct drm_plane_funcs evdi_plane_funcs = {
	.update_plane = drm_plane_helper_update,
	.disable_plane = drm_plane_helper_disable,
	.destroy = drm_plane_cleanup,
	.reset = NULL,
};

static const struct drm_encoder_funcs evdi_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int evdi_modeset_init(struct drm_device *dev)
{
	struct evdi_device *evdi = dev->dev_private;
	int ret, i;

#if KERNEL_VERSION(4, 14, 0) <= LINUX_VERSION_CODE
	ret = drm_mode_config_init(dev);
	if (ret) {
		evdi_err("Failed to initialize mode config: %d", ret);
		return ret;
	}
#else
	drm_mode_config_init(dev);
#endif

	dev->mode_config.min_width = 640;
	dev->mode_config.min_height = 480;
	dev->mode_config.max_width = 8192;
	dev->mode_config.max_height = 8192;

	dev->mode_config.preferred_depth = 24;
	dev->mode_config.prefer_shadow = 1;

	dev->mode_config.funcs = &evdi_mode_config_funcs;

	ret = evdi_connector_init(dev, evdi);
	if (ret) {
		evdi_err("Failed to initialize connector: %d", ret);
		goto err_connector;
	}

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		struct drm_plane *plane = &evdi->pipe[i].plane;
		struct drm_crtc *crtc = &evdi->pipe[i].crtc;
		struct drm_encoder *encoder = &evdi->pipe[i].encoder;
		struct drm_connector *connector = evdi->connector[i];

		ret = drm_universal_plane_init(dev, plane, 0, &evdi_plane_funcs,
					       evdi_formats,
					       ARRAY_SIZE(evdi_formats),
					       DRM_PLANE_TYPE_PRIMARY, NULL);
		if (ret) {
			evdi_err("Failed to initialize plane[%d]: %d", i, ret);
			goto err_pipe;
		}

		drm_crtc_helper_add(crtc, &evdi_crtc_helper_funcs);
		ret = drm_crtc_init_with_planes(dev, crtc, plane, NULL,
						&evdi_crtc_funcs, NULL);
		if (ret) {
			evdi_err("Failed to initialize crtc[%d]: %d", i, ret);
			goto err_pipe;
		}

		encoder->possible_crtcs = 1 << drm_crtc_index(crtc);
		ret = drm_encoder_init(dev, encoder, &evdi_encoder_funcs,
				       DRM_MODE_ENCODER_NONE, NULL);
		if (ret) {
			evdi_err("Failed to initialize encoder[%d]: %d", i,
				 ret);
			goto err_pipe;
		}

		ret = drm_mode_connector_attach_encoder(connector, encoder);
		if (ret) {
			evdi_err("Failed to attach connector[%d]: %d", i, ret);
			goto err_pipe;
		}

		drm_mode_crtc_set_gamma_size(crtc, 256);
		evdi->pipe[i].connector = connector;
	}

	evdi_info("Modeset initialized for device %d", evdi->dev_index);
	return 0;

err_pipe:
	evdi_connector_cleanup(evdi);
err_connector:
	drm_mode_config_cleanup(dev);
	return ret;
}

void evdi_modeset_cleanup(struct drm_device *dev)
{
	struct evdi_device *evdi = dev->dev_private;
	int i;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		drm_encoder_cleanup(&evdi->pipe[i].encoder);
		drm_crtc_cleanup(&evdi->pipe[i].crtc);
		drm_plane_cleanup(&evdi->pipe[i].plane);
	}

	evdi_connector_cleanup(evdi);

	drm_mode_config_cleanup(dev);

	evdi_debug("Modeset cleaned up for device %d", evdi->dev_index);
}
