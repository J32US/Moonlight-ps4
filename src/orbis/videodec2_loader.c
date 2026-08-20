// Dynamic load of libSceVideodec2 (LoadStartModule + Dlsym).
#include "videodec2.h"
#include "video_out_c.h"
#include "../log.h"

#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

#define DMEM_ALIGN ML_DMEM_ALIGN

static OrbisVideodec2ComputeQueue s_queue;
static void *s_compute_mem;
static off_t s_compute_off;
static size_t s_compute_size;
static int s_mod = -1;
static videodec2_api_t s_api;

static int dlsym1(int mod, const char *name, void **out) {
    int rc = sceKernelDlsym(mod, name, out);
    if (rc != 0 || !*out) {
        LOGE("videodec2: Dlsym %s => 0x%08x", name, (unsigned)rc);
        return -1;
    }
    return 0;
}

static int alloc_dmem(size_t need, int32_t mem_type, void **ptr, off_t *off, size_t *sz_out) {
    need = (need + DMEM_ALIGN - 1) & ~(size_t)(DMEM_ALIGN - 1);
    if (need < DMEM_ALIGN)
        need = DMEM_ALIGN;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                           need, DMEM_ALIGN, mem_type, off);
    if (rc < 0) {
        LOGE("videodec2: AllocateDirectMemory(%zu type=%d) => 0x%08x",
             need, mem_type, (unsigned)rc);
        return -1;
    }
    rc = sceKernelMapDirectMemory(ptr, need, ML_DMEM_PROT_RW, 0, *off, DMEM_ALIGN);
    if (rc < 0) {
        LOGE("videodec2: MapDirectMemory => 0x%08x", (unsigned)rc);
        sceKernelReleaseDirectMemory(*off, need);
        return -1;
    }
    *sz_out = need;
    return 0;
}

static void free_compute_mem(void) {
    if (s_compute_mem) {
        sceKernelReleaseDirectMemory(s_compute_off, s_compute_size);
        s_compute_mem = NULL;
        s_compute_off = 0;
        s_compute_size = 0;
    }
}

static int load_module(const char *path) {
    int rc = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, NULL);
    LOGI("videodec2: LoadStartModule(%s) => 0x%08x", path, (unsigned)rc);
    return rc;
}

static int load_vdec_deps(void) {
    static const char *mods[] = {
        "/system/common/lib/libSceVdecCore.sprx",
        "/system/common/lib/libSceVdecSavc.sprx",
        "/system/common/lib/libSceVdecSavc2.sprx",
        "/system/common/lib/libSceVdecwrap.sprx",
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        int rc = load_module(mods[i]);
        if (rc < 0)
            LOGW("videodec2: opcional %s => 0x%08x", mods[i], (unsigned)rc);
    }
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);
    return 0;
}

static int resolve_api(videodec2_api_t *api, int mod) {
    api->module = mod;
    if (dlsym1(mod, "sceVideodec2QueryComputeMemoryInfo", (void **)&api->QueryComputeMemoryInfo) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2AllocateComputeQueue", (void **)&api->AllocateComputeQueue) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2ReleaseComputeQueue", (void **)&api->ReleaseComputeQueue) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2QueryDecoderMemoryInfo", (void **)&api->QueryDecoderMemoryInfo) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2CreateDecoder", (void **)&api->CreateDecoder) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2DeleteDecoder", (void **)&api->DeleteDecoder) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2Decode", (void **)&api->Decode) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2Flush", (void **)&api->Flush) < 0)
        return -1;
    if (dlsym1(mod, "sceVideodec2Reset", (void **)&api->Reset) < 0)
        return -1;
    LOGI("videodec2: simbolos OK");
    return 0;
}

int videodec2_load(videodec2_api_t *api) {
    if (!api)
        return -1;
    if (s_mod >= 0) {
        *api = s_api;
        api->queue = s_queue;
        return 0;
    }

    memset(api, 0, sizeof(*api));
    api->module = -1;

    load_vdec_deps();

    int mod = load_module("/system/common/lib/libSceVideodec2.sprx");
    if (mod < 0) {
        mod = load_module("libSceVideodec2.sprx");
        if (mod < 0)
            return -1;
    }

    if (resolve_api(api, mod) != 0)
        return -1;

    s_mod = mod;
    s_api = *api;
    return 0;
}

