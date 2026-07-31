// Video interface: decoders (FFmpeg / Videodec2) + YCbCr/NV12 presentation.
#pragma once

#include <Limelight.h>
#include <stdint.h>
#include <stddef.h>

#ifndef ML_ENABLE_VIDEODEC2
#define ML_ENABLE_VIDEODEC2 0
#endif

extern DECODER_RENDERER_CALLBACKS video_callbacks_ffmpeg;

#ifdef __ORBIS__
extern DECODER_RENDERER_CALLBACKS video_callbacks_orbis;
int video_orbis_probe(int width, int height);
int videodec2_spike_run(void);
#endif

typedef enum {
    VIDEO_FRAME_NV12,
    VIDEO_FRAME_YUV420P,
} video_frame_format_t;

typedef struct {
    unsigned long long decode_us_total;
    unsigned long long convert_us_total; /* bounce + blit/bgra */
    unsigned long long bounce_us_total;  /* memcpy WC→cacheable (decoder_orbis) */
    unsigned long long bgra_us_total;    /* kernel convert/blit only in present */
    unsigned long long present_us_total;
    unsigned frames;   /* frames presented (flip OK) */
    unsigned decodes;  /* Decode calls counted (may be > frames) */
    unsigned dropped;
} video_stats_t;

void video_get_stats(video_stats_t *out);
void video_reset_stats(void);
void video_stats_add(unsigned long long decode_us, unsigned long long convert_us,
                     unsigned long long present_us, unsigned dropped);
/* Accumulate Decode time and increment decodes (not frames). */
void video_stats_add_decode(unsigned long long decode_us);
/* Accumulate WC→bounce memcpy in convert/bounce (without incrementing frames). */
void video_stats_add_bounce(unsigned long long bounce_us);

#ifdef __ORBIS__
int video_present_init(int w, int h, int prefer_ycbcr);
void video_present_shutdown(void);
int video_present_should_drop(void);
int video_present_plugin_loaded(void);
int video_present_is_bgra(void);
/* BGRA pipeline: kick convert async; finish = join + flip. Decode always separate. */
int video_present_bgra_pipe_kick(const uint8_t *y, const uint8_t *uv,
                                 int pitch_y, int pitch_uv, int w, int h);
int video_present_bgra_pipe_finish(void);
int video_present_frame(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        int pitch_y, int pitch_uv, int w, int h,
                        video_frame_format_t fmt);
int video_present_frame_nv12_copy(const void *src, size_t src_size,
                                  int pitch_y, int pitch_uv, int w, int h);

/* Menu UI: reuses BGRA present (CPU draw + flip). Exclusive with
 * the stream: call video_ui_end() before LiStartConnection. */
int video_ui_begin(int w, int h);
int video_ui_get_fb(uint8_t **bgra, int *pitch_bytes, int *idx);
int video_ui_flip(int idx);
/* Present a full BGRA frame from staging (avoids drawing on-screen). */
int video_ui_present(const uint8_t *src, int src_pitch);
void video_ui_end(void);
#endif
