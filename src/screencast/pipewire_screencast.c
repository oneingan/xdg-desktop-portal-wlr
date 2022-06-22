#include "pipewire_screencast.h"

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>
#include <libdrm/drm_fourcc.h>

#include "wlr_screencast.h"
#include "xdpw.h"
#include "logger.h"

#define CURSOR_META_SIZE(width, height) \
	(sizeof(struct spa_meta_cursor) + \
	sizeof(struct spa_meta_bitmap) + \
	width * height * 4)

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

static struct spa_pod *build_buffer(struct spa_pod_builder *b, uint32_t blocks, uint32_t size,
		uint32_t stride, uint32_t datatype) {
	assert(blocks > 0);
	assert(datatype > 0);
	struct spa_pod_frame f[1];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(XDPW_PWR_BUFFERS, XDPW_PWR_BUFFERS_MIN, 32), 0);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(blocks), 0);
	if (size > 0) {
		spa_pod_builder_add(b, SPA_PARAM_BUFFERS_size, SPA_POD_Int(size), 0);
	}
	if (stride > 0) {
		spa_pod_builder_add(b, SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), 0);
	}
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_align, SPA_POD_Int(XDPW_PWR_ALIGN), 0);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(datatype), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *fixate_format(struct spa_pod_builder *b, enum spa_video_format format,
		uint32_t width, uint32_t height, uint32_t framerate, uint64_t *modifier)
{
	struct spa_pod_frame f[1];

	enum spa_video_format format_without_alpha = xdpw_format_pw_strip_alpha(format);

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	if (modifier || format_without_alpha == SPA_VIDEO_FORMAT_UNKNOWN) {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	} else {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
				SPA_POD_CHOICE_ENUM_Id(3, format, format, format_without_alpha), 0);
	}
	/* modifiers */
	if (modifier) {
		// implicit modifier
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, *modifier);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
		SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(framerate, 1),
			&SPA_FRACTION(1, 1),
			&SPA_FRACTION(framerate, 1)),
		0);
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *build_format(struct spa_pod_builder *b, enum spa_video_format format,
		uint32_t width, uint32_t height, uint32_t framerate,
		uint64_t *modifiers, int modifier_count) {
	struct spa_pod_frame f[2];
	int i, c;

	enum spa_video_format format_without_alpha = xdpw_format_pw_strip_alpha(format);

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	if (modifier_count > 0 || format_without_alpha == SPA_VIDEO_FORMAT_UNKNOWN) {
		// modifiers are defined only in combinations with their format
		// we should not announce the format without alpha
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	} else {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
				SPA_POD_CHOICE_ENUM_Id(3, format, format, format_without_alpha), 0);
	}
	/* modifiers */
	if (modifier_count > 0) {
		// build an enumeration of modifiers
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
		// modifiers from the array
		for (i = 0, c = 0; i < modifier_count; i++) {
			spa_pod_builder_long(b, modifiers[i]);
			if (c++ == 0)
				spa_pod_builder_long(b, modifiers[i]);
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
		SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(framerate, 1),
			&SPA_FRACTION(1, 1),
			&SPA_FRACTION(framerate, 1)),
		0);
	return spa_pod_builder_pop(b, &f[0]);
}

static bool build_modifierlist(struct xdpw_screencast_instance *cast,
		uint32_t drm_format, uint64_t **modifiers, uint32_t *modifier_count) {
	if (!wlr_query_dmabuf_modifiers(cast->ctx, drm_format, 0, NULL, modifier_count)) {
		*modifiers = NULL;
		*modifier_count = 0;
		return false;
	}
	if (*modifier_count == 0) {
		logprint(INFO, "wlroots: no modifiers available for format %u", drm_format);
		*modifiers = NULL;
		return true;
	}
	*modifiers = calloc(*modifier_count, sizeof(uint64_t));
	bool ret = wlr_query_dmabuf_modifiers(cast->ctx, drm_format, *modifier_count, *modifiers, modifier_count);
	logprint(INFO, "wlroots: num_modififiers %d", *modifier_count);
	return ret;
}

