#ifndef SCREENCAST_COMMON_H
#define SCREENCAST_COMMON_H

#include <gbm.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <wayland-client-protocol.h>

#include "fps_limit.h"

// this seems to be right based on
// https://github.com/flatpak/xdg-desktop-portal/blob/309a1fc0cf2fb32cceb91dbc666d20cf0a3202c2/src/screen-cast.c#L955
#define XDP_CAST_PROTO_VER 4
#define XDP_CAST_DATA_VER 1

enum cursor_modes {
  HIDDEN = 1,
  EMBEDDED = 2,
  METADATA = 4,
};

enum source_types {
  MONITOR = 1,
  WINDOW = 2,
};

enum buffer_type {
  WL_SHM = 0,
  DMABUF = 1,
};

enum xdpw_chooser_types {
  XDPW_CHOOSER_DEFAULT,
  XDPW_CHOOSER_NONE,
  XDPW_CHOOSER_SIMPLE,
  XDPW_CHOOSER_DMENU,
};

enum xdpw_frame_state {
  XDPW_FRAME_STATE_NONE,
  XDPW_FRAME_STATE_STARTED,
  XDPW_FRAME_STATE_RENEG,
  XDPW_FRAME_STATE_FAILED,
  XDPW_FRAME_STATE_SUCCESS,
};

struct xdpw_output_chooser {
	enum xdpw_chooser_types type;
	char *cmd;
};

struct xdpw_frame_damage {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct xdpw_frame {
	bool y_invert;
	uint64_t tv_sec;
	uint32_t tv_nsec;
	struct xdpw_frame_damage damage;
	struct xdpw_buffer *xdpw_buffer;
	struct pw_buffer *pw_buffer;
};

struct xdpw_screencopy_frame_info {
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t stride;
	uint32_t format;
};

struct xdpw_buffer {
	struct wl_list link;
	enum buffer_type buffer_type;

	uint32_t width;
	uint32_t height;
	uint32_t format;
	int plane_count;

	int fd[4];
	uint32_t size[4];
	uint32_t stride[4];
	uint32_t offset[4];

	struct gbm_bo *bo;

	struct wl_buffer *buffer;
};

struct xdpw_format_modifier_pair {
	struct wl_list link;
	uint32_t fourcc;
	uint64_t modifier;
};

struct xdpw_screencast_context {

	// xdpw
	struct xdpw_state *state;

	// pipewire
	struct pw_context *pwr_context;
	struct pw_core *core;

	// wlroots
	struct wl_list output_list;
	struct wl_registry *registry;
	struct zwlr_screencopy_manager_v1 *screencopy_manager;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct wl_shm *shm;
	struct zwp_linux_dmabuf_v1 *linux_dmabuf;
	struct wl_list format_modifier_pairs;

	// gbm
	struct gbm_device *gbm;

	// sessions
	struct wl_list screencast_instances;
};

struct xdpw_screencast_instance {
	// list
	struct wl_list link;

	// xdpw
	uint32_t refcount;
	struct xdpw_screencast_context *ctx;
	bool initialized;
	struct xdpw_frame current_frame;
	enum xdpw_frame_state frame_state;
	struct wl_list buffer_list;
	bool avoid_dmabufs;

	// pipewire
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info_raw pwr_format;
	uint32_t seq;
	uint32_t node_id;
	bool pwr_stream_state;
	uint32_t framerate;

	// wlroots
	struct zwlr_screencopy_frame_v1 *frame_callback;
	struct xdpw_wlr_output *target_output;
	uint32_t max_framerate;
	struct zwlr_screencopy_frame_v1 *wlr_frame;
	struct xdpw_screencopy_frame_info screencopy_frame_info[2];
	bool with_cursor;
	int err;
	bool quit;
	enum buffer_type buffer_type;

	// fps limit
	struct fps_limit_state fps_limit;
};

struct xdpw_screencast_session_data {
	uint32_t version;
	struct xdpw_screencast_instance *screencast_instance;
	char *output_name;
	uint32_t cursor_mode;
	uint32_t persist_mode;
};

struct xdpw_wlr_output {
	struct wl_list link;
	uint32_t id;
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	char *make;
	char *model;
	char *name;
	int width;
	int height;
	float framerate;
};

void randname(char *buf);
struct gbm_device *xdpw_gbm_device_create(void);
struct xdpw_buffer *xdpw_buffer_create(struct xdpw_screencast_instance *cast,
	enum buffer_type buffer_type, struct xdpw_screencopy_frame_info *frame_info);
void xdpw_buffer_destroy(struct xdpw_buffer *buffer);
bool wlr_query_dmabuf_modifiers(struct xdpw_screencast_context *ctx, uint32_t drm_format,
		uint32_t num_modifiers, uint64_t *modifiers, uint32_t *max_modifiers);
enum wl_shm_format xdpw_format_wl_shm_from_drm_fourcc(uint32_t format);
uint32_t xdpw_format_drm_fourcc_from_wl_shm(enum wl_shm_format format);
enum spa_video_format xdpw_format_pw_from_drm_fourcc(uint32_t format);
enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format);

enum xdpw_chooser_types get_chooser_type(const char *chooser_type);
const char *chooser_type_str(enum xdpw_chooser_types chooser_type);
#endif /* SCREENCAST_COMMON_H */
