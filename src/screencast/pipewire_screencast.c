#include "pipewire_screencast.h"

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>

#include "wlr_screencast.h"
#include "xdpw.h"
#include "logger.h"

static void writeFrameData(void *pwFramePointer, void *wlrFramePointer,
		uint32_t height, uint32_t stride, bool inverted) {
	if (!inverted) {
		memcpy(pwFramePointer, wlrFramePointer, height * stride);
		return;
	}

	for (size_t i = 0; i < (size_t)height; ++i) {
		void *flippedWlrRowPointer = wlrFramePointer + ((height - i - 1) * stride);
		void *pwRowPointer = pwFramePointer + (i * stride);
		memcpy(pwRowPointer, flippedWlrRowPointer, stride);
	}

	return;
}

static void writeDmabufData(void *data, struct xdpw_screencast_instance *cast) {
	void *map_data = NULL;
	cast->xdpw_frames.screencopy_frame.data = gbm_bo_map(cast->xdpw_frames.screencopy_frame.bo,
			0, 0, cast->xdpw_frames.screencopy_frame.width, cast->xdpw_frames.screencopy_frame.height,
			GBM_BO_TRANSFER_READ, &cast->xdpw_frames.screencopy_frame.stride, &map_data);
	writeFrameData(data, cast->xdpw_frames.screencopy_frame.data, cast->xdpw_frames.screencopy_frame.height,
		cast->xdpw_frames.screencopy_frame.stride, cast->xdpw_frames.screencopy_frame.y_invert);
	gbm_bo_unmap(cast->xdpw_frames.screencopy_frame.bo, map_data);
}

void pwr_copy_screencast(struct spa_buffer *spa_buf, struct xdpw_screencast_instance *cast) {
	struct spa_meta_header *h;
	struct spa_data *d;

	d = spa_buf->datas;
	if ((d[0].data) == NULL) {
		logprint(TRACE, "pipewire: data pointer undefined");
		return;
	}
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*h)))) {
		h->pts = -1;
		h->flags = 0;
		h->seq = cast->seq++;
		h->dts_offset = 0;
	}

	switch (cast->type) {
	case XDPW_INSTANCE_SCP_SHM:
		d[0].type = SPA_DATA_MemPtr;
		d[0].maxsize = cast->xdpw_frames.screencopy_frame.size;
		d[0].mapoffset = 0;
		d[0].chunk->size = cast->xdpw_frames.screencopy_frame.size;
		d[0].chunk->stride = cast->xdpw_frames.screencopy_frame.stride;
		d[0].chunk->offset = 0;
		d[0].flags = 0;
		d[0].fd = -1;

		writeFrameData(d[0].data, cast->xdpw_frames.screencopy_frame.data, cast->xdpw_frames.screencopy_frame.height,
			cast->xdpw_frames.screencopy_frame.stride, cast->xdpw_frames.screencopy_frame.y_invert);

		logprint(TRACE, "pipewire: pointer %p", d[0].data);
		logprint(TRACE, "pipewire: size %d", d[0].maxsize);
		logprint(TRACE, "pipewire: stride %d", d[0].chunk->stride);
		logprint(TRACE, "pipewire: width %d", cast->xdpw_frames.screencopy_frame.width);
		logprint(TRACE, "pipewire: height %d", cast->xdpw_frames.screencopy_frame.height);
		logprint(TRACE, "pipewire: y_invert %d", cast->xdpw_frames.screencopy_frame.y_invert);
		break;
	case XDPW_INSTANCE_SCP_DMABUF:
		d[0].type = SPA_DATA_DmaBuf;
		d[0].maxsize = cast->xdpw_frames.screencopy_frame.size;
		d[0].mapoffset = cast->xdpw_frames.screencopy_frame.offset;
		d[0].flags = 0;
		d[0].fd = cast->xdpw_frames.screencopy_frame.fd;

		logprint(TRACE, "pipewire: fd %p", d[0].fd);
		logprint(TRACE, "pipewire: size %d", d[0].maxsize);
		logprint(TRACE, "pipewire: width %d", cast->xdpw_frames.screencopy_frame.width);
		logprint(TRACE, "pipewire: height %d", cast->xdpw_frames.screencopy_frame.height);
		break;
	case XDPW_INSTANCE_SCP_DMABUF2MemPtr:
		d[0].type = SPA_DATA_MemPtr;
		d[0].maxsize = cast->xdpw_frames.screencopy_frame.size;
		d[0].mapoffset = 0;
		d[0].chunk->size = cast->xdpw_frames.screencopy_frame.size;
		d[0].chunk->stride = cast->xdpw_frames.screencopy_frame.stride;
		d[0].chunk->offset = 0;
		d[0].flags = 0;
		d[0].fd = -1;

		writeDmabufData(d[0].data, cast);

		logprint(TRACE, "pipewire: pointer %p", d[0].data);
		logprint(TRACE, "pipewire: size %d", d[0].maxsize);
		logprint(TRACE, "pipewire: stride %d", d[0].chunk->stride);
		logprint(TRACE, "pipewire: width %d", cast->xdpw_frames.screencopy_frame.width);
		logprint(TRACE, "pipewire: height %d", cast->xdpw_frames.screencopy_frame.height);
		logprint(TRACE, "pipewire: y_invert %d", cast->xdpw_frames.screencopy_frame.y_invert);
		break;
	default:
		abort();
	}
	logprint(TRACE, "********************");

}