static uint32_t build_formats(struct spa_pod_builder *b, struct xdpw_screencast_instance *cast,
		const struct spa_pod *params[static 2]) {
	uint32_t param_count;
	uint32_t modifier_count;
	uint64_t *modifiers = NULL;

	if (!cast->avoid_dmabufs &&
			build_modifierlist(cast, cast->screencopy_frame_info[DMABUF].format, &modifiers, &modifier_count) && modifier_count > 0) {
		param_count = 2;
		params[0] = build_format(b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[DMABUF].format),
				cast->screencopy_frame_info[DMABUF].width, cast->screencopy_frame_info[DMABUF].height, cast->framerate,
				modifiers, modifier_count);
		params[1] = build_format(b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[WL_SHM].format),
				cast->screencopy_frame_info[WL_SHM].width, cast->screencopy_frame_info[WL_SHM].height, cast->framerate,
				NULL, 0);
	} else {
		param_count = 1;
		params[0] = build_format(b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[WL_SHM].format),
				cast->screencopy_frame_info[WL_SHM].width, cast->screencopy_frame_info[WL_SHM].height, cast->framerate,
				NULL, 0);
	}
	free(modifiers);
	return param_count;
}

void xdpw_pwr_dequeue_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: dequeueing buffer");

	assert(!cast->current_frame.pw_buffer);
	if ((cast->current_frame.pw_buffer = pw_stream_dequeue_buffer(cast->stream)) == NULL) {
		logprint(WARN, "pipewire: out of buffers");
		return;
	}

	cast->current_frame.xdpw_buffer = cast->current_frame.pw_buffer->user_data;
}

static void pwr_handle_cursor(struct spa_meta_cursor *cursor, struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: handle_cursor");
	if (!cast->xdpw_cursor.present) {
		cursor->id = 0;
		logprint(TRACE, "pipewire: handle_cursor: cursor hidden");
		return;
	}

	cursor->id = 1;
	cursor->hotspot.x = cast->xdpw_cursor.hotspot_x;
	cursor->hotspot.y = cast->xdpw_cursor.hotspot_y;
	cursor->position.x = cast->xdpw_cursor.position_x;
	cursor->position.y = cast->xdpw_cursor.position_y;
	if (cast->xdpw_cursor.damaged) {
		cursor->bitmap_offset = sizeof(struct spa_meta_cursor);

		struct spa_meta_bitmap *cursor_bitmap;
		cursor_bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);
		cursor_bitmap->format = xdpw_format_pw_from_drm_fourcc(cast->xdpw_cursor.xdpw_buffer->format);
		cursor_bitmap->size.width = cast->xdpw_cursor.xdpw_buffer->width;
		cursor_bitmap->size.height = cast->xdpw_cursor.xdpw_buffer->height;
		cursor_bitmap->stride = cast->xdpw_cursor.xdpw_buffer->stride[0];
		cursor_bitmap->offset = sizeof(struct spa_meta_bitmap);
		uint32_t *bitmap_data = SPA_MEMBER(cursor_bitmap, cursor_bitmap->offset, uint32_t);
		void *data = mmap(NULL, cast->xdpw_cursor.xdpw_buffer->size[0], PROT_READ | PROT_WRITE, MAP_SHARED, cast->xdpw_cursor.xdpw_buffer->fd[0], cast->xdpw_cursor.xdpw_buffer->offset[0]);
		if (data != MAP_FAILED) {
			writeFrameData(bitmap_data, data, cast->xdpw_cursor.xdpw_buffer->height, cast->xdpw_cursor.xdpw_buffer->stride[0], false);
			munmap(data, cast->xdpw_cursor.xdpw_buffer->size[0]);
		} else {
			logprint(WARN, "pipewire: failed to mmap cursor buffer");
			cursor_bitmap->offset = 0;
		}
		cast->xdpw_cursor.damaged = false;
	} else {
		logprint(TRACE, "pipewire: handle_cursor: cursor not damaged");
		cursor->bitmap_offset = 0;
	}
}

