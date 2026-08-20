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
        "/system/common/lib/libSceVdecShevc.sprx", /* HEVC codec module (12.00) */
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
    if (dlsym1(mod, "sceVideodec2CreateHevcDecoder", (void **)&api->CreateHevcDecoder) < 0) {
        LOGW("videodec2: CreateHevcDecoder no exportada — FW sin HEVC?");
        api->CreateHevcDecoder = NULL;
    }
    if (dlsym1(mod, "sceVideodec2QueryHevcDecoderMemoryInfo",
               (void **)&api->QueryHevcDecoderMemoryInfo) < 0) {
        LOGW("videodec2: QueryHevcDecoderMemoryInfo no exportada");
        api->QueryHevcDecoderMemoryInfo = NULL;
    }
    if (dlsym1(mod, "sceVideodec2GetHevcPictureInfo",
               (void **)&api->GetHevcPictureInfo) < 0) {
        LOGW("videodec2: GetHevcPictureInfo no exportada");
        api->GetHevcPictureInfo = NULL;
    }
    if (api->CreateHevcDecoder && api->QueryHevcDecoderMemoryInfo)
        LOGI("videodec2: HEVC API completa (create/query/picture-info)");
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

/* One config probe: QueryDecoderMemoryInfo + CreateDecoder/CreateHevcDecoder,
 * logged with every input so the console error catalog can be filled from one
 * run. via_hevc=1 routes the create through sceVideodec2CreateHevcDecoder
 * (HEVC is a separate export — generic path rejects codecType=2). The query
 * still runs with queryCodec (1 = AVC) because QueryDecoderMemoryInfo itself
 * validates codecType; sizes come from resolution/DPB. */
static void spike_probe(const videodec2_api_t *api, const char *label, int via_hevc,
                        uint32_t queryCodec,
                        uint32_t resType, uint32_t codecType, uint32_t profile,
                        uint32_t maxLevel, int dpb, int w, int h) {
    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config(&dc, api->queue, w, h);
    dc.resourceType = resType;
    dc.codecType = queryCodec;
    dc.profile = profile;
    dc.maxLevel = maxLevel;
    dc.maxDpbFrameCount = dpb;

    OrbisVideodec2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);
    int rc = api->QueryDecoderMemoryInfo(&dc, &dm);
    LOGI("spike[%s]: resType=%u codec=%u prof=%u lvl=%u dpb=%d %dx%d => 0x%08x "
         "cpu=%llu gpu=%llu cpuGpu=%llu fb=%llu",
         label, (unsigned)resType, (unsigned)queryCodec, (unsigned)profile,
         (unsigned)maxLevel, dpb, w, h, (unsigned)rc,
         (unsigned long long)dm.cpuMemorySize,
         (unsigned long long)dm.gpuMemorySize,
         (unsigned long long)dm.cpuGpuMemorySize,
         (unsigned long long)dm.maxFrameBufferSize);

    if (rc == 0) {
        /* Round 1 forgot the queried memories → MEMORY_POINTER. The real app
         * allocates cpu=ONION, gpu=GARLIC, cpuGpu=ONION before create. */
        void *cpu_m = NULL, *gpu_m = NULL, *cg_m = NULL;
        off_t cpu_o = 0, gpu_o = 0, cg_o = 0;
        size_t cpu_s = 0, gpu_s = 0, cg_s = 0;
        int ok = 1;
        if (dm.cpuMemorySize &&
            alloc_dmem((size_t)dm.cpuMemorySize, ML_DMEM_TYPE_ONION,
                       &cpu_m, &cpu_o, &cpu_s) < 0)
            ok = 0;
        if (ok && dm.gpuMemorySize &&
            alloc_dmem((size_t)dm.gpuMemorySize, ML_DMEM_TYPE_GARLIC,
                       &gpu_m, &gpu_o, &gpu_s) < 0)
            ok = 0;
        if (ok && dm.cpuGpuMemorySize &&
            alloc_dmem((size_t)dm.cpuGpuMemorySize, ML_DMEM_TYPE_ONION,
                       &cg_m, &cg_o, &cg_s) < 0)
            ok = 0;
        if (ok) {
            dm.cpuMemory = cpu_m;
            dm.gpuMemory = gpu_m;
            dm.cpuGpuMemory = cg_m;
            OrbisVideodec2Decoder dec = NULL;
            int rc2;
            const char *fn;
            if (via_hevc && api->CreateHevcDecoder) {
                fn = "CreateHevcDecoder";
                rc2 = api->CreateHevcDecoder(&dc, &dm, &dec);
            } else {
                fn = "CreateDecoder";
                rc2 = api->CreateDecoder(&dc, &dm, &dec);
            }
            LOGI("spike[%s]: %s => 0x%08x dec=%p", label, fn, (unsigned)rc2, dec);
            if (dec && api->DeleteDecoder)
                api->DeleteDecoder(dec);
        } else {
            LOGE("spike[%s]: alloc decoder memories FAILED", label);
        }
        if (cpu_m) sceKernelReleaseDirectMemory(cpu_o, cpu_s);
        if (gpu_m) sceKernelReleaseDirectMemory(gpu_o, gpu_s);
        if (cg_m) sceKernelReleaseDirectMemory(cg_o, cg_s);
    }
}

