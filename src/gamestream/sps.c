/*
 * This file is based on Moonlight Embedded libgamestream/sps.c
 * (GPL-3.0). Adapted for moonlight-ps4.
 */
#include "sps.h"

#include <string.h>
#include <h264_stream.h>

static h264_stream_t *h264_stream;
static int initial_width, initial_height;

void gs_sps_init(int width, int height) {
    if (!h264_stream)
        h264_stream = h264_new();
    initial_width = width;
    initial_height = height;
}

void gs_sps_fix(PLENTRY sps, int flags, uint8_t *out_buf, uint32_t *out_offset) {
    int start_len = sps->data[2] == 0x01 ? 3 : 4;

    read_nal_unit(h264_stream, (uint8_t *)(sps->data + start_len), sps->length - start_len);

    if (initial_width == 1280 && initial_height == 720)
        h264_stream->sps->level_idc = 32;
    else if (initial_width == 1920 && initial_height == 1080)
        h264_stream->sps->level_idc = 42;

    h264_stream->sps->num_ref_frames = 1;

    if (flags & GS_SPS_REMOVE_VST_FIXUP)
        h264_stream->sps->vui.video_signal_type_present_flag = 0;
    if (flags & GS_SPS_REMOVE_CLI_FIXUP)
        h264_stream->sps->vui.chroma_loc_info_present_flag = 0;

    if ((flags & GS_SPS_BITSTREAM_FIXUP) == GS_SPS_BITSTREAM_FIXUP) {
        /* Without VUI, bitstream_restriction is not written. */
        h264_stream->sps->vui_parameters_present_flag = 1;
        if (!h264_stream->sps->vui.bitstream_restriction_flag) {
            h264_stream->sps->vui.bitstream_restriction_flag = 1;
            h264_stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
            h264_stream->sps->vui.max_bits_per_mb_denom = 1;
            h264_stream->sps->vui.log2_max_mv_length_horizontal = 16;
            h264_stream->sps->vui.log2_max_mv_length_vertical = 16;
            h264_stream->sps->vui.num_reorder_frames = 0;
        }
        h264_stream->sps->vui.max_dec_frame_buffering = 1;
        h264_stream->sps->vui.max_bytes_per_pic_denom = 2;
        h264_stream->sps->vui.max_bits_per_mb_denom = 1;
    }

    memcpy(out_buf + *out_offset, sps->data, (size_t)start_len);
    *out_offset += (uint32_t)start_len;

    int nal_len = write_nal_unit(h264_stream, out_buf + *out_offset, 128);
    if (nal_len <= 0) {
        /* Fallback: original SPS without rewrite. */
        memcpy(out_buf + *out_offset, sps->data + start_len,
               (size_t)(sps->length - start_len));
        *out_offset += (uint32_t)(sps->length - start_len);
        return;
    }

    /*
     * h264bitstream rbsp_to_nal() writes from byte 1 and leaves nal_buf[0]=0x00.
     * After the Annex-B start code that yields NAL type 0 and Videodec2 → 0x811d0303.
     */
    uint8_t *nal = out_buf + *out_offset;
    int skip = 0;
    while (skip < nal_len - 1 && nal[skip] == 0x00)
        skip++;
    if (skip > 0) {
        memmove(nal, nal + skip, (size_t)(nal_len - skip));
        nal_len -= skip;
    }
    *out_offset += (uint32_t)nal_len;
}
