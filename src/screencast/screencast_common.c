#include "screencast_common.h"
#include <assert.h>

void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		assert(buf[i] == 'X');
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

enum spa_video_format xdpw_format_pw_from_wl_shm(
		enum wl_shm_format format) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case WL_SHM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case WL_SHM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case WL_SHM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case WL_SHM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case WL_SHM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case WL_SHM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case WL_SHM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	case WL_SHM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	default:
		abort();
	}
}

enum spa_video_format xdpw_format_pw_from_dmabuf(uint32_t fourcc) {
	switch (fourcc) {
	case GBM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	case GBM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case GBM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case GBM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case GBM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case GBM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case GBM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case GBM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case GBM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

enum spa_video_format xdpw_format_pw(
		struct xdpw_screencast_instance *cast) {
	switch (cast->type) {
	case XDPW_INSTANCE_SCP_SHM:
		return xdpw_format_pw_from_wl_shm(cast->xdpw_frames.screencopy_frame.format);
	case XDPW_INSTANCE_SCP_DMABUF:
		return xdpw_format_pw_from_dmabuf(cast->xdpw_frames.screencopy_frame.fourcc);
	default:
		abort();
	}
}

enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return SPA_VIDEO_FORMAT_BGRx;
	case SPA_VIDEO_FORMAT_ABGR:
		return SPA_VIDEO_FORMAT_xBGR;
	case SPA_VIDEO_FORMAT_RGBA:
		return SPA_VIDEO_FORMAT_RGBx;
	case SPA_VIDEO_FORMAT_ARGB:
		return SPA_VIDEO_FORMAT_xRGB;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

enum spa_data_type xdpw_datatype_pw(
		enum xdpw_instance_type type) {
	switch (type) {
	case XDPW_INSTANCE_SCP_SHM:
		return SPA_DATA_MemPtr;
	case XDPW_INSTANCE_SCP_DMABUF:
		return SPA_DATA_DmaBuf;
	default:
		return SPA_DATA_Invalid;
	}
}