/* HEVC create probe. Query and create values are INDEPENDENT: the query
 * (QueryHevcDecoderMemoryInfo, or the AVC-gated shared query as fallback)
 * validates with AVC semantics — round 4 showed prof=100 lvl=51 passes
 * while ANY lvl=153 fails. The create (CreateHevcDecoder) validates
 * separately; round 4 showed zeros fail there. This round sweeps the create
 * space (AVC-style 100/51..153 vs raw HEVC profile_idc 1 + levels) against
 * a known-good query. codecType stays 1 (codec=2 → 0x811d0204). */
static void spike_hevc_probe(const videodec2_api_t *api, const char *label,
                             uint32_t resType,
                             uint32_t queryProfile, uint32_t queryLevel, int queryDpb,
                             uint32_t createProfile, uint32_t createLevel,
                             int createDpb, int w, int h) {
    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config(&dc, api->queue, w, h);
    dc.resourceType = resType;
    dc.codecType = ORBIS_VIDEODEC2_CODEC_AVC;
    dc.profile = queryProfile;
    dc.maxLevel = queryLevel;
    dc.maxDpbFrameCount = queryDpb;

    OrbisVideodec2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);
    int rc;
    const char *qfn;
    if (api->QueryHevcDecoderMemoryInfo) {
        qfn = "QueryHevcDecoderMemoryInfo";
        rc = api->QueryHevcDecoderMemoryInfo(&dc, &dm);
    } else {
        qfn = "QueryDecoderMemoryInfo(AVC-valid)";
        rc = api->QueryDecoderMemoryInfo(&dc, &dm);
    }
    LOGI("spike[%s]: %s(resType=%u prof=%u lvl=%u dpb=%d %dx%d) => 0x%08x "
         "cpu=%llu gpu=%llu cpuGpu=%llu fb=%llu",
         label, qfn, (unsigned)resType, (unsigned)dc.profile,
         (unsigned)dc.maxLevel, dc.maxDpbFrameCount, w, h, (unsigned)rc,
         (unsigned long long)dm.cpuMemorySize,
         (unsigned long long)dm.gpuMemorySize,
         (unsigned long long)dm.cpuGpuMemorySize,
         (unsigned long long)dm.maxFrameBufferSize);
    if (rc != 0)
        return;

    void *cpu_m = NULL, *gpu_m = NULL, *cg_m = NULL;
    off_t cpu_o = 0, gpu_o = 0, cg_o = 0;
    size_t cpu_s = 0, gpu_s = 0, cg_s = 0;
    int ok = 1;
    if (dm.cpuMemorySize &&
        alloc_dmem((size_t)dm.cpuMemorySize, ML_DMEM_TYPE_ONION,
                   &cpu_m, &cpu_o, &cpu_s) < 0)
        ok = 0;
    if (ok && dm.gpuMemorySize &&
        alloc_dmem((size_t)dm.gpuMemorySize, ML_DMEM_TYPE_GARLIC,
                   &gpu_m, &gpu_o, &gpu_s) < 0)
        ok = 0;
    if (ok && dm.cpuGpuMemorySize &&
        alloc_dmem((size_t)dm.cpuGpuMemorySize, ML_DMEM_TYPE_ONION,
                   &cg_m, &cg_o, &cg_s) < 0)
        ok = 0;
    if (!ok) {
        LOGE("spike[%s]: alloc decoder memories FAILED", label);
        goto out;
    }
    dm.cpuMemory = cpu_m;
    dm.gpuMemory = gpu_m;
    dm.cpuGpuMemory = cg_m;

    dc.codecType = ORBIS_VIDEODEC2_CODEC_AVC;
    dc.profile = createProfile;
    dc.maxLevel = createLevel;
    dc.maxDpbFrameCount = createDpb;

    OrbisVideodec2Decoder dec = NULL;
    int rc2 = api->CreateHevcDecoder(&dc, &dm, &dec);
    LOGI("spike[%s]: CreateHevcDecoder(codec=%u prof=%u lvl=%u dpb=%d) "
         "=> 0x%08x dec=%p",
         label, (unsigned)dc.codecType, (unsigned)createProfile,
         (unsigned)createLevel, createDpb, (unsigned)rc2, dec);
    if (dec && api->DeleteDecoder)
        api->DeleteDecoder(dec);
