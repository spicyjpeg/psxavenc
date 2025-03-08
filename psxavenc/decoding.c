/*
psxavenc: MDEC video + SPU/XA-ADPCM audio encoder frontend

Copyright (c) 2019, 2020 Adrian "asie" Siekierka
Copyright (c) 2019 Ben "GreaseMonkey" Russell
Copyright (c) 2023, 2025 spicyjpeg

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/avdct.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "args.h"
#include "decoding.h"
#include "ringbuf.h"

#define BUFFERED_AUDIO_SAMPLES 0x4000
#define BUFFERED_VIDEO_FRAMES  0x20

// Each audio packet in the input stream can have up to 4096 samples after
// resampling.
#define RESAMPLE_BUFFER_SIZE 0x1000

static bool decode_frame(AVCodecContext *codec, AVFrame *frame, int *frame_size, AVPacket *packet) {
	if (packet != NULL) {
		if (avcodec_send_packet(codec, packet) != 0)
			return false;
	}

	int ret = avcodec_receive_frame(codec, frame);

	if (ret >= 0) {
		*frame_size = ret;
		return true;
	}
	if (ret == AVERROR(EAGAIN))
		return true;

	return false;
}

bool open_av_data(decoder_t *decoder, const args_t *args, int flags) {
	init_ring_buffer(&(decoder->audio_samples), 1, 0);
	init_ring_buffer(&(decoder->video_frames), 1, 0);

	decoder->video_width = args->video_width;
	decoder->video_height = args->video_height;
	decoder->video_fps_num = args->str_fps_num;
	decoder->video_fps_den = args->str_fps_den;
	decoder->end_of_input = false;

	decoder_state_t *av = &(decoder->state);

	av->video_next_pts = 0.0;
	av->frame = NULL;
	av->audio_stream_index = -1;
	av->video_stream_index = -1;
	av->format = NULL;
	av->audio_stream = NULL;
	av->video_stream = NULL;
	av->audio_codec_context = NULL;
	av->video_codec_context = NULL;
	av->resampler = NULL;
	av->scaler = NULL;
	av->resample_buffer = NULL;

	if (args->flags & FLAG_QUIET)
		av_log_set_level(AV_LOG_QUIET);

	av->format = avformat_alloc_context();

	if (avformat_open_input(&(av->format), args->input_file, NULL, NULL))
		return false;
	if (avformat_find_stream_info(av->format, NULL) < 0)
		return false;

	if (flags & DECODER_USE_AUDIO) {
		for (int i = 0; i < av->format->nb_streams; i++) {
			if (av->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				if (av->audio_stream_index >= 0) {
					fprintf(stderr, "Input file must have a single audio track\n");
					return false;
				}
				av->audio_stream_index = i;
			}
		}

		if ((flags & DECODER_AUDIO_REQUIRED) && av->audio_stream_index == -1) {
			fprintf(stderr, "Input file has no audio data\n");
			return false;
		}
	}

	if (flags & DECODER_USE_VIDEO) {
		for (int i = 0; i < av->format->nb_streams; i++) {
			if (av->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				if (av->video_stream_index >= 0) {
					fprintf(stderr, "Input file must have a single video track\n");
					return false;
				}
				av->video_stream_index = i;
			}
		}

		if ((flags & DECODER_VIDEO_REQUIRED) && av->video_stream_index == -1) {
			fprintf(stderr, "Input file has no video data\n");
			return false;
		}
	}

	av->audio_stream = (av->audio_stream_index != -1 ? av->format->streams[av->audio_stream_index] : NULL);
	av->video_stream = (av->video_stream_index != -1 ? av->format->streams[av->video_stream_index] : NULL);

	if (av->audio_stream != NULL) {
		const AVCodec *codec = avcodec_find_decoder(av->audio_stream->codecpar->codec_id);
		av->audio_codec_context = avcodec_alloc_context3(codec);

		if (av->audio_codec_context == NULL)
			return false;
		if (avcodec_parameters_to_context(av->audio_codec_context, av->audio_stream->codecpar) < 0)
			return false;
		if (avcodec_open2(av->audio_codec_context, codec, NULL) < 0)
			return false;

		AVChannelLayout layout;
		layout.nb_channels = args->audio_channels;

		if (args->audio_channels == 1) {
			layout.order = AV_CHANNEL_ORDER_NATIVE;
			layout.u.mask = AV_CH_LAYOUT_MONO;
		} else if (args->audio_channels == 2) {
			layout.order = AV_CHANNEL_ORDER_NATIVE;
			layout.u.mask = AV_CH_LAYOUT_STEREO;
		} else {
			layout.order = AV_CHANNEL_ORDER_UNSPEC;
		}

		if (!(args->flags & FLAG_QUIET)) {
			if (args->audio_channels > av->audio_codec_context->ch_layout.nb_channels)
				fprintf(stderr, "Warning: input file has less than %d channels\n", args->audio_channels);
		}

		if (swr_alloc_set_opts2(
			&av->resampler,
			&layout,
			AV_SAMPLE_FMT_S16,
			args->audio_frequency,
			&av->audio_codec_context->ch_layout,
			av->audio_codec_context->sample_fmt,
			av->audio_codec_context->sample_rate,
			0,
			NULL
		) < 0) {
			return false;
		}
		if (args->swresample_options) {
			if (av_opt_set_from_string(av->resampler, args->swresample_options, NULL, "=", ":,") < 0)
				return false;
		}
		if (swr_init(av->resampler) < 0)
			return false;

		init_ring_buffer(&(decoder->audio_samples), args->audio_channels * sizeof(int16_t), BUFFERED_AUDIO_SAMPLES);

		av->resample_buffer = malloc(args->audio_channels * sizeof(int16_t) * RESAMPLE_BUFFER_SIZE);
	}

	if (av->video_stream != NULL) {
		const AVCodec *codec = avcodec_find_decoder(av->video_stream->codecpar->codec_id);
		av->video_codec_context = avcodec_alloc_context3(codec);

		if (av->video_codec_context == NULL)
			return false;
		if (avcodec_parameters_to_context(av->video_codec_context, av->video_stream->codecpar) < 0)
			return false;
		if (avcodec_open2(av->video_codec_context, codec, NULL) < 0)
			return false;

		if (!(args->flags & FLAG_QUIET)) {
			if (
				decoder->video_width > av->video_codec_context->width ||
				decoder->video_height > av->video_codec_context->height
			)
				fprintf(stderr, "Warning: input file has resolution lower than %dx%d\n", decoder->video_width, decoder->video_height);
		}

		if (!(args->flags & FLAG_BS_IGNORE_ASPECT)) {
			// Reduce the provided size so that it matches the input file's
			// aspect ratio.
			double src_ratio = (double)av->video_codec_context->width / (double)av->video_codec_context->height;
			double dst_ratio = (double)decoder->video_width / (double)decoder->video_height;

			if (src_ratio < dst_ratio) {
				decoder->video_width = (int)((double)decoder->video_height * src_ratio + 15.0) & ~15;
			} else {
				decoder->video_height = (int)((double)decoder->video_width / src_ratio + 15.0) & ~15;
			}
		}

		av->scaler = sws_getContext(
			av->video_codec_context->width,
			av->video_codec_context->height,
			av->video_codec_context->pix_fmt,
			decoder->video_width,
			decoder->video_height,
			AV_PIX_FMT_NV21,
			SWS_BICUBIC,
			NULL,
			NULL,
			NULL
		);
		if (av->scaler == NULL)
			return false;
		if (sws_setColorspaceDetails(
			av->scaler,
			sws_getCoefficients(av->video_codec_context->colorspace),
			av->video_codec_context->color_range == AVCOL_RANGE_JPEG,
			sws_getCoefficients(SWS_CS_ITU601),
			true,
			0,
			1 << 16,
			1 << 16
		) < 0)
			return false;
		if (args->swscale_options) {
			if (av_opt_set_from_string(av->scaler, args->swscale_options, NULL, "=", ":,") < 0)
				return false;
		}

		// 1 full-resolution Y plane + 2 interleaved 1/4 resolution Cr/Cb planes
		init_ring_buffer(&(decoder->video_frames), decoder->video_width * decoder->video_height * 3 / 2, BUFFERED_VIDEO_FRAMES);
	}

	av->frame = av_frame_alloc();

	if (av->frame == NULL)
		return false;

	return true;
}

static void poll_av_packet_audio(decoder_t *decoder, AVPacket *packet) {
	decoder_state_t *av = &(decoder->state);

	int frame_size;

	if (!decode_frame(av->audio_codec_context, av->frame, &frame_size, packet))
		return;

	int sample_count = swr_get_out_samples(av->resampler, av->frame->nb_samples);

	if (sample_count == 0)
		return;

	assert(sample_count <= RESAMPLE_BUFFER_SIZE);

	uint8_t *ptr = (uint8_t *)av->resample_buffer;
	sample_count = swr_convert(
		av->resampler,
		&ptr,
		sample_count,
		(const uint8_t **)av->frame->data,
		av->frame->nb_samples
	);

	// Copy as many contiguous samples as possible at a time into the FIFO.
	while (sample_count > 0) {
		void *span = ring_buffer_get_tail(&(decoder->audio_samples), 0);
		int span_count = ring_buffer_get_contiguous_span(&(decoder->audio_samples));

		if (span_count > sample_count)
			span_count = sample_count;

		memcpy(span, ptr, decoder->audio_samples.item_size * span_count);
		ring_buffer_append(&(decoder->audio_samples), span_count);
		ptr += decoder->audio_samples.item_size * span_count;
		sample_count -= span_count;
	}
}

static void poll_av_packet_video(decoder_t *decoder, AVPacket *packet) {
	decoder_state_t *av = &(decoder->state);
	int frame_size;

	if (!decode_frame(av->video_codec_context, av->frame, &frame_size, packet))
		return;
	if (!av->frame->width || !av->frame->height || !av->frame->data[0])
		return;

	double frame_time = (double)decoder->video_fps_den / (double)decoder->video_fps_num;
	double pts = (double)av->frame->pts * (double)av->video_stream->time_base.num / (double)av->video_stream->time_base.den;

#if 0
	// Some files seem to have timestamps starting from a negative value
	// (but otherwise valid) for whatever reason.
	if (pts < 0.0)
		return;
#endif

	// Drop frames if the frame rate of the input stream is higher than the
	// target frame rate (but do not drop the first frame).
	if (decoder->video_frames.count == 0)
		av->video_next_pts = pts;
	else if (pts < av->video_next_pts)
		return;

	//fprintf(stderr, "%d %f %f %f\n", decoder->video_frames.count, pts, av->video_next_pts, frame_time);

	// Insert duplicate frames if the frame rate of the input stream is lower
	// than the target frame rate.
	while ((av->video_next_pts + frame_time) < pts) {
		const void *last_frame = ring_buffer_get_tail(&(decoder->video_frames), 1);
		void *dupe_frame = ring_buffer_get_tail(&(decoder->video_frames), 0);

		assert(last_frame != NULL);
		memcpy(dupe_frame, last_frame, decoder->video_frames.item_size);
		ring_buffer_append(&(decoder->video_frames), 1);
		av->video_next_pts += frame_time;
	}

	void *new_frame = ring_buffer_get_tail(&(decoder->video_frames), 0);
	int plane_size = decoder->video_width * decoder->video_height;
	uint8_t *dst_pointers[2] = {
		(uint8_t *)new_frame, (uint8_t *)new_frame + plane_size
	};
	int dst_strides[2] = {
		decoder->video_width, decoder->video_width
	};

	sws_scale(
		av->scaler,
		(const uint8_t *const *)av->frame->data,
		av->frame->linesize,
		0,
		av->frame->height,
		dst_pointers,
		dst_strides
	);
	ring_buffer_append(&(decoder->video_frames), 1);
	av->video_next_pts += frame_time;
}

bool poll_av_data(decoder_t *decoder) {
	decoder_state_t *av = &(decoder->state);

	if (decoder->end_of_input)
		return false;

	AVPacket packet;

	if (av_read_frame(av->format, &packet) >= 0) {
		if (packet.stream_index == av->audio_stream_index)
			poll_av_packet_audio(decoder, &packet);
		else if (packet.stream_index == av->video_stream_index)
			poll_av_packet_video(decoder, &packet);

		av_packet_unref(&packet);
		return true;
	} else {
#if 0
		// out is always padded out with 4032 "0" samples, this makes calculations elsewhere easier
		for (int i = 4032; i > 0;) {
			void *span = ring_buffer_get_tail(&(decoder->audio_samples), 0);
			int span_count = ring_buffer_get_contiguous_span(&(decoder->audio_samples));

			if (span_count > i)
				span_count = i;

			memset(span, 0, decoder->audio_samples.item_size * span_count);
			ring_buffer_append(&(decoder->audio_samples), span_count);
			i -= span_count;
		}
#endif

		decoder->end_of_input = true;
		return false;
	}
}

bool ensure_av_data(decoder_t *decoder, int needed_audio_samples, int needed_video_frames) {
	// HACK: in order to update decoder->end_of_input as soon as all data has
	// been read from the input file, this loop waits for more data than
	// strictly needed.
	while (
#if 0
		decoder->audio_samples.count < needed_audio_samples ||
		decoder->video_frames.count < needed_video_frames
#else
		(needed_audio_samples && decoder->audio_samples.count <= needed_audio_samples) ||
		(needed_video_frames && decoder->video_frames.count <= needed_video_frames)
#endif
	) {
		//fprintf(stderr, "ensure %d -> %d, %d -> %d\n", decoder->audio_samples.count, needed_audio_samples, decoder->video_frames.count, needed_video_frames);
		if (!poll_av_data(decoder)) {
			// Keep returning true even if the end of the input file has been
			// reached, if the buffer is not yet completely empty.
			return
				(decoder->audio_samples.count > 0 || !needed_audio_samples) &&
				(decoder->video_frames.count > 0 || !needed_video_frames);
		}
	}
	//fprintf(stderr, "ensure %d -> %d, %d -> %d\n", decoder->audio_samples.count, needed_audio_samples, decoder->video_frames.count, needed_video_frames);

	return true;
}

void close_av_data(decoder_t *decoder) {
	decoder_state_t *av = &(decoder->state);

	av_frame_free(&(av->frame));
	swr_free(&(av->resampler));
	// Deprecated, kept for compatibility with older FFmpeg versions.
	avcodec_close(av->audio_codec_context);
	avcodec_free_context(&(av->audio_codec_context));
	avformat_free_context(av->format);
	free(av->resample_buffer);

	destroy_ring_buffer(&(decoder->audio_samples));
	destroy_ring_buffer(&(decoder->video_frames));
}