static void pwr_on_event(void *data, uint64_t expirations) {
	struct xdpw_screencast_instance *cast = data;
	struct pw_buffer *pw_buf;

	logprint(TRACE, "********************");
	logprint(TRACE, "pipewire: event fired");

	if ((pw_buf = pw_stream_dequeue_buffer(cast->stream)) == NULL) {
		logprint(WARN, "pipewire: out of buffers");
		return;
	}

	switch (cast->type) {
	case XDPW_INSTANCE_SCP_SHM:
	case XDPW_INSTANCE_SCP_DMABUF:
	case XDPW_INSTANCE_SCP_DMABUF2MemPtr:
		pwr_copy_screencast(pw_buf->buffer, cast);
		break;
	default:
		abort();
	}

	pw_stream_queue_buffer(cast->stream, pw_buf);

	xdpw_wlr_frame_free(cast);
}

static void pwr_handle_stream_state_changed(void *data,
		enum pw_stream_state old, enum pw_stream_state state, const char *error) {
	struct xdpw_screencast_instance *cast = data;
	cast->node_id = pw_stream_get_node_id(cast->stream);

	logprint(TRACE, "pipewire: state changed event handle");

	logprint(INFO, "pipewire: stream state changed to \"%s\"",
		pw_stream_state_as_string(state));
	logprint(INFO, "pipewire: node id is %d", cast->node_id);

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		cast->pwr_stream_state = true;
		break;
	default:
		cast->pwr_stream_state = false;
		break;
	}
}

static void pwr_handle_stream_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	struct xdpw_screencast_instance *cast = data;
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	logprint(TRACE, "pipewire: param changed event handle");

	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &cast->pwr_format);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(BUFFERS, 1, 32),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(cast->xdpw_frames.simple_frame.size),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(cast->xdpw_frames.simple_frame.stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(ALIGN));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(stream, params, 2);
}

static const struct pw_stream_events pwr_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pwr_handle_stream_state_changed,
	.param_changed = pwr_handle_stream_param_changed,
};

void xdpw_pwr_stream_init(struct xdpw_screencast_instance *cast) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	struct xdpw_state *state = ctx->state;

	pw_loop_enter(state->pw_loop);

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	char name[] = "xdpw-stream-XXXXXX";
	randname(name + strlen(name) - 6);
	cast->stream = pw_stream_new(ctx->core, name,
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			NULL));

	if (!cast->stream) {
		logprint(ERROR, "pipewire: failed to create stream");
		abort();
	}
	cast->pwr_stream_state = false;

	/* make an event to signal frame ready */
	cast->event =
		pw_loop_add_event(state->pw_loop, pwr_on_event, cast);
	logprint(DEBUG, "pipewire: registered event %p", cast->event);

	enum spa_video_format format = xdpw_format_pw(cast);
	enum spa_video_format format_without_alpha =
		xdpw_format_pw_strip_alpha(format);
	uint32_t n_formats = 1;
	if (format_without_alpha != SPA_VIDEO_FORMAT_UNKNOWN) {
		n_formats++;
	}

	logprint(DEBUG, "pipewire: Supported format %d", format);

	const struct spa_pod *param;
	switch (cast->type) {
	case XDPW_INSTANCE_SCP_SHM:
	case XDPW_INSTANCE_SCP_DMABUF2MemPtr:
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(n_formats + 1,
				format, format, format_without_alpha),
			SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
				&SPA_RECTANGLE(cast->xdpw_frames.simple_frame.width, cast->xdpw_frames.simple_frame.height),
				&SPA_RECTANGLE(1, 1),
				&SPA_RECTANGLE(4096, 4096)),
			// variable framerate
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
			SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
				&SPA_FRACTION(cast->framerate, 1),
				&SPA_FRACTION(1, 1),
				&SPA_FRACTION(cast->framerate, 1)));
		break;
	case XDPW_INSTANCE_SCP_DMABUF:
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(n_formats + 1,
				format, format, format_without_alpha),
			SPA_FORMAT_VIDEO_modifier,	SPA_POD_CHOICE_ENUM_Long(1,cast->xdpw_frames.screencopy_frame.fourcc),
			SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
				&SPA_RECTANGLE(cast->xdpw_frames.simple_frame.width, cast->xdpw_frames.simple_frame.height),
				&SPA_RECTANGLE(1, 1),
				&SPA_RECTANGLE(4096, 4096)),
			// variable framerate
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
			SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
				&SPA_FRACTION(cast->framerate, 1),
				&SPA_FRACTION(1, 1),
				&SPA_FRACTION(cast->framerate, 1)));
		break;
	default:
		abort();
	}

	pw_stream_add_listener(cast->stream, &cast->stream_listener,
		&pwr_stream_events, cast);

	pw_stream_connect(cast->stream,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		(PW_STREAM_FLAG_DRIVER |
			PW_STREAM_FLAG_MAP_BUFFERS),
		&param, 1);

}

int xdpw_pwr_core_connect(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	logprint(DEBUG, "pipewire: establishing connection to core");

	if (!ctx->pwr_context) {
		ctx->pwr_context = pw_context_new(state->pw_loop, NULL, 0);
		if (!ctx->pwr_context) {
			logprint(ERROR, "pipewire: failed to create context");
			return -1;
		}
	}

	if (!ctx->core) {
		ctx->core = pw_context_connect(ctx->pwr_context, NULL, 0);
		if (!ctx->core) {
			logprint(ERROR, "pipewire: couldn't connect to context");
			return -1;
		}
	}
	return 0;
}

void xdpw_pwr_stream_destroy(struct xdpw_screencast_instance *cast) {
	logprint(DEBUG, "pipewire: destroying stream");
	pw_stream_flush(cast->stream, false);
	pw_stream_disconnect(cast->stream);
	pw_stream_destroy(cast->stream);
	cast->stream = NULL;
}
