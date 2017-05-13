#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wayland-server.h>

#include <wlr/session.h>
#include <wlr/common/list.h>

#include "backend.h"
#include "backend/drm/backend.h"
#include "backend/drm/drm.h"
#include "backend/drm/udev.h"
#include "common/log.h"

static bool wlr_drm_backend_init(struct wlr_backend_state *state) {
	wlr_drm_scan_connectors(state);
	return true;
}

static void wlr_drm_backend_destroy(struct wlr_backend_state *state) {
	if (!state) {
		return;
	}
	// TODO: free outputs in shared backend code
	wlr_drm_renderer_free(&state->renderer);
	wlr_udev_free(&state->udev);
	wlr_session_close_file(state->session, state->fd);
	wlr_session_finish(state->session);
	wl_event_source_remove(state->drm_event);
	free(state);
}

static struct wlr_backend_impl backend_impl = {
	.init = wlr_drm_backend_init,
	.destroy = wlr_drm_backend_destroy
};

static void device_paused(struct wl_listener *listener, void *data) {
	struct wlr_backend_state *backend = wl_container_of(listener, backend, device_paused);

	// TODO: Actually pause the renderer or something.
	// We currently just expect it to fail its next pageflip.

	(void)backend;
}

static void device_resumed(struct wl_listener *listener, void *data) {
	struct wlr_backend_state *drm = wl_container_of(listener, drm, device_paused);
	int *new_fd = data;

	close(drm->fd);
	drm->fd = *new_fd;
	drm->renderer.fd = *new_fd;

	for (size_t i = 0; i < drm->outputs->length; ++i) {
		struct wlr_output_state *output = drm->outputs->items[i];
		wlr_drm_output_draw_blank(output);
	}
}

struct wlr_backend *wlr_drm_backend_create(struct wl_display *display,
		struct wlr_session *session) {
	struct wlr_backend_state *state = calloc(1, sizeof(struct wlr_backend_state));
	if (!state) {
		wlr_log(L_ERROR, "Allocation failed: %s", strerror(errno));
		return NULL;
	}

	struct wlr_backend *backend = wlr_backend_create(&backend_impl, state);
	if (!backend) {
		wlr_log(L_ERROR, "Allocation failed: %s", strerror(errno));
		return NULL;
	}

	state->backend = backend;
	state->session = session;
	state->outputs = list_create();
	if (!state->outputs) {
		wlr_log(L_ERROR, "Failed to allocate list");
		goto error_backend;
	}

	if (!wlr_udev_init(display, &state->udev)) {
		wlr_log(L_ERROR, "Failed to start udev");
		goto error_list;
	}

	state->fd = wlr_udev_find_gpu(&state->udev, state->session);
	if (state->fd == -1) {
		wlr_log(L_ERROR, "Failed to open DRM device");
		goto error_udev;
	}

	struct wl_event_loop *event_loop = wl_display_get_event_loop(display);

	state->drm_event = wl_event_loop_add_fd(event_loop, state->fd,
		WL_EVENT_READABLE, wlr_drm_event, NULL);
	if (!state->drm_event) {
		wlr_log(L_ERROR, "Failed to create DRM event source");
		goto error_fd;
	}

	wl_list_init(&state->device_paused.link);
	wl_list_init(&state->device_paused.link);

	state->device_paused.notify = device_paused;
	state->device_resumed.notify = device_resumed;

	wl_signal_add(&session->device_paused, &state->device_paused);
	wl_signal_add(&session->device_resumed, &state->device_resumed);

	// TODO: what is the difference between the per-output renderer and this
	// one?
	if (!wlr_drm_renderer_init(&state->renderer, state->fd)) {
		wlr_log(L_ERROR, "Failed to initialize renderer");
		goto error_event;
	}

	return backend;

error_event:
	wl_event_source_remove(state->drm_event);
error_fd:
	wlr_session_close_file(state->session, state->fd);
error_udev:
	wlr_udev_free(&state->udev);
error_list:
	list_free(state->outputs);
error_backend:
	free(state);
	free(backend);
	return NULL;
}

void wlr_drm_backend_dpms(struct wlr_backend *backend, bool screen_on) {
	struct wlr_backend_state *state = backend->state;
	for (size_t i = 0; i < state->outputs->length; ++i) {
		struct wlr_output_state *output = state->outputs->items[i];
		wlr_drm_output_dpms(state->fd, output, screen_on);
	}
}