void xdpw_pwr_enqueue_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: enqueueing buffer");

	if (!cast->current_frame.pw_buffer) {
		logprint(WARN, "pipewire: no buffer to queue");
		goto done;
	}
	struct pw_buffer *pw_buf = cast->current_frame.pw_buffer;
	struct spa_buffer *spa_buf = pw_buf->buffer;
	struct spa_data *d = spa_buf->datas;

	bool buffer_corrupt = cast->frame_state != XDPW_FRAME_STATE_SUCCESS;

	if (cast->current_frame.y_invert) {
		//TODO: Flip buffer or set stride negative
		buffer_corrupt = true;
		cast->err = 1;
	}

	struct spa_meta_header *h;
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*h)))) {
		h->pts = -1;
		h->flags = buffer_corrupt ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
		h->seq = cast->seq++;
		h->dts_offset = 0;
	}

	if (buffer_corrupt) {
		for (uint32_t plane = 0; plane < spa_buf->n_datas; plane++) {
			d[plane].chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
		}
	} else {
		for (uint32_t plane = 0; plane < spa_buf->n_datas; plane++) {
			d[plane].chunk->flags = SPA_CHUNK_FLAG_NONE;
		}
	}

	struct spa_meta_cursor *cursor;
	if ((cursor = spa_buffer_find_meta_data(spa_buf, SPA_META_Cursor, sizeof(*cursor)))) {
		pwr_handle_cursor(cursor, cast);
	}

	logprint(TRACE, "********************");
	for (uint32_t plane = 0; plane < spa_buf->n_datas; plane++) {
		logprint(TRACE, "pipewire: plane %d", plane);
		logprint(TRACE, "pipewire: fd %u", d[plane].fd);
		logprint(TRACE, "pipewire: maxsize %d", d[plane].maxsize);
		logprint(TRACE, "pipewire: size %d", d[plane].chunk->size);
		logprint(TRACE, "pipewire: stride %d", d[plane].chunk->stride);
		logprint(TRACE, "pipewire: offset %d", d[plane].chunk->offset);
		logprint(TRACE, "pipewire: chunk flags %d", d[plane].chunk->flags);
	}
	logprint(TRACE, "pipewire: width %d", cast->current_frame.xdpw_buffer->width);
	logprint(TRACE, "pipewire: height %d", cast->current_frame.xdpw_buffer->height);
	logprint(TRACE, "pipewire: y_invert %d", cast->current_frame.y_invert);
	logprint(TRACE, "********************");

	pw_stream_queue_buffer(cast->stream, pw_buf);

done:
	cast->current_frame.xdpw_buffer = NULL;
	cast->current_frame.pw_buffer = NULL;
}

void pwr_update_stream_param(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: stream update parameters");
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	uint32_t n_params = build_formats(&b, cast, params);

	pw_stream_update_params(stream, params, n_params);
}

static void pwr_handle_stream_state_changed(void *data,
		enum pw_stream_state old, enum pw_stream_state state, const char *error) {
	struct xdpw_screencast_instance *cast = data;
	cast->node_id = pw_stream_get_node_id(cast->stream);

	logprint(INFO, "pipewire: stream state changed to \"%s\"",
		pw_stream_state_as_string(state));
	logprint(INFO, "pipewire: node id is %d", (int)cast->node_id);

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		cast->pwr_stream_state = true;
		break;
	case PW_STREAM_STATE_PAUSED:
		if (old == PW_STREAM_STATE_STREAMING) {
			xdpw_pwr_enqueue_buffer(cast);
		}
		// fall through
	default:
		cast->pwr_stream_state = false;
		break;
	}
}