out:
    if (cpu_m) sceKernelReleaseDirectMemory(cpu_o, cpu_s);
    if (gpu_m) sceKernelReleaseDirectMemory(gpu_o, gpu_s);
    if (cg_m) sceKernelReleaseDirectMemory(cg_o, cg_s);
}

/* Round 7: decode acceptance — the captured Sunshine NVENC VPS/SPS/PPS fed
 * to a decoder created with the validated config (100/51/dpb4). Each SPS
 * variant is bit-identical to the rejected stream except the tested field
 * (tier, level_idc, temporal_mvp, VUI presence, VUI timing), so a
 * non-0x811d0303 result pinpoints what videodec2 rejects. AU-format
 * variants (length-prefixed, no-VPS, per-NAL feeding) test delivery shape. */
static const uint8_t spike_au_real[84] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xe6, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75,
    0x30, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_tier0[84] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xe6, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75,
    0x30, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_lvl153[84] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x99, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xe6, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75,
    0x30, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_lvl51[84] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x33, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xe6, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75,
    0x30, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_tmvp0[84] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0x66, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75,
    0x30, 0x04, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_novui[71] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xc8, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_notiming[76] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xe6, 0xa0, 0x20, 0x20, 0x20, 0x08, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01,
    0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_min[71] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0x48, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_lp[84] = {
    0x00, 0x00, 0x00, 0x18, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x7b, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x2a, 0x42, 0x01, 0x01, 0x21,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2,
    0xe6, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75,
    0x30, 0x04, 0x00, 0x00, 0x00, 0x06, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_au_novps[56] = {
    0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x21, 0x60, 0x00, 0x00, 0x03,
    0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x7b, 0xa0, 0x03,
    0xc0, 0x80, 0x10, 0xe5, 0x96, 0xea, 0xe4, 0xc2, 0xe6, 0xa0, 0x20, 0x20,
    0x20, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75, 0x30, 0x04, 0x00, 0x00,
    0x00, 0x01, 0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};
static const uint8_t spike_nal_vps[24] = {
    0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x21, 0x60, 0x00, 0x00, 0x03, 0x00,
    0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x7b, 0xba, 0x02, 0x40
};
static const uint8_t spike_nal_sps[42] = {
    0x42, 0x01, 0x01, 0x21, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x03, 0x00, 0x7b, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5,
    0x96, 0xea, 0xe4, 0xc2, 0xe6, 0xa0, 0x20, 0x20, 0x20, 0x80, 0x00, 0x01,
    0xf4, 0x80, 0x00, 0x75, 0x30, 0x04
};
static const uint8_t spike_nal_pps[6] = {
    0x44, 0x01, 0xc1, 0x73, 0xc1, 0x89
};

static void spike_hevc_feed_probe(const videodec2_api_t *api, const char *label,
                                  uint32_t createCodecType, uint32_t createProfile,
                                  uint32_t createLevel, int createDpb,
                                  const uint8_t *const *aus, const size_t *lens,
                                  int n) {
    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config(&dc, api->queue, 1920, 1080);
    dc.resourceType = ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED;
    dc.codecType = ORBIS_VIDEODEC2_CODEC_AVC;
    dc.profile = ORBIS_VIDEODEC2_PROFILE_HIGH;
    dc.maxLevel = ORBIS_VIDEODEC2_LEVEL_51;
    dc.maxDpbFrameCount = ORBIS_VIDEODEC2_DPB_DEFAULT;

    OrbisVideodec2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);
    int rc = api->QueryHevcDecoderMemoryInfo(&dc, &dm);
    if (rc != 0) {
        LOGI("spike[%s]: query => 0x%08x (skip)", label, (unsigned)rc);
        return;
    }
    void *cpu_m = NULL, *gpu_m = NULL, *cg_m = NULL;
    off_t cpu_o = 0, gpu_o = 0, cg_o = 0;
    size_t cpu_s = 0, gpu_s = 0, cg_s = 0;
    if ((dm.cpuMemorySize &&
         alloc_dmem((size_t)dm.cpuMemorySize, ML_DMEM_TYPE_ONION,
                    &cpu_m, &cpu_o, &cpu_s) < 0) ||
        (dm.gpuMemorySize &&
         alloc_dmem((size_t)dm.gpuMemorySize, ML_DMEM_TYPE_GARLIC,
                    &gpu_m, &gpu_o, &gpu_s) < 0) ||
        (dm.cpuGpuMemorySize &&
         alloc_dmem((size_t)dm.cpuGpuMemorySize, ML_DMEM_TYPE_ONION,
                    &cg_m, &cg_o, &cg_s) < 0)) {
        LOGE("spike[%s]: alloc FAILED", label);
        goto out;
    }
    dm.cpuMemory = cpu_m;
    dm.gpuMemory = gpu_m;
    dm.cpuGpuMemory = cg_m;

    dc.codecType = createCodecType;
    dc.profile = createProfile;
    dc.maxLevel = createLevel;
    dc.maxDpbFrameCount = createDpb;

    OrbisVideodec2Decoder dec = NULL;
    int rc2 = api->CreateHevcDecoder(&dc, &dm, &dec);
    if (!dec || rc2 != 0) {
        LOGI("spike[%s]: CreateHevcDecoder(ct=%u prof=%u lvl=%u dpb=%d) => 0x%08x",
             label, (unsigned)createCodecType, (unsigned)createProfile,
             (unsigned)createLevel, createDpb, (unsigned)rc2);
        goto out;
    }

    void *fb_m = NULL;
    off_t fb_o = 0;
    size_t fb_s = 0;
    if (dm.maxFrameBufferSize &&
        alloc_dmem((size_t)dm.maxFrameBufferSize, ML_DMEM_TYPE_ONION,
                   &fb_m, &fb_o, &fb_s) < 0) {
        LOGE("spike[%s]: fb alloc FAILED", label);
        goto out;
    }

    OrbisVideodec2FrameBuffer fb;
    memset(&fb, 0, sizeof(fb));
    fb.thisSize = sizeof(fb);
    fb.frameBuffer = fb_m;
    fb.frameBufferSize = (uint64_t)fb_s;

    OrbisVideodec2OutputInfo out;
    memset(&out, 0, sizeof(out));
    out.thisSize = sizeof(out);

    for (int i = 0; i < n; i++) {
        OrbisVideodec2InputData in;
        memset(&in, 0, sizeof(in));
        in.thisSize = sizeof(in);
        in.auData = (void *)aus[i];
        in.auSize = (uint64_t)lens[i];
        in.ptsData = 0;
        in.dtsData = 0;
        int rc3 = api->Decode(dec, &in, &fb, &out);
        LOGI("spike[%s]: Decode#%d (%zuB) => 0x%08x", label, i, lens[i],
             (unsigned)rc3);
        if (rc3 != 0)
            break;
    }

    if (fb_m)
        sceKernelReleaseDirectMemory(fb_o, fb_s);
    if (api->DeleteDecoder)
        api->DeleteDecoder(dec);
out:
    if (cpu_m) sceKernelReleaseDirectMemory(cpu_o, cpu_s);
    if (gpu_m) sceKernelReleaseDirectMemory(gpu_o, gpu_s);
    if (cg_m) sceKernelReleaseDirectMemory(cg_o, cg_s);
}

