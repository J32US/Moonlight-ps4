// Reversed types from libSceVideodec2 (shadPS4). Size validated by the library.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define ORBIS_VIDEODEC2_CODEC_AVC 1
#define ORBIS_VIDEODEC2_CODEC_HEVC 2 /* UNVERIFIED on console — Task 1.3 spike */

// Validated on console: resourceType=0 → 0x811D0203; without profile/level → 0x811D0205;
// cpuThreadPriority=16 → 0x811D0208.
#define ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED 1u
#define ORBIS_VIDEODEC2_PROFILE_HIGH           100u
#define ORBIS_VIDEODEC2_LEVEL_51               51u
#define ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN      200u /* UNVERIFIED — spike */
#define ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN10    202u /* UNVERIFIED — spike */
#define ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51      153u /* HEVC L5.1 = 4K60 */
#define ORBIS_VIDEODEC2_DPB_DEFAULT            4    /* AVC + 1080p HEVC */
#define ORBIS_VIDEODEC2_DPB_HEVC_4K            6    /* HEVC L5.1 4K min DPB */
#define ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT    700
#define ORBIS_VIDEODEC2_AFFINITY_ALL           0x3Full

// Structs BEFORE typedef void* OrbisVideodec2Decoder (C lexer prefix).
typedef struct {
    uint64_t thisSize;
    uint16_t computePipeId;
    uint16_t computeQueueId;
    bool checkMemoryType;
    uint8_t reserved0;
    uint16_t reserved1;
} OrbisVideodec2ComputeConfigInfo;

typedef struct {
    uint64_t thisSize;
    uint64_t cpuGpuMemorySize;
    void *cpuGpuMemory;
} OrbisVideodec2ComputeMemoryInfo;

typedef struct {
    uint64_t thisSize;
    uint32_t resourceType;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    int32_t maxFrameWidth;
    int32_t maxFrameHeight;
    int32_t maxDpbFrameCount;
    uint32_t decodePipelineDepth;
    void *computeQueue;
    uint64_t cpuAffinityMask;
    int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    bool checkMemoryType;
    uint8_t reserved0;
    uint8_t reserved1;
    void *extraConfigInfo;
} OrbisVideodec2DecoderConfigInfo;

typedef struct {
    uint64_t thisSize;
    uint64_t cpuMemorySize;
    void *cpuMemory;
    uint64_t gpuMemorySize;
    void *gpuMemory;
    uint64_t cpuGpuMemorySize;
    void *cpuGpuMemory;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment;
    uint32_t reserved0;
} OrbisVideodec2DecoderMemoryInfo;

typedef struct {
    uint64_t thisSize;
    void *auData;
    uint64_t auSize;
    uint64_t ptsData;
    uint64_t dtsData;
    uint64_t attachedData;
} OrbisVideodec2InputData;

typedef struct {
    uint64_t thisSize;
    bool isValid;
    bool isErrorFrame;
    uint8_t pictureCount;
    uint32_t codecType;
    uint32_t frameWidth;
    uint32_t framePitch;
    uint32_t frameHeight;
    void *frameBuffer;
    uint64_t frameBufferSize;
    uint32_t frameFormat;
    uint32_t framePitchInBytes;
} OrbisVideodec2OutputInfo;

typedef struct {
    uint64_t thisSize;
    void *frameBuffer;
    uint64_t frameBufferSize;
    bool isAccepted;
} OrbisVideodec2FrameBuffer;

typedef void *OrbisVideodec2Decoder;
typedef void *OrbisVideodec2ComputeQueue;

/* Frames Videodec2 may have in flight. Each one needs its own framebuffer, so
 * this also bounds how many the caller has to allocate and rotate. */
#define ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH 4