static void pwr_handle_stream_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	logprint(TRACE, "pipewire: stream parameters changed");
	struct xdpw_screencast_instance *cast = data;
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[2048];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[3];
	uint32_t blocks;
	uint32_t data_type;

	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &cast->pwr_format);
	cast->framerate = (uint32_t)(cast->pwr_format.max_framerate.num / cast->pwr_format.max_framerate.denom);

	const struct spa_pod_prop *prop_modifier;
	if ((prop_modifier = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier)) != NULL) {
		cast->buffer_type = DMABUF;
		data_type = 1<<SPA_DATA_DmaBuf;
		assert(cast->pwr_format.format == xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[DMABUF].format));
		if ((prop_modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) > 0) {
			const struct spa_pod *pod_modifier = &prop_modifier->value;

			uint32_t n_modifiers = SPA_POD_CHOICE_N_VALUES(pod_modifier) - 1;
			uint64_t *modifiers = SPA_POD_CHOICE_VALUES(pod_modifier);
			modifiers++;
			uint32_t flags = GBM_BO_USE_RENDERING;
			uint64_t modifier;
			uint32_t n_params;

			struct gbm_bo *bo = gbm_bo_create_with_modifiers2(cast->ctx->gbm,
				cast->screencopy_frame_info[cast->buffer_type].width, cast->screencopy_frame_info[cast->buffer_type].height,
				cast->screencopy_frame_info[cast->buffer_type].format, modifiers, n_modifiers, flags);
			if (bo) {
				modifier = gbm_bo_get_modifier(bo);
				gbm_bo_destroy(bo);
				goto fixate_format;
			}

			logprint(INFO, "pipewire: unable to allocate a dmabuf with modifiers. Falling back to the old api");
			for (uint32_t i = 0; i < n_modifiers; i++) {
				switch (modifiers[i]) {
				case DRM_FORMAT_MOD_INVALID:
					flags = cast->ctx->state->config->screencast_conf.force_mod_linear ?
						GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR : GBM_BO_USE_RENDERING;
					break;
				default:
					continue;
				}
				bo = gbm_bo_create(cast->ctx->gbm,
					cast->screencopy_frame_info[cast->buffer_type].width, cast->screencopy_frame_info[cast->buffer_type].height,
					cast->screencopy_frame_info[cast->buffer_type].format, flags);
				if (bo) {
					modifier = gbm_bo_get_modifier(bo);
					gbm_bo_destroy(bo);
					goto fixate_format;
				}
			}

			logprint(WARN, "pipewire: unable to allocate a dmabuf. Falling back to shm");
			cast->avoid_dmabufs = true;

			n_params = build_formats(&b, cast, &params[0]);
			pw_stream_update_params(stream, params, n_params);
			return;

fixate_format:
			params[0] = fixate_format(&b, xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[cast->buffer_type].format),
					cast->screencopy_frame_info[cast->buffer_type].width, cast->screencopy_frame_info[cast->buffer_type].height, cast->framerate, &modifier);

			n_params = build_formats(&b, cast, &params[1]);
			n_params++;

			pw_stream_update_params(stream, params, n_params);
			return;
		}

		if (cast->pwr_format.modifier == DRM_FORMAT_MOD_INVALID) {
			blocks = 1;
		} else {
			blocks = gbm_device_get_format_modifier_plane_count(cast->ctx->gbm,
				cast->screencopy_frame_info[DMABUF].format, cast->pwr_format.modifier);
		}
	} else {
		cast->buffer_type = WL_SHM;
		blocks = 1;
		data_type = 1<<SPA_DATA_MemFd;
	}

	logprint(DEBUG, "pipewire: Format negotiated:");
	logprint(DEBUG, "pipewire: buffer_type: %u (%u)", cast->buffer_type, data_type);
	logprint(DEBUG, "pipewire: format: %u", cast->pwr_format.format);
	logprint(DEBUG, "pipewire: modifier: %lu", cast->pwr_format.modifier);
	logprint(DEBUG, "pipewire: size: (%u, %u)", cast->pwr_format.size.width, cast->pwr_format.size.height);
	logprint(DEBUG, "pipewire: max_framerate: (%u / %u)", cast->pwr_format.max_framerate.num, cast->pwr_format.max_framerate.denom);

	uint32_t n_params = 0;
	params[n_params++] = build_buffer(&b, blocks, cast->screencopy_frame_info[cast->buffer_type].size,
			cast->screencopy_frame_info[cast->buffer_type].stride, data_type);

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	if (cast->cursor_mode == METADATA) {
		params[n_params++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
			SPA_PARAM_META_size, SPA_POD_Int(CURSOR_META_SIZE(384, 384)));
	}

	pw_stream_update_params(stream, params, n_params);
}

static void pwr_handle_stream_add_buffer(void *data, struct pw_buffer *buffer) {
	struct xdpw_screencast_instance *cast = data;
	struct spa_data *d;

	logprint(DEBUG, "pipewire: add buffer event handle");

	d = buffer->buffer->datas;

	// Select buffer type from negotiation result
	if ((d[0].type & (1u << SPA_DATA_MemFd)) > 0) {
		assert(cast->buffer_type == WL_SHM);
		d[0].type = SPA_DATA_MemFd;
	} else if ((d[0].type & (1u << SPA_DATA_DmaBuf)) > 0) {
		assert(cast->buffer_type == DMABUF);
		d[0].type = SPA_DATA_DmaBuf;
	} else {
		logprint(ERROR, "pipewire: unsupported buffer type");
		cast->err = 1;
		return;
	}

	logprint(TRACE, "pipewire: selected buffertype %u", d[0].type);

	struct xdpw_buffer *xdpw_buffer = xdpw_buffer_create(cast, cast->buffer_type, &cast->screencopy_frame_info[cast->buffer_type]);
	if (xdpw_buffer == NULL) {
		logprint(ERROR, "pipewire: failed to create xdpw buffer");
		cast->err = 1;
		return;
	}
	wl_list_insert(&cast->buffer_list, &xdpw_buffer->link);
	buffer->user_data = xdpw_buffer;

	assert(xdpw_buffer->plane_count >= 0 && buffer->buffer->n_datas == (uint32_t)xdpw_buffer->plane_count);
	for (uint32_t plane = 0; plane < buffer->buffer->n_datas; plane++) {
		d[plane].maxsize = xdpw_buffer->size[plane];
		d[plane].mapoffset = 0;
		d[plane].chunk->size = xdpw_buffer->size[plane];
		d[plane].chunk->stride = xdpw_buffer->stride[plane];
		d[plane].chunk->offset = xdpw_buffer->offset[plane];
		d[plane].flags = 0;
		d[plane].fd = xdpw_buffer->fd[plane];
		d[plane].data = NULL;
		// clients have implemented to check chunk->size if the buffer is valid instead
		// of using the flags. Until they are patched we should use some arbitrary value.
		if (xdpw_buffer->buffer_type == DMABUF && d[plane].chunk->size == 0) {
			d[plane].chunk->size = 9; // This was choosen by a fair d20.
		}
	}
}