void videodec2_unload(videodec2_api_t *api) {
    (void)api;
    // Free queue BEFORE Onion memory (queue lives inside cpuGpuMemory).
    if (s_api.ReleaseComputeQueue && s_queue) {
        int rc = s_api.ReleaseComputeQueue(s_queue);
        LOGI("videodec2: ReleaseComputeQueue => 0x%08x", (unsigned)rc);
    }
    s_queue = NULL;
    s_api.queue = NULL;
    free_compute_mem();
}

static int alloc_compute_queue(videodec2_api_t *api) {
    if (s_queue) {
        api->queue = s_queue;
        s_api.queue = s_queue;
        LOGI("videodec2: reusando compute queue %p", s_queue);
        return 0;
    }

    OrbisVideodec2ComputeMemoryInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.thisSize = sizeof(mi);

    int rc = api->QueryComputeMemoryInfo(&mi);
    LOGI("videodec2: QueryComputeMemoryInfo => 0x%08x size=%llu",
         (unsigned)rc, (unsigned long long)mi.cpuGpuMemorySize);
    if (rc < 0)
        return rc;

    if (alloc_dmem((size_t)mi.cpuGpuMemorySize, ML_DMEM_TYPE_ONION,
                   &s_compute_mem, &s_compute_off, &s_compute_size) < 0)
        return -1;
    mi.cpuGpuMemory = s_compute_mem;

    // pipe 0..4, queue 0..7 (limites de shadPS4 / docs).
    static const struct { uint16_t pipe; uint16_t queue; } k_try[] = {
        {0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {3, 0}, {4, 0},
    };

    rc = -1;
    for (size_t i = 0; i < sizeof(k_try) / sizeof(k_try[0]); i++) {
        OrbisVideodec2ComputeConfigInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.thisSize = sizeof(ci);
        ci.computePipeId = k_try[i].pipe;
        ci.computeQueueId = k_try[i].queue;
        ci.checkMemoryType = true;

        OrbisVideodec2ComputeQueue q = NULL;
        rc = api->AllocateComputeQueue(&ci, &mi, &q);
        LOGI("videodec2: AllocateComputeQueue pipe=%u queue=%u => 0x%08x q=%p",
             k_try[i].pipe, k_try[i].queue, (unsigned)rc, q);
        if (rc == 0 && q) {
            s_queue = q;
            api->queue = s_queue;
            s_api.queue = s_queue;
            return 0;
        }
        if ((unsigned)rc == 0x811d0109u)
            LOGW("videodec2: NOT_ONION_MEMORY (cpuGpu must be Onion type=0)");
        else if ((unsigned)rc == 0x811d010au)
            LOGW("videodec2: NOT_GARLIC_MEMORY");
        else if ((unsigned)rc == 0x811d0200u)
            LOGW("videodec2: CONFIG_INFO (pipe/queue ocupado o config invalida)");
    }

    free_compute_mem();
    api->queue = NULL;
    s_api.queue = NULL;
    return rc < 0 ? rc : -1;
}

/* One config probe: QueryDecoderMemoryInfo + CreateDecoder, logged with every
 * input so the console error catalog (Task 1.3) can be filled from one run. */
static void spike_probe(const videodec2_api_t *api, const char *label,
                        uint32_t resType, uint32_t codecType, uint32_t profile,
                        uint32_t maxLevel, int dpb, int w, int h) {
    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config(&dc, api->queue, w, h);
    dc.resourceType = resType;
    dc.codecType = codecType;
    dc.profile = profile;
    dc.maxLevel = maxLevel;
    dc.maxDpbFrameCount = dpb;

    OrbisVideodec2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);
    int rc = api->QueryDecoderMemoryInfo(&dc, &dm);
    LOGI("spike[%s]: resType=%u codec=%u prof=%u lvl=%u dpb=%d %dx%d => 0x%08x "
         "cpu=%llu gpu=%llu cpuGpu=%llu fb=%llu",
         label, (unsigned)resType, (unsigned)codecType, (unsigned)profile,
         (unsigned)maxLevel, dpb, w, h, (unsigned)rc,
         (unsigned long long)dm.cpuMemorySize,
         (unsigned long long)dm.gpuMemorySize,
         (unsigned long long)dm.cpuGpuMemorySize,
         (unsigned long long)dm.maxFrameBufferSize);

    if (rc == 0) {
        OrbisVideodec2Decoder dec = NULL;
        rc = api->CreateDecoder(&dc, &dm, &dec);
        LOGI("spike[%s]: CreateDecoder => 0x%08x dec=%p", label, (unsigned)rc, dec);
        if (dec && api->DeleteDecoder)
            api->DeleteDecoder(dec);
    }
}

