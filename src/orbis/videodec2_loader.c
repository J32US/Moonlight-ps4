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

static void spike_probe_export(const videodec2_api_t *api, const char *name) {
    void *p = NULL;
    int rc = sceKernelDlsym(api->module, name, &p);
    LOGI("spike[dlsym]: %s => 0x%08x %p", name, (unsigned)rc, p);
}

/* HEVC create probe. QueryDecoderMemoryInfo is a shared entry point that
 * validates the config as AVC (codec=2 → 0x811d0204, HEVC prof/level →
 * 0x811d0205), so the query must run AVC-valid (codec=1 prof=100 lvl=51);
 * sizes derive from resolution/DPB. CreateHevcDecoder then interprets the
 * same struct with HEVC semantics — createCodec/profile/level/dpb are the
 * values we actually hand it (0 = leave SDK default). */
static void spike_hevc_probe(const videodec2_api_t *api, const char *label,
                             uint32_t resType, uint32_t createCodec,
                             uint32_t createProfile, uint32_t createLevel,
                             int createDpb, int w, int h) {
    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config(&dc, api->queue, w, h);
    dc.resourceType = resType;
    dc.codecType = ORBIS_VIDEODEC2_CODEC_AVC;
    dc.profile = ORBIS_VIDEODEC2_PROFILE_HIGH;
    dc.maxLevel = ORBIS_VIDEODEC2_LEVEL_51;
    dc.maxDpbFrameCount = ORBIS_VIDEODEC2_DPB_DEFAULT;

    OrbisVideodec2DecoderMemoryInfo dm;
    memset(&dm, 0, sizeof(dm));
    dm.thisSize = sizeof(dm);
    int rc = api->QueryDecoderMemoryInfo(&dc, &dm);
    LOGI("spike[%s]: query(AVC-valid resType=%u %dx%d) => 0x%08x "
         "cpu=%llu gpu=%llu cpuGpu=%llu fb=%llu",
         label, (unsigned)resType, w, h, (unsigned)rc,
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

    dc.codecType = createCodec ? createCodec : ORBIS_VIDEODEC2_CODEC_HEVC;
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
        /* Any dedicated HEVC query/delete exports? (OpenOrbis header only
         * lists CreateHevcDecoder — these may or may not exist on 12.00.) */
        spike_probe_export(&api, "sceVideodec2QueryHevcDecoderMemoryInfo");
        spike_probe_export(&api, "sceVideodec2QueryHevcDecoderInfo");
        spike_probe_export(&api, "sceVideodec2GetHevcPictureInfo");
        spike_probe_export(&api, "sceVideodec2DeleteHevcDecoder");

        /* Query AVC-valid; create with HEVC semantics. createCodec=1 keeps
         * AVC's value in the struct (likely overridden internally), the
         * -codec2 row tests whether the create wants codec=2 explicitly. */
        spike_hevc_probe(&api, "HEVC-Main", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         1, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                         ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "HEVC-Main10", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         1, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN10,
                         ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "HEVC-Main-codec2", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         2, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                         ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_DEFAULT, 1920, 1080);
        spike_hevc_probe(&api, "HEVC-defaults", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         1, 0, 0, 0, 1920, 1080);
        spike_hevc_probe(&api, "HEVC-4K", ORBIS_VIDEODEC2_RESOURCE_TYPE_EMBEDDED,
                         1, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                         ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_HEVC_4K, 3840, 2160);
        spike_hevc_probe(&api, "HEVC-4K-res2", 2u,
                         1, ORBIS_VIDEODEC2_PROFILE_HEVC_MAIN,
                         ORBIS_VIDEODEC2_MAX_LEVEL_HEVC_51,
                         ORBIS_VIDEODEC2_DPB_HEVC_4K, 3840, 2160);
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