static void spike_hevc_au_probe(const videodec2_api_t *api, const char *label,
                                uint32_t codecType, uint32_t profile,
                                uint32_t level, int dpb,
                                const uint8_t *au, size_t au_len) {
    const uint8_t *aus[1] = { au };
    const size_t lens[1] = { au_len };
    spike_hevc_feed_probe(api, label, codecType, profile, level, dpb,
                          aus, lens, 1);
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
    spike_probe(&api, "AVC-base", 0, ORBIS_VIDEODEC2_CODEC_AVC,
                ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                ORBIS_VIDEODEC2_CODEC_AVC, ORBIS_VIDEODEC2_PROFILE_HIGH,
                ORBIS_VIDEODEC2_LEVEL_51, ORBIS_VIDEODEC2_DPB_DEFAULT,
                1920, 1080);

    if (!api.CreateHevcDecoder) {
        LOGW("spike: no sceVideodec2CreateHevcDecoder — HEVC matrix SKIPPED");
    } else {
        /* Round 5: query fixed to the known-good AVC-style (100/51/dpb4);
         * create swept across AVC-style levels (51/150/153) x profile
         * (100 vs raw HEVC profile_idc 1). The -defaults row recreates the
         * r4 mystery with honest logging (zeros go to the create as-is). */
        spike_hevc_probe(&api, "C-100/51", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "C-p1/lvl51", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         1, 51, ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "C-p1/lvl150", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         1, 150, ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "C-p1/lvl153", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         1, ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "C-p100/lvl150", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, 150,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "C-p100/lvl153", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "C-defaults", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         0, 0, 0, 1920, 1080);
        /* 4K rows once the 1080p create space is known: dpb=6 for L5.1. */
        spike_hevc_probe(&api, "4K-C-p1/lvl150", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         1, 150, ORBIS_VIDEODEC2_DPB_HEVC_4K, 3840, 2160);
        spike_hevc_probe(&api, "4K-C-p100/lvl51", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT,
                         ORBIS_VIDEODEC2_PROFILE_HIGH, ORBIS_VIDEODEC2_LEVEL_51,
                         ORBIS_VIDEODEC2_DPB_HEVC_4K, 3840, 2160);

        /* Round 8: codecType sweep on the HEVC entry point. The r1
         * "codecType=2 is dead" result came from the GENERIC CreateDecoder;
         * CreateHevcDecoder with codecType=2 was never tested. If the
         * decoder instance parses NALs per the codecType field, ct=1 (AVC)
         * rejects every HEVC NAL as INVALID_SEQUENCE — which would explain
         * the universal 0x811d0303 across all of rounds 6-7. */
        LOGI("=== videodec2 spike: decode acceptance matrix (r8) ===");
        spike_hevc_au_probe(&api, "ct1-100/51", 1, 100, 51, 4,
                            spike_au_real, sizeof(spike_au_real));
        spike_hevc_au_probe(&api, "ct2-100/51", 2, 100, 51, 4,
                            spike_au_real, sizeof(spike_au_real));
        spike_hevc_au_probe(&api, "ct2-1/123", 2, 1, 123, 4,
                            spike_au_real, sizeof(spike_au_real));
        spike_hevc_au_probe(&api, "ct2-1/153", 2, 1, 153, 4,
                            spike_au_real, sizeof(spike_au_real));
        spike_hevc_au_probe(&api, "ct2-2/153", 2, 2, 153, 4,
                            spike_au_real, sizeof(spike_au_real));
        spike_hevc_au_probe(&api, "ct2-100/51/dpb3", 2, 100, 51, 3,
                            spike_au_real, sizeof(spike_au_real));
        LOGI("=== videodec2 spike: decode acceptance DONE ===");
    }

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