int videodec2_spike_run(void) {
    LOGI("=== videodec2 spike begin ===");
    videodec2_api_t api;
    if (videodec2_load(&api) != 0) {
        LOGE("VD2 FAIL load");
        return -1;
    }

    int rc = alloc_compute_queue(&api);
    if (rc < 0) {
        LOGE("VD2 FAIL alloc 0x%08x", (unsigned)rc);
        videodec2_unload(&api);
        return -1;
    }

    /* Baseline: stock AVC 1080p — must keep working. */
    spike_probe(&api, "AVC-base", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                ORBIS_VIDEODEC2_CODEC_AVC, ORBIS_VIDEODEC2_PROFILE_HIGH,
                ORBIS_VIDEODEC2_LEVEL_51, ORBIS_VIDEODEC2_DPB_DEFAULT,
                1920, 1080);

    /* Phase 1 HEVC matrix (Task 1.3 unknowns #1-#6). resourceType binary
     * search: 1 = EMBEDDED (validated for AVC), then 2, 3 if it errors. */
    spike_probe(&api, "HEVC-Main-res1", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                ORBIS_VIDEODEC2_CODEC_HEVC, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51, ORBIS_VIDEODEC2_DPB_DEFAULT,
                1920, 1080);
    spike_probe(&api, "HEVC-Main-res2", 2u,
                ORBIS_VIDEODEC2_CODEC_HEVC, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51, ORBIS_VIDEODEC2_DPB_DEFAULT,
                1920, 1080);
    spike_probe(&api, "HEVC-Main-res3", 3u,
                ORBIS_VIDEODEC2_CODEC_HEVC, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51, ORBIS_VIDEODEC2_DPB_DEFAULT,
                1920, 1080);
    /* Main10 (unknown #4): 10-bit profile, resType as for Main. */
    spike_probe(&api, "HEVC-Main10", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                ORBIS_VIDEODEC2_CODEC_HEVC, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN10,
                ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51, ORBIS_VIDEODEC2_DPB_DEFAULT,
                1920, 1080);
    /* 4K (unknowns #5-#6): L5.1 + DPB 6, resType 1 then 2. */
    spike_probe(&api, "HEVC-4K-res1", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                ORBIS_VIDEODEC2_CODEC_HEVC, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51, ORBIS_VIDEODEC2_DPB_HEVC_4K,
                3840, 2160);
    spike_probe(&api, "HEVC-4K-res2", 2u,
                ORBIS_VIDEODEC2_CODEC_HEVC, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51, ORBIS_VIDEODEC2_DPB_HEVC_4K,
                3840, 2160);

    videodec2_unload(&api);
    LOGI("=== videodec2 spike DONE (see matrix above) ===");
    return 0;
}

int videodec2_probe_decoder(int width, int height) {
    videodec2_api_t api;
    if (videodec2_load(&api) != 0)
        return -1;
    if (alloc_compute_queue(&api) < 0)
        return -1;

    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config(&dc, api.queue, width, height);
    OrbisVideodec2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);
    int rc = api.QueryDecoderMemoryInfo(&dc, &dm);
    LOGI("videodec2: probe QueryDecoderMemoryInfo %dx%d => 0x%08x fb=%llu",
         width, height, (unsigned)rc, (unsigned long long)dm.maxFrameBufferSize);
    if (rc < 0)
        return -1;
    return 0;
}

int videodec2_ensure_compute_queue(videodec2_api_t *api) {
    if (!api || !api->AllocateComputeQueue)
        return -1;
    if (s_queue) {
        api->queue = s_queue;
        s_api.queue = s_queue;
        return 0;
    }
    return alloc_compute_queue(api);
}

int video_orbis_probe(int width, int height) {
    return videodec2_probe_decoder(width, height);
}
