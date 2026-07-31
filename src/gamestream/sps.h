/*
 * SPS fixup (from Moonlight Embedded / libgamestream).
 * Forces num_ref_frames=1 and max_dec_frame_buffering=1 for low latency.
 */
#pragma once

#include <Limelight.h>
#include <stdint.h>

#define GS_SPS_BITSTREAM_FIXUP  0x01
#define GS_SPS_REMOVE_VST_FIXUP 0x02
#define GS_SPS_REMOVE_CLI_FIXUP 0x04

void gs_sps_init(int width, int height);
void gs_sps_fix(PLENTRY sps, int flags, uint8_t *out_buf, uint32_t *out_offset);