static void pwr_handle_stream_remove_buffer(void *data, struct pw_buffer *buffer) {
	struct xdpw_screencast_instance *cast = data;

	logprint(DEBUG, "pipewire: remove buffer event handle");

	struct xdpw_buffer *xdpw_buffer = buffer->user_data;
	if (xdpw_buffer) {
		xdpw_buffer_destroy(xdpw_buffer);
	}
	if (cast->current_frame.pw_buffer == buffer) {
		cast->current_frame.pw_buffer = NULL;
	}
	for (uint32_t plane = 0; plane < buffer->buffer->n_datas; plane++) {
		buffer->buffer->datas[plane].fd = -1;
	}
	buffer->user_data = NULL;
}

static void pwr_handle_stream_on_process(void *data) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "pipewire: on process event handle");

	if (!cast->pwr_stream_state) {
		logprint(INFO, "pipewire: not streaming");
		return;
	}

	if (cast->current_frame.pw_buffer) {
		logprint(WARN, "pipewire: buffer already exported");
		return;
	}

	xdpw_pwr_dequeue_buffer(cast);
	if (!cast->current_frame.pw_buffer) {
		logprint(WARN, "pipewire: unable to export buffer");
		return;
	}
	if (cast->seq > 0) {
		uint64_t delay_ns = fps_limit_measure_end(&cast->fps_limit, cast->framerate);
		if (delay_ns > 0) {
			xdpw_add_timer(cast->ctx->state, delay_ns,
				(xdpw_event_loop_timer_func_t) xdpw_wlr_handle_frame, cast);
			return;
		}
	}
	xdpw_wlr_handle_frame(cast);
}

static const struct pw_stream_events pwr_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pwr_handle_stream_state_changed,
	.param_changed = pwr_handle_stream_param_changed,
	.add_buffer = pwr_handle_stream_add_buffer,
	.remove_buffer = pwr_handle_stream_remove_buffer,
	.process = pwr_handle_stream_on_process,
};

void xdpw_pwr_stream_create(struct xdpw_screencast_instance *cast) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	struct xdpw_state *state = ctx->state;

	pw_loop_enter(state->pw_loop);

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];

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

	uint32_t param_count = build_formats(&b, cast, params);

	pw_stream_add_listener(cast->stream, &cast->stream_listener,
		&pwr_stream_events, cast);

	pw_stream_connect(cast->stream,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		PW_STREAM_FLAG_ALLOC_BUFFERS,
		params, param_count);
}

void xdpw_pwr_stream_destroy(struct xdpw_screencast_instance *cast) {
	if (!cast->stream) {
		return;
	}

	logprint(DEBUG, "pipewire: destroying stream");
	pw_stream_flush(cast->stream, false);
	pw_stream_disconnect(cast->stream);
	pw_stream_destroy(cast->stream);
	cast->stream = NULL;
}

int xdpw_pwr_context_create(struct xdpw_state *state) {
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

void xdpw_pwr_context_destroy(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	logprint(DEBUG, "pipewire: disconnecting fom core");

	if (ctx->core) {
		pw_core_disconnect(ctx->core);
		ctx->core = NULL;
	}

	if (ctx->pwr_context) {
		pw_context_destroy(ctx->pwr_context);
		ctx->pwr_context = NULL;
	}
}
