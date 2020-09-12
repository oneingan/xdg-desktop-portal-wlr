#include "wlr_screencopy.h"
#include "wlr_screencast.h"

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"

static void wlr_frame_buffer_destroy(struct xdpw_screencast_instance *cast) {
	// Even though this check may be deemed unnecessary,
	// this has been found to cause SEGFAULTs, like this one:
	// https://github.com/emersion/xdg-desktop-portal-wlr/issues/50
	if (cast->xdpw_frames.screencopy_frame.data != NULL) {
		munmap(cast->xdpw_frames.screencopy_frame.data, cast->xdpw_frames.screencopy_frame.size);
		cast->xdpw_frames.screencopy_frame.data = NULL;
	}

	if (cast->xdpw_frames.screencopy_frame.buffer != NULL) {
		wl_buffer_destroy(cast->xdpw_frames.screencopy_frame.buffer);
		cast->xdpw_frames.screencopy_frame.buffer = NULL;
	}
}

void xdpw_wlr_screencopy_frame_free(struct xdpw_screencast_instance *cast) {
	zwlr_screencopy_frame_v1_destroy(cast->wlr_frame);
	cast->wlr_frame = NULL;
	if (cast->quit || cast->err) {
		wlr_frame_buffer_destroy(cast);
		logprint(TRACE, "xdpw: xdpw_frames.screencopy_frame buffer destroyed");
	}
	logprint(TRACE, "wlroots: frame destroyed");
}

static int anonymous_shm_open(void) {
	char name[] = "/xdpw-shm-XXXXXX";
	int retries = 100;

	do {
		randname(name + strlen(name) - 6);

		--retries;
		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

static struct wl_buffer *create_shm_buffer(struct xdpw_screencast_instance *cast,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	int size = stride * height;

	int fd = anonymous_shm_open();
	if (fd < 0) {
		logprint(ERROR, "wlroots: shm_open failed");
		return NULL;
	}

	int ret;
	while ((ret = ftruncate(fd, size)) == EINTR);

	if (ret < 0) {
		close(fd);
		logprint(ERROR, "wlroots: ftruncate failed");
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		logprint(ERROR, "wlroots: mmap failed: %m");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
	close(fd);
	struct wl_buffer *buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
	wl_shm_pool_destroy(pool);

	*data_out = data;
	return buffer;
}

static void wlr_frame_buffer_chparam(struct xdpw_screencast_instance *cast,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {

	logprint(DEBUG, "wlroots: reset buffer");
	cast->xdpw_frames.screencopy_frame.width = width;
	cast->xdpw_frames.screencopy_frame.height = height;
	cast->xdpw_frames.screencopy_frame.stride = stride;
	cast->xdpw_frames.screencopy_frame.size = stride * height;
	cast->xdpw_frames.screencopy_frame.format = format;
	wlr_frame_buffer_destroy(cast);
}

static void wlr_frame_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
	//struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: linux_dmabuf event handler");
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: buffer_done event handler");
	zwlr_screencopy_frame_v1_copy_with_damage(frame, cast->xdpw_frames.screencopy_frame.buffer);
	logprint(TRACE, "wlroots: frame copied");
}

static void wlr_frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: buffer event handler");
	cast->wlr_frame = frame;
	if (cast->xdpw_frames.screencopy_frame.width != width ||
			cast->xdpw_frames.screencopy_frame.height != height ||
			cast->xdpw_frames.screencopy_frame.stride != stride ||
			cast->xdpw_frames.screencopy_frame.format != format) {
		logprint(TRACE, "wlroots: buffer properties changed");
		wlr_frame_buffer_chparam(cast, format, width, height, stride);
	}

	if (cast->xdpw_frames.screencopy_frame.buffer == NULL) {
		logprint(DEBUG, "wlroots: create shm buffer");
		cast->xdpw_frames.screencopy_frame.buffer = create_shm_buffer(cast, format, width, height,
			stride, &cast->xdpw_frames.screencopy_frame.data);
	} else {
		logprint(TRACE,"wlroots: shm buffer exists");
	}

	if (cast->xdpw_frames.screencopy_frame.buffer == NULL) {
		logprint(ERROR, "wlroots: failed to create buffer");
		abort();
	}

	if (cast->ctx->state->screencast_version < 3) {
		wlr_frame_buffer_done(cast,frame);
	}
}

static void wlr_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t flags) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: flags event handler");
	cast->xdpw_frames.screencopy_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: ready event handler");

	cast->xdpw_frames.screencopy_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->xdpw_frames.screencopy_frame.tv_nsec = tv_nsec;

	if (!cast->quit && !cast->err && cast->pwr_stream_state) {
		pw_loop_signal_event(cast->ctx->state->pw_loop, cast->event);
		return ;
	}

	xdpw_wlr_frame_free(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: failed event handler");
	cast->err = true;

	xdpw_wlr_frame_free(cast);
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: damage event handler");

	cast->xdpw_frames.screencopy_frame.damage.x = x;
	cast->xdpw_frames.screencopy_frame.damage.y = y;
	cast->xdpw_frames.screencopy_frame.damage.width = width;
	cast->xdpw_frames.screencopy_frame.damage.height = height;
}

static const struct zwlr_screencopy_frame_v1_listener wlr_frame_listener = {
	.buffer = wlr_frame_buffer,
	.buffer_done = wlr_frame_buffer_done,
	.linux_dmabuf = wlr_frame_linux_dmabuf,
	.flags = wlr_frame_flags,
	.ready = wlr_frame_ready,
	.failed = wlr_frame_failed,
	.damage = wlr_frame_damage,
};

void xdpw_wlr_screencopy_register_cb(struct xdpw_screencast_instance *cast) {

	cast->frame_callback = zwlr_screencopy_manager_v1_capture_output(
		cast->ctx->screencopy_manager, cast->with_cursor, cast->target_output->output);

	zwlr_screencopy_frame_v1_add_listener(cast->frame_callback,
		&wlr_frame_listener, cast);
	logprint(TRACE, "wlroots: callbacks registered");
}

void wlr_registry_handle_add_screencopy(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct xdpw_screencast_context *ctx = data;

	if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		uint32_t version = SC_MANAGER_VERSION < ver ? SC_MANAGER_VERSION : ver;
		ctx->state->screencast_version = version;
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->screencopy_manager = wl_registry_bind(
			reg, id, &zwlr_screencopy_manager_v1_interface, version);
	}
}

int xdpw_wlr_screencopy_init(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	// make sure our wlroots supports screencopy protocol
	if (!ctx->screencopy_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zwlr_screencopy_manager_v1_interface.name);
		return -1;
	}

	return 0;
}

void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx) {
	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
}