static inline void videodec2_fill_decoder_config_ex(OrbisVideodec2DecoderConfigInfo *dc,
                                                    OrbisVideodec2ComputeQueue q,
                                                    int w, int h,
                                                    int pipelineDepth, int cpuPriority,
                                                    uint32_t codecType, uint32_t profile) {
    if (pipelineDepth < 1)
        pipelineDepth = 1;
    if (pipelineDepth > ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH)
        pipelineDepth = ORBIS_VIDEODEC2_MAX_PIPELINE_DEPTH;
    if (cpuPriority <= 0)
        cpuPriority = ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT;

    memset(dc, 0, sizeof(*dc));
    dc->thisSize = sizeof(*dc);
    dc->resourceType = ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED;
    dc->codecType = codecType;
    dc->profile = profile;
    /* HEVC level_idc is 3-digit (L5.1 = 153); AVC is 2-digit (5.1 = 51). */
    dc->maxLevel = (codecType == ORBIS_VIDEODEC2_CODEC_HEVC)
        ? ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51 : ORBIS_VIDEODEC2_LEVEL_51;
    dc->maxFrameWidth = w;
    /* 1088: macroblock-align; 1080 in config → slow Decode / rare paths. */
    dc->maxFrameHeight = (h + 15) & ~15;
    /* dpb=1 + stream with refs → Decode ~150ms and IDR storm. 4 = typical NVENC. */
    dc->maxDpbFrameCount = (codecType == ORBIS_VIDEODEC2_CODEC_HEVC && w >= 3840)
        ? ORBIS_VIDEODEC2_DPB_HEVC_4K : ORBIS_VIDEODEC2_DPB_DEFAULT;
    /* depth=1 serialises submit→wait against the compute queue every frame. */
    dc->decodePipelineDepth = (uint32_t)pipelineDepth;
    dc->computeQueue = q;
    dc->cpuAffinityMask = ORBIS_VIDEODEC2_AFFINITY_ALL;
    dc->cpuThreadPriority = cpuPriority;
    dc->optimizeProgressiveVideo = true;
    // checkMemoryType=0: validated in 0.4.x; with 1 + AU Onion the Vdec worker SIGSEGV.
    dc->checkMemoryType = false;
}

static inline void videodec2_fill_decoder_config(OrbisVideodec2DecoderConfigInfo *dc,
                                                   OrbisVideodec2ComputeQueue q,
                                                   int w, int h) {
    videodec2_fill_decoder_config_ex(dc, q, w, h, 1,
                                     ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT,
                                     ORBIS_VIDEODEC2_CODEC_AVC,
                                     ORBIS_VIDEODEC2_PROFILE_HIGH);
}

typedef int32_t (*sceVideodec2QueryComputeMemoryInfo_t)(OrbisVideodec2ComputeMemoryInfo *);
typedef int32_t (*sceVideodec2AllocateComputeQueue_t)(const OrbisVideodec2ComputeConfigInfo *,
                                                      const OrbisVideodec2ComputeMemoryInfo *,
                                                      OrbisVideodec2ComputeQueue *);
typedef int32_t (*sceVideodec2ReleaseComputeQueue_t)(OrbisVideodec2ComputeQueue);
typedef int32_t (*sceVideodec2QueryDecoderMemoryInfo_t)(const OrbisVideodec2DecoderConfigInfo *,
                                                        OrbisVideodec2DecoderMemoryInfo *);
typedef int32_t (*sceVideodec2CreateDecoder_t)(const OrbisVideodec2DecoderConfigInfo *,
                                               const OrbisVideodec2DecoderMemoryInfo *,
                                               OrbisVideodec2Decoder *);
/* HEVC is a separate entry point (OpenOrbis SDK header): the generic
 * CreateDecoder rejects codecType=2 with 0x811d0204 (CODEC_TYPE). */
typedef int32_t (*sceVideodec2CreateHevcDecoder_t)(const OrbisVideodec2DecoderConfigInfo *,
                                                   const OrbisVideodec2DecoderMemoryInfo *,
                                                   OrbisVideodec2Decoder *);
typedef int32_t (*sceVideodec2DeleteDecoder_t)(OrbisVideodec2Decoder);
typedef int32_t (*sceVideodec2Decode_t)(OrbisVideodec2Decoder,
                                        const OrbisVideodec2InputData *,
                                        OrbisVideodec2FrameBuffer *,
                                        OrbisVideodec2OutputInfo *);
typedef int32_t (*sceVideodec2Flush_t)(OrbisVideodec2Decoder,
                                       OrbisVideodec2FrameBuffer *,
                                       OrbisVideodec2OutputInfo *);
typedef int32_t (*sceVideodec2Reset_t)(OrbisVideodec2Decoder);

typedef struct {
    int module;
    OrbisVideodec2ComputeQueue queue;
    sceVideodec2QueryComputeMemoryInfo_t QueryComputeMemoryInfo;
    sceVideodec2AllocateComputeQueue_t AllocateComputeQueue;
    sceVideodec2ReleaseComputeQueue_t ReleaseComputeQueue;
    sceVideodec2QueryDecoderMemoryInfo_t QueryDecoderMemoryInfo;
    sceVideodec2CreateDecoder_t CreateDecoder;
    sceVideodec2CreateHevcDecoder_t CreateHevcDecoder; /* nullable: FW w/o HEVC */
    sceVideodec2DeleteDecoder_t DeleteDecoder;
    sceVideodec2Decode_t Decode;
    sceVideodec2Flush_t Flush;
    sceVideodec2Reset_t Reset;
} videodec2_api_t;

int videodec2_load(videodec2_api_t *api);
void videodec2_unload(videodec2_api_t *api);
int videodec2_spike_run(void);
int videodec2_probe_decoder(int width, int height);
int videodec2_ensure_compute_queue(videodec2_api_t *api);
