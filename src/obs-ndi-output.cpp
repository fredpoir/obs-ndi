/*
obs-ndi (NDI I/O in OBS Studio)
Copyright (C) 2016-2017 Stéphane Lepin <stephane.lepin@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/profiler.h>
#include <util/circlebuf.h>

#include "obs-ndi.h"

#define MAX_BUFFERING_FRAMES 60

static FORCE_INLINE uint32_t min_uint32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static FORCE_INLINE int min_int(int a, int b) {
    return a < b ? a : b;
}

void convert_nv12_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
    uint32_t start_y, uint32_t end_y,
    uint8_t* output, uint32_t out_linesize)
{
    uint8_t* _Y;
    uint8_t* _U;
    uint8_t* _V;
    uint8_t* _out;
    uint32_t width = min_uint32(in_linesize[0], out_linesize);
    for (uint32_t y = start_y; y < end_y; y++) {
        _Y = input[0] + (y * in_linesize[0]);
        _U = input[1] + ((y / 2) * in_linesize[1]);
        _V = _U + 1;

        _out = output + (y * out_linesize);

        for (uint32_t x = 0; x < width; x += 2) {
            *(_out++) = *(_U++); _U++;
            *(_out++) = *(_Y++);
            *(_out++) = *(_V++); _V++;
            *(_out++) = *(_Y++);
        }
    }
}

void convert_i420_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
    uint32_t start_y, uint32_t end_y,
    uint8_t* output, uint32_t out_linesize)
{
    uint8_t* _Y;
    uint8_t* _U;
    uint8_t* _V;
    uint8_t* _out;
    uint32_t width = min_uint32(in_linesize[0], out_linesize);
    for (uint32_t y = start_y; y < end_y; y++) {
        _Y = input[0] + (y * in_linesize[0]);
        _U = input[1] + ((y / 2) * in_linesize[1]);
        _V = input[2] + ((y / 2) * in_linesize[2]);

        _out = output + (y * out_linesize);

        for (uint32_t x = 0; x < width; x += 2) {
            *(_out++) = *(_U++);
            *(_out++) = *(_Y++);
            *(_out++) = *(_V++);
            *(_out++) = *(_Y++);
        }
    }
}

void convert_i444_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
    uint32_t start_y, uint32_t end_y,
    uint8_t* output, uint32_t out_linesize)
{
    uint8_t* _Y;
    uint8_t* _U;
    uint8_t* _V;
    uint8_t* _out;
    uint32_t width = min_uint32(in_linesize[0], out_linesize);
    for (uint32_t y = start_y; y < end_y; y++) {
        _Y = input[0] + (y * in_linesize[0]);
        _U = input[1] + (y * in_linesize[1]);
        _V = input[2] + (y * in_linesize[2]);

        _out = output + (y * out_linesize);

        for (uint32_t x = 0; x < width; x += 2) {
            // Quality loss here. Some chroma samples are ignored.
            *(_out++) = *(_U++); _U++;
            *(_out++) = *(_Y++);
            *(_out++) = *(_V++); _V++;
            *(_out++) = *(_Y++);
        }
    }
}

struct ndi_output {
    obs_output_t *output;
    const char* ndi_name;
    bool async_sending;
    obs_video_info video_info;
    obs_audio_info audio_info;

    bool started;
    NDIlib_FourCC_type_e frame_format;
    NDIlib_send_instance_t ndi_sender;

    uint8_t* conv_buffer;
    uint32_t conv_linesize;

    struct circlebuf video_frames;
    pthread_t video_send_thread;
    os_sem_t* video_send_sem;
    os_event_t* video_send_stop_event;

    uint64_t last_audio_timestamp;
};

const char* ndi_output_getname(void* data) {
    UNUSED_PARAMETER(data);
    return obs_module_text("NDIPlugin.OutputName");
}

obs_properties_t* ndi_output_getproperties(void* data) {
    UNUSED_PARAMETER(data);

    obs_properties_t* props = obs_properties_create();
    obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

    obs_properties_add_text(props, "ndi_name",
        obs_module_text("NDIPlugin.OutputProps.NDIName"), OBS_TEXT_DEFAULT);

    return props;
}

void* ndi_videosend_thread(void* data) {
    struct ndi_output* o = static_cast<ndi_output*>(data);

    uint64_t frame_duration = 0;

    while (os_sem_wait(o->video_send_sem) == 0) {
        if (os_event_try(o->video_send_stop_event) == 0) {
            break;
        }

        NDIlib_video_frame_v2_t video_frame = { 0 };
        circlebuf_pop_front(&o->video_frames,
            &video_frame, sizeof(NDIlib_video_frame_v2_t));

        frame_duration = 1000000000ULL /
            (video_frame.frame_rate_N / video_frame.frame_rate_D);

        uint64_t next_frame = os_gettime_ns() + frame_duration;

        if (video_frame.p_data != nullptr) {
            ndiLib->NDIlib_send_send_video_v2(o->ndi_sender, &video_frame);
            bfree(video_frame.p_data);
        }

        os_sleepto_ns(next_frame);
    }

    // Flush frames in circlebuf before exiting
    while (o->video_frames.size > 0) {
        NDIlib_video_frame_v2_t flushed_frame = { 0 };
        circlebuf_pop_front(&o->video_frames,
            &flushed_frame, sizeof(NDIlib_video_frame_v2_t));
        bfree(flushed_frame.p_data);
    }

    return NULL;
}

bool ndi_output_start(void* data) {
    struct ndi_output* o = (struct ndi_output*)data;

    ndiLib->NDIlib_send_destroy(o->ndi_sender);
    delete o->conv_buffer;

    obs_get_video_info(&o->video_info);
    obs_get_audio_info(&o->audio_info);

    switch (o->video_info.output_format) {
        case VIDEO_FORMAT_NV12:
        case VIDEO_FORMAT_I420:
        case VIDEO_FORMAT_I444:
            o->frame_format = NDIlib_FourCC_type_UYVY;
            o->conv_linesize = o->video_info.output_width * 2;
            o->conv_buffer =
                new uint8_t[o->video_info.output_height * o->conv_linesize * 2]();
            break;

        case VIDEO_FORMAT_RGBA:
            o->frame_format = NDIlib_FourCC_type_RGBA;
            break;

        case VIDEO_FORMAT_BGRA:
            o->frame_format = NDIlib_FourCC_type_BGRA;
            break;

        case VIDEO_FORMAT_BGRX:
            o->frame_format = NDIlib_FourCC_type_BGRX;
            break;
    }


    NDIlib_send_create_t send_desc;
    send_desc.p_ndi_name = o->ndi_name;
    send_desc.p_groups = NULL;
    send_desc.clock_video = false;
    send_desc.clock_audio = false;

    o->ndi_sender = ndiLib->NDIlib_send_create(&send_desc);
    if (o->ndi_sender) {
        o->started = obs_output_begin_data_capture(o->output, 0);
        if (o->started) {
            pthread_create(&o->video_send_thread,
                NULL, ndi_videosend_thread, o);

            if (o->async_sending) {
                blog(LOG_INFO, "asynchronous video sending enabled");
            }
            else {
                blog(LOG_INFO, "asynchronous video sending disabled");
            }
        }
    }

    return o->started;
}

void ndi_output_stop(void* data, uint64_t ts) {
    struct ndi_output* o = (struct ndi_output*)data;

    o->started = false;
    obs_output_end_data_capture(o->output);

    os_event_signal(o->video_send_stop_event);
    os_sem_post(o->video_send_sem);
    pthread_join(o->video_send_thread, NULL);

    ndiLib->NDIlib_send_destroy(o->ndi_sender);
    delete o->conv_buffer;
    o->conv_buffer = nullptr;
}

void ndi_output_update(void* data, obs_data_t* settings) {
    struct ndi_output* o = (struct ndi_output*)data;
    o->ndi_name = obs_data_get_string(settings, "ndi_name");
    o->async_sending = obs_data_get_bool(settings, "ndi_async_sending");
}

void* ndi_output_create(obs_data_t* settings, obs_output_t* output) {
    struct ndi_output* o =
            (struct ndi_output*)bzalloc(sizeof(struct ndi_output));
    o->output = output;
    o->started = false;
    o->last_audio_timestamp = 0;

    circlebuf_init(&o->video_frames);
    os_sem_init(&o->video_send_sem, 0);
    os_event_init(&o->video_send_stop_event, OS_EVENT_TYPE_AUTO);

    ndi_output_update(o, settings);
    return o;
}

void ndi_output_destroy(void* data) {
    struct ndi_output* o = (struct ndi_output*)data;
    bfree(o);
}

void ndi_output_rawvideo(void* data, struct video_data* frame) {
    struct ndi_output* o = (struct ndi_output*)data;
    if (!o->started)
        return;

    size_t frames_waiting =
        o->video_frames.size / sizeof(NDIlib_video_frame_v2_t);
    if (frames_waiting >= MAX_BUFFERING_FRAMES) {
        return;
    }

    uint32_t width = o->video_info.output_width;
    uint32_t height = o->video_info.output_height;

    NDIlib_video_frame_v2_t video_frame = {0};
    video_frame.xres = width;
    video_frame.yres = height;
    video_frame.frame_rate_N = o->video_info.fps_num;
    video_frame.frame_rate_D = o->video_info.fps_den;
    video_frame.picture_aspect_ratio = (float)width / (float)height;
    video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
    video_frame.timecode = (int64_t)(frame->timestamp / 100.0);

    video_frame.FourCC = o->frame_format;
    if (video_frame.FourCC == NDIlib_FourCC_type_UYVY) {
        video_format source_f = o->video_info.output_format;

        if (source_f == VIDEO_FORMAT_NV12) {
            convert_nv12_to_uyvy(frame->data, frame->linesize,
                0, height,
                o->conv_buffer, o->conv_linesize);
        }
        else if (source_f == VIDEO_FORMAT_I420) {
            convert_i420_to_uyvy(frame->data, frame->linesize,
                0, height,
                o->conv_buffer, o->conv_linesize);
        }
        else if (source_f == VIDEO_FORMAT_I444) {
            convert_i444_to_uyvy(frame->data, frame->linesize,
                0, height,
                o->conv_buffer, o->conv_linesize);
        }

        video_frame.p_data = o->conv_buffer;
        video_frame.line_stride_in_bytes = o->conv_linesize;
    }
    else {
        video_frame.p_data = frame->data[0];
        video_frame.line_stride_in_bytes = frame->linesize[0];
    }

    // Keep a copy of video data for the send thread
    size_t video_bytes = video_frame.line_stride_in_bytes * video_frame.yres;
    uint8_t* video_data = (uint8_t*)bmalloc(video_bytes);
    memcpy(video_data, video_frame.p_data, video_bytes);
    video_frame.p_data = video_data;

    uint64_t frame_time = 1000000000ULL /
        (video_frame.frame_rate_N / video_frame.frame_rate_D);
    int64_t audio_buffering =
        (int64_t)frame->timestamp - (int64_t)o->last_audio_timestamp;
    if (audio_buffering < 0) audio_buffering = 0;

    int required_delay_frames = (int)audio_buffering / (int)frame_time;
    size_t current_delay_frames =
        o->video_frames.size / sizeof(NDIlib_video_frame_v2_t);
    int add_delay_frames = required_delay_frames - (int)current_delay_frames;

    if (add_delay_frames < 0) {
        size_t frames_to_pop = min_int(abs(add_delay_frames), (int)current_delay_frames);
        for (size_t i = 0; i < frames_to_pop; i++) {
            NDIlib_video_frame_v2_t popped_frame = { 0 };
            circlebuf_pop_front(&o->video_frames, &popped_frame, sizeof(NDIlib_video_frame_v2_t));
            os_sem_wait(o->video_send_sem);
            bfree(popped_frame.p_data);
        }
    }
    else if (add_delay_frames > 0) {
        for (size_t i = 0; i < add_delay_frames; i++) {
            NDIlib_video_frame_v2_t filler_frame = { 0 };
            memcpy(&filler_frame, &video_frame, sizeof(NDIlib_video_frame_v2_t));
            filler_frame.p_data = nullptr;

            circlebuf_push_back(&o->video_frames, &filler_frame, sizeof(NDIlib_video_frame_v2_t));
            os_sem_post(o->video_send_sem);
        }
    }

    // Push to queue
    circlebuf_push_back(&o->video_frames,
        &video_frame, sizeof(NDIlib_video_frame_v2_t));
    os_sem_post(o->video_send_sem);
}

void ndi_output_rawaudio(void* data, struct audio_data* frame) {
    struct ndi_output* o = (struct ndi_output*)data;
    if (!o->started) return;

    NDIlib_audio_frame_v2_t audio_frame = {0};
    audio_frame.sample_rate = o->audio_info.samples_per_sec;
    audio_frame.no_channels = o->audio_info.speakers;
    audio_frame.no_samples = frame->frames;
    audio_frame.channel_stride_in_bytes = frame->frames * 4;

    size_t data_size =
        audio_frame.no_channels * audio_frame.channel_stride_in_bytes;
    uint8_t* audio_data = (uint8_t*)bmalloc(data_size);

    for (int i = 0; i < audio_frame.no_channels; i++) {
        memcpy(&audio_data[i * audio_frame.channel_stride_in_bytes],
            frame->data[i],
            audio_frame.channel_stride_in_bytes);
    }

    audio_frame.p_data = (float*)audio_data;
    audio_frame.timecode = (int64_t)(frame->timestamp / 100);

    ndiLib->NDIlib_send_send_audio_v2(o->ndi_sender, &audio_frame);
    bfree(audio_data);

    o->last_audio_timestamp = frame->timestamp;
}

struct obs_output_info create_ndi_output_info() {
    struct obs_output_info ndi_output_info = {};
    ndi_output_info.id				= "ndi_output";
    ndi_output_info.flags			= OBS_OUTPUT_AV;
    ndi_output_info.get_name		= ndi_output_getname;
    ndi_output_info.get_properties	= ndi_output_getproperties;
    ndi_output_info.create			= ndi_output_create;
    ndi_output_info.destroy			= ndi_output_destroy;
    ndi_output_info.update			= ndi_output_update;
    ndi_output_info.start			= ndi_output_start;
    ndi_output_info.stop			= ndi_output_stop;
    ndi_output_info.raw_video		= ndi_output_rawvideo;
    ndi_output_info.raw_audio		= ndi_output_rawaudio;

    return ndi_output_info;
}
