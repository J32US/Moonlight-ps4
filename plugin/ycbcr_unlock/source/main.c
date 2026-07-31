// GoldHEN plugin 1.49:
// Post-kpatch: RegisterBuffers n=1 → ioctl OK; SubmitFlip YCbCr failed
// 0x80290001 with FIX_MODE=2 (tag +0x08=2 BGRA inconsistent). Default:
// passthrough natural tag + priv BIT_YCC_EXT + SubmitFlip hook.
#include <Common.h>
#include <orbis/libkernel.h>

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

#ifndef BISECT_LEVEL
#define BISECT_LEVEL 5
#endif

//   0 = passthrough (DEFAULT 1.49: do not force BGRA tag; kpatch is enough)
//   1 = AddBuffer
//   2 = golden BGRA layout: +0x08=2, +0x10=addr[n-1], +0x18=+0x20=0
//   3 = +0x00 = &addresses[0] + fill +0x10/+0x18/+0x20
//   4 = dump addr[0..2] into +0x00/+0x08/+0x10
//   5 = a5a5→0 + fill +0x10/+0x18/+0x20
#ifndef FIX_MODE
#define FIX_MODE 0
#endif

#define MARKER_PATH "/data/moonlight/ycbcr_unlock.loaded"
#define DIAG_PATH   "/data/moonlight/vo_diag.txt"
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))
#define attr_public __attribute__((visibility("default")))

#define YCBCR420_BT709 0x08322200u
#define PORT_PRIV_OFF 0x450u
#define PORT_OFF_46C  0x46Cu  /* drv_method: cmp [r13+0x46c],0 → else INVALID */
#define PORT_OFF_4A8  0x4A8u
#define PORT_OFF_478  0x478u  /* stub h05 @+0x881: cmp [r15+0x478],0 */
#define PORT_OFF_480  0x480u  /* stub h05 @+0x7d0: cmp [r15+0x480],0 */
#define PORT_OFF_SLOT 0x150u  /* validate CommitTail: [base+idx*4+0x150]=1 */
#define TAG_YCC 2ull
#define TAG_BIT0 1ull
#define BIT_YCC_EXT 0x20u
#define TEX_ICON_SYSTEM "cxml://psnotification/tex_icon_system"
#define OFF_VALIDATE_FROM_REGBUFS 0x5660u
#define OFF_MOV_EBX_ERR   0x3A7u
#define OFF_SUCCESS       0x5F6u
#define OFF_LEA_BPP       0x813u
#define OFF_CAP_EBX       0x99Bu  /* bb 01 00 29 80 in capacity block */
#define OFF_CAP_JL        0x9A8u  /* 0f 8c .. jl fail capacity */
#define OFF_FMT_JNE_3C    0x7C0u  /* 0f 85 .. jne +0x3C */
#define OFF_EARLY_01      0x3Cu
#define OFF_EBX_0A        0x49u  /* bb 0a 00 29 80 — NOT +0x4A */
#define OFF_BA_01_STUB    0x13F2u /* ba 01 00 29 80 ; e9 → fail (case 0x16) */
#define OFF_FAIL_EPILOGUE 0xB4Eu
#define OFF_JT_JA         0x13E0u /* 77 2d ja ; then lea table + jmp [rdx] */
#define OFF_JT_LEA        0x13E2u
#define OFF_IOCTL_JS      0x132Cu /* 0f 88 9c 00 00 00  js +0x13CE (ioctl fail) */
#define JT_BAD_INDEX      0x16u

typedef struct {
    int32_t format;
    int32_t tmode;
    int32_t aspect;
    uint32_t width;
    uint32_t height;
    uint32_t pixelPitch;
    uint64_t reserved[2];
} VoBufferAttribute;

int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t startIndex,
                                   void *const *addresses, int32_t bufferNum,
                                   const VoBufferAttribute *attribute);
int32_t sceVideoOutUnregisterBuffers(int32_t handle, int32_t attributeIndex);
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t bufferIndex,
                              uint32_t flipMode, int64_t flipArg);
int32_t sceVideoOutAddBufferYccPrivilege(int32_t handle);
int32_t sceVideoOutSysUpdatePrivilege(int32_t handle);

attr_public const char *g_pluginName = "ycbcr_unlock";
attr_public const char *g_pluginDesc = "YCbCr: force ioctl addrs n=2 (1.63)";
attr_public const char *g_pluginAuth = "moonlight-ps4";
attr_public uint32_t g_pluginVersion = 0x0000013f; // 1.63

static struct jailbreak_backup s_jb;
static int s_jb_ok;
static int s_plugin_started;
static int s_core_ready;
static char s_marker_extra[160];

static void *s_fn_regbufs;
static void *s_fn_yccpriv;
static void *s_fn_regattr;
static void *s_fn_addbuf;
static void *s_fn_unregattr;
static void *(*s_get_ctx)(int32_t handle);
static uint8_t *s_fn_validate;
static int s_patched;
static int s_spy_installed;
static uint8_t s_spy_orig[5];
static uint8_t *s_spy_site;
static uint8_t s_orig_mov_ebx[5];
static uint8_t s_orig_lea[8];
static uint8_t s_orig_jne3c[6];
static uint8_t s_orig_early01[10];
static uint8_t s_orig_ebx0a[5];
static uint8_t s_orig_cap_ebx[5];
static uint8_t s_orig_capjl[6];
static uint8_t s_orig_ioctl_js[6];
static int32_t s_orig_jt_ent16;
static uint8_t *s_cap_ebx_site;
static uint8_t *s_capjl_site;
static uint8_t *s_jt_ent16_site;
#define H05_MAX 8
static struct { uint8_t *site; uint8_t orig[6]; int len; } s_h05[H05_MAX];
static int s_h05_n;

static void ensure_moonlight_dir(void);
static void ensure_core_ready(void);
static void write_marker(int jb_rc);
static void write_diag_base(void);
static int s_have_lea;
static int s_have_jne3c;
static int s_have_early01;
static int s_have_ebx0a;
static int s_have_cap_ebx;
static int s_have_capjl;
static int s_have_ioctl_js;
static int s_have_jt;
static int s_have_ioctl_spy;

/* Referenced from ioctl_spy asm; with BISECT_LEVEL<4 there is no C use
 * and the linker would drop it → undefined symbol. Force retention. */
static void *s_ioctl_orig_fn __attribute__((used));
static uint8_t *s_ioctl_call_site;
static uint8_t s_ioctl_orig_bytes[5];
static int64_t s_ioctl_rdi;
static int64_t s_ioctl_rsi;
static int64_t s_ioctl_rdx;
static int32_t s_ioctl_ret;
#define IOCTL_DUMP_LEN 0x100
static uint8_t s_ioctl_arg_before[IOCTL_DUMP_LEN];
static uint8_t s_ioctl_arg_after[IOCTL_DUMP_LEN];
static int s_ioctl_arg_ok;
static int s_ioctl_called;
static int s_ioctl_arg_filled; /* some slot != 0 and != 0xa5a5 */
static int s_have_drv46c;

/* C path (FIX_MODE=2): buffer addresses the hook received, to inject
 * into the ioctl struct if it arrives without pointers (a5a5 sentinel). */
static void *const *s_hook_addresses;
static int32_t s_hook_bufcount;
static int s_fixup_done;

void *s_drv_method_slot;
static uint8_t *s_success_abs;
static uint8_t *s_continue_abs;

typedef int32_t (*fn_regattr_t)(int32_t, uint32_t, const VoBufferAttribute *);
typedef int32_t (*fn_addbuf_t)(int32_t, int32_t, int32_t, void *);
typedef int32_t (*fn_unregattr_t)(int32_t, uint32_t);

HOOK_INIT(sceVideoOutRegisterBuffers);
HOOK_INIT(sceVideoOutSubmitFlip);

void vo_call_spy(void);
__asm__(
    ".text\n"
    ".globl vo_call_spy\n"
    ".type vo_call_spy, @function\n"
    "vo_call_spy:\n"
    "  movq %r14, %r8\n"
    "  movq %rax, s_drv_method_slot(%rip)\n"
    "  callq *%rax\n"
    "  ret\n"
    ".size vo_call_spy, .-vo_call_spy\n"
);

void ioctl_spy_capture_before(void);
void ioctl_spy_capture_after(void);

void ioctl_spy_capture_before(void) {
    s_ioctl_called = 1;
    s_ioctl_arg_filled = 0;
    memset(s_ioctl_arg_before, 0, sizeof(s_ioctl_arg_before));
    if (!s_ioctl_rdx)
        return;

    uint8_t *arg = (uint8_t *)(uintptr_t)s_ioctl_rdx;
    memcpy(s_ioctl_arg_before, arg, IOCTL_DUMP_LEN);

    /* filled: buffer ptr present in +0x10..+0x28 (do not count a5a5 magic). */
    for (int i = 0x10; i < 0x30; i += 8) {
        uint64_t q = 0;
        memcpy(&q, s_ioctl_arg_before + i, 8);
        if (q != 0) {
            s_ioctl_arg_filled = 1;
            break;
        }
    }

#if FIX_MODE == 0 || FIX_MODE == 2 || FIX_MODE == 3 || FIX_MODE == 4 || FIX_MODE == 5
    if (s_hook_addresses && s_hook_bufcount > 0) {
        int n = s_hook_bufcount < 3 ? s_hook_bufcount : 3;
        int wrote = 0;

#if FIX_MODE == 0
        /* Force hook VAs at +0x10/+0x18/+0x20.
         * n=2: lib left fb[1],fb[1] (vo_diag 0.7.62) — filling zeros alone is not enough. */
        for (int i = 0; i < n; i++) {
            size_t off = 0x10 + (size_t)i * 8;
            uint64_t cur = 0;
            memcpy(&cur, arg + off, 8);
            uint64_t want = (uint64_t)(uintptr_t)s_hook_addresses[i];
            if (want != 0 && cur != want) {
                memcpy(arg + off, &want, 8);
                wrote = 1;
            }
        }
#elif FIX_MODE == 4
        /* Overwrite a5a5: embedded array at +0x00/+0x08/+0x10. */
        for (int i = 0; i < n; i++) {
            uint64_t want = (uint64_t)(uintptr_t)s_hook_addresses[i];
            memcpy(arg + (size_t)i * 8, &want, 8);
            wrote = 1;
        }
#elif FIX_MODE == 2
        /* Golden BGRA layout (1.44): +0x08=2, +0x10=addr[last], +0x18=+0x20=0.
         * Filling all 3 VAs was the bug: BGRA OK leaves them at 0. */
        {
            uint64_t tag = 2, z = 0;
            uint64_t last = (uint64_t)(uintptr_t)s_hook_addresses[n - 1];
            uint64_t cur8 = 0, cur10 = 0, cur18 = 0, cur20 = 0;
            memcpy(&cur8, arg + 0x08, 8);
            memcpy(&cur10, arg + 0x10, 8);
            memcpy(&cur18, arg + 0x18, 8);
            memcpy(&cur20, arg + 0x20, 8);
            if (cur8 != tag) {
                memcpy(arg + 0x08, &tag, 8);
                wrote = 1;
            }
            if (cur10 != last) {
                memcpy(arg + 0x10, &last, 8);
                wrote = 1;
            }
            if (cur18 != 0) {
                memcpy(arg + 0x18, &z, 8);
                wrote = 1;
            }
            if (cur20 != 0) {
                memcpy(arg + 0x20, &z, 8);
                wrote = 1;
            }
        }
#elif FIX_MODE == 5
        /* Clear a5a5 → 0; fill +0x10/+0x18/+0x20 (no nested ptr). */
        {
            uint64_t z = 0;
            uint64_t head = 0;
            memcpy(&head, arg, 8);
            if (head == 0xa5a5ull || (head & 0xffffull) == 0xa5a5ull) {
                memcpy(arg, &z, 8);
                wrote = 1;
            }
        }
        for (int i = 0; i < n; i++) {
            size_t off = 0x10 + (size_t)i * 8;
            uint64_t cur = 0;
            memcpy(&cur, arg + off, 8);
            uint64_t want = (uint64_t)(uintptr_t)s_hook_addresses[i];
            if (cur == 0 || cur != want) {
                memcpy(arg + off, &want, 8);
                wrote = 1;
            }
        }
#else
        /* Fill embedded slots +0x10/+0x18/+0x20 (FIX 3). */
        for (int i = 0; i < n; i++) {
            size_t off = 0x10 + (size_t)i * 8;
            uint64_t cur = 0;
            memcpy(&cur, arg + off, 8);
            uint64_t want = (uint64_t)(uintptr_t)s_hook_addresses[i];
            if (cur == 0 || cur != want) {
                memcpy(arg + off, &want, 8);
                wrote = 1;
            }
        }
#if FIX_MODE == 3
        /* +0x00 = pointer to caller's original array (replaces a5a5). */
        {
            uint64_t ptr = (uint64_t)(uintptr_t)s_hook_addresses;
            uint64_t head = 0;
            memcpy(&head, arg, 8);
            if (head == 0xa5a5ull || (head & 0xffffull) == 0xa5a5ull || head == 0) {
                memcpy(arg, &ptr, 8);
                wrote = 1;
            }
        }
#endif
#endif
        if (wrote) {
            s_fixup_done = 1;
            s_ioctl_arg_filled = 1;
            memcpy(s_ioctl_arg_before, arg, IOCTL_DUMP_LEN);
        }
    }
#endif
}

void ioctl_spy_capture_after(void) {
    memset(s_ioctl_arg_after, 0, sizeof(s_ioctl_arg_after));
    if (s_ioctl_rdx)
        memcpy(s_ioctl_arg_after, (void *)(uintptr_t)s_ioctl_rdx, IOCTL_DUMP_LEN);
    s_ioctl_arg_ok = 1;
}

void ioctl_spy(void);
__asm__(
    ".text\n"
    ".globl ioctl_spy\n"
    ".type ioctl_spy, @function\n"
    "ioctl_spy:\n"
    "  movq %rdi, s_ioctl_rdi(%rip)\n"
    "  movq %rsi, s_ioctl_rsi(%rip)\n"
    "  movq %rdx, s_ioctl_rdx(%rip)\n"
    "  subq $8, %rsp\n"
    "  call ioctl_spy_capture_before\n"
    "  movq s_ioctl_rdi(%rip), %rdi\n"
    "  movq s_ioctl_rsi(%rip), %rsi\n"
    "  movq s_ioctl_rdx(%rip), %rdx\n"
    "  movq s_ioctl_orig_fn(%rip), %rax\n"
    "  callq *%rax\n"
    "  movl %eax, s_ioctl_ret(%rip)\n"
    "  pushq %rax\n"
    "  call ioctl_spy_capture_after\n"
    "  popq %rax\n"
    "  addq $8, %rsp\n"
    "  ret\n"
    ".size ioctl_spy, .-ioctl_spy\n"
);

void ycc_format_gate(void);
__asm__(
    ".text\n"
    ".globl ycc_format_gate\n"
    ".type ycc_format_gate, @function\n"
    "ycc_format_gate:\n"
    "  cmpl $0x08322200, %esi\n"
    "  jne 1f\n"
    /* 1.25: do NOT touch edx (1.12 GNM hint → jump table [rdx] → 0x80290001) */
    "  movq s_success_abs(%rip), %rax\n"
    "  jmpq *%rax\n"
    "1:\n"
    "  movl $0x80290003, %ebx\n"
    "  movq s_continue_abs(%rip), %rax\n"
    "  jmpq *%rax\n"
    ".size ycc_format_gate, .-ycc_format_gate\n"
);

static void notify(const char *msg) {
    /* No toasts: plugin klog only. */
    klog("[ycbcr_unlock] %s\n", msg);
}

static void poke(void *addr, const void *data, size_t len) {
    sceKernelMprotect(addr, len, VM_PROT_ALL);
    memcpy(addr, data, len);
}

static int name_has(const char *name, const char *sub) {
    if (!name || !sub) return 0;
    size_t n = strlen(name), m = strlen(sub);
    if (m > n) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        for (; j < m; j++) {
            char a = name[i + j], b = sub[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == m) return 1;
    }
    return 0;
}

static void *rel32_target(uint8_t *call_insn) {
    if (!call_insn || call_insn[0] != 0xE8) return NULL;
    int32_t rel;
    memcpy(&rel, call_insn + 1, 4);
    return call_insn + 5 + rel;
}

static int resolve_videoout(void) {
    OrbisKernelModule mods[256];
    size_t avail = 0;
    s_fn_regbufs = s_fn_yccpriv = NULL;
    s_get_ctx = NULL;
    if (sceKernelGetModuleList(mods, 256, &avail) != 0 || avail == 0)
        return -1;
    for (size_t i = 0; i < avail && i < 256; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(mods[i], &info) != 0) continue;
        if (!name_has(info.name, "videoout")) continue;
        void *fn = NULL;
        if (sceKernelDlsym(mods[i], "sceVideoOutRegisterBuffers", &fn) == 0)
            s_fn_regbufs = fn;
        fn = NULL;
        if (sceKernelDlsym(mods[i], "sceVideoOutAddBufferYccPrivilege", &fn) == 0)
            s_fn_yccpriv = fn;
        fn = NULL;
        if (sceKernelDlsym(mods[i], "sceVideoOutRegisterBufferAttribute", &fn) == 0)
            s_fn_regattr = fn;
        fn = NULL;
        if (sceKernelDlsym(mods[i], "sceVideoOutAddBuffer", &fn) == 0)
            s_fn_addbuf = fn;
        fn = NULL;
        if (sceKernelDlsym(mods[i], "sceVideoOutUnregisterBufferAttribute", &fn) == 0)
            s_fn_unregattr = fn;
        if (s_fn_yccpriv && ((uint8_t *)s_fn_yccpriv)[6] == 0xE8)
            s_get_ctx = (void *(*)(int32_t))rel32_target((uint8_t *)s_fn_yccpriv + 6);
        return (s_fn_regbufs && s_fn_yccpriv) ? 0 : -1;
    }
    return -1;
}

static void hex_dump_fn(FILE *f, const char *label, const uint8_t *fn, size_t n) {
    if (!fn) { fprintf(f, "%s: NULL\n", label); return; }
    fprintf(f, "%s @ %p (%zu bytes):\n", label, (void *)fn, n);
    for (size_t i = 0; i < n; i += 16) {
        fprintf(f, "  %04zx:", i);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            fprintf(f, " %02x", fn[i + j]);
        fprintf(f, "\n");
    }
}

static void install_call_spy(void) {
    if (s_spy_installed || !s_fn_regbufs) return;
    uint8_t *site = (uint8_t *)s_fn_regbufs + 0x57;
    if (site[0] != 0x4d || site[3] != 0xff || site[4] != 0xd0) return;
    memcpy(s_spy_orig, site, 5);
    s_spy_site = site;
    intptr_t rel = (uint8_t *)&vo_call_spy - (site + 5);
    if (rel < (intptr_t)INT32_MIN || rel > (intptr_t)INT32_MAX) return;
    uint8_t patch[5] = { 0xE8, 0, 0, 0, 0 };
    int32_t r32 = (int32_t)rel;
    memcpy(patch + 1, &r32, 4);
    poke(site, patch, 5);
    s_spy_installed = 1;
}

static void uninstall_call_spy(void) {
    if (s_spy_installed && s_spy_site) {
        poke(s_spy_site, s_spy_orig, 5);
        s_spy_installed = 0;
    }
}

static void resolve_validate(void) {
    if (!s_fn_validate && s_fn_regbufs)
        s_fn_validate = (uint8_t *)s_fn_regbufs - OFF_VALIDATE_FROM_REGBUFS;
}

static int patch_bit_jz(uint8_t *p, size_t size, uint8_t bit, FILE *f, const char *tag) {
    int n = 0;
    static const uint8_t nop6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    for (size_t i = 0; i + 14 < size; i++) {
        size_t off = i;
        if (p[off] == 0x41) off++;
        if (p[off] != 0xF6) continue;
        uint8_t modrm = p[off + 1];
        if ((modrm & 0xC0) != 0x80 || (modrm & 0x38) != 0x00) continue;
        uint32_t disp;
        memcpy(&disp, p + off + 2, 4);
        if (disp != PORT_PRIV_OFF || p[off + 6] != bit) continue;
        size_t jcc = off + 7;
        if (p[jcc] == 0x0F && p[jcc + 1] == 0x84) {
            if (f) fprintf(f, "NOP %s jz @ +0x%zx\n", tag, jcc);
            poke(p + jcc, nop6, 6);
            n++;
        }
    }
    return n;
}

static void *port_inner(int32_t handle) {
    if (!s_get_ctx) return NULL;
    void *ctx = s_get_ctx(handle);
    if (!ctx) return NULL;
    return *(void **)ctx;
}

static uint32_t load_u32(void *inner, uint32_t off) {
    uint32_t v = 0;
    if (!inner) return 0;
    memcpy(&v, (uint8_t *)inner + off, 4);
    return v;
}

static void store_u32(void *inner, uint32_t off, uint32_t v) {
    if (!inner) return;
    sceKernelMprotect((uint8_t *)inner + off, 4, VM_PROT_ALL);
    memcpy((uint8_t *)inner + off, &v, 4);
}

static void or_safe_tags_and_flags(int32_t handle, FILE *f) {
    void *inner = port_inner(handle);
    if (!inner) {
        if (f) fprintf(f, "port inner=NULL\n");
        return;
    }

    uint64_t tagged = 0;
    memcpy(&tagged, (uint8_t *)inner + PORT_PRIV_OFF, 8);
    /* BIT_YCC_EXT (0x20): validate only NOPs the jz; SubmitFlip may require it. */
    uint64_t neu = tagged | TAG_YCC | TAG_BIT0 | (uint64_t)BIT_YCC_EXT;
    if (neu != tagged) {
        sceKernelMprotect((uint8_t *)inner + PORT_PRIV_OFF, 8, VM_PROT_ALL);
        memcpy((uint8_t *)inner + PORT_PRIV_OFF, &neu, 8);
    }

    uint32_t v4a8 = load_u32(inner, PORT_OFF_4A8);
    if (v4a8 == 0)
        store_u32(inner, PORT_OFF_4A8, 1);

    /* h05 stubs read these; force 1 to avoid INVALID_RESOLUTION. */
    uint32_t v478 = load_u32(inner, PORT_OFF_478);
    uint32_t v480 = load_u32(inner, PORT_OFF_480);
    if (v478 == 0)
        store_u32(inner, PORT_OFF_478, 1);
    if (v480 == 0)
        store_u32(inner, PORT_OFF_480, 1);

    /* drv_method: cmp [r13+0x46c],0 ; je → INVALID. Force 1. */
    uint32_t v46c = load_u32(inner, PORT_OFF_46C);
    if (v46c == 0)
        store_u32(inner, PORT_OFF_46C, 1);

    if (f) {
        fprintf(f, "priv 0x%llx -> 0x%llx\n",
                (unsigned long long)tagged, (unsigned long long)neu);
        fprintf(f, "port+0x4a8=0x%x -> 0x%x\n",
                v4a8, load_u32(inner, PORT_OFF_4A8));
        fprintf(f, "port+0x46c=0x%x -> 0x%x\n",
                v46c, load_u32(inner, PORT_OFF_46C));
        fprintf(f, "port+0x478=0x%x -> 0x%x  +0x480=0x%x -> 0x%x\n",
                v478, load_u32(inner, PORT_OFF_478),
                v480, load_u32(inner, PORT_OFF_480));
    }
}

/* After RegisterBuffers OK: ensure slot marks that SubmitFlip reads. */
static void force_slot_marks(int32_t handle, int32_t startIndex, int32_t bufferNum,
                             FILE *f) {
    void *inner = port_inner(handle);
    if (!inner || bufferNum < 1)
        return;
    for (int32_t i = 0; i < bufferNum; i++) {
        uint32_t off = PORT_OFF_SLOT + (uint32_t)(startIndex + i) * 4u;
        uint32_t was = load_u32(inner, off);
        if (was != 1)
            store_u32(inner, off, 1);
        if (f)
            fprintf(f, "slot[%d] @+0x%x =0x%x -> 0x%x\n",
                    (int)(startIndex + i), off, was, load_u32(inner, off));
    }
}

/* Scan for candidate pitches in the port after register. */
static void scan_port_stride(int32_t handle, FILE *f) {
    void *inner = port_inner(handle);
    if (!inner || !f)
        return;
    int hits_7680 = 0, hits_1920 = 0;
    for (uint32_t off = 0; off < 0x1800u; off += 4u) {
        uint32_t v = load_u32(inner, off);
        if (v == 7680u) {
            fprintf(f, "stride_scan: +0x%x = 7680\n", off);
            if (++hits_7680 >= 16)
                break;
        } else if (v == 1920u) {
            if (hits_1920 < 8)
                fprintf(f, "stride_scan: +0x%x = 1920\n", off);
            hits_1920++;
        }
    }
    fprintf(f, "stride_scan: hits_7680=%d hits_1920=%d\n", hits_7680, hits_1920);
}

/*
 * 1.61 dump: +0x1a8/+0x1d0 = 0 (empty UV offset) while +0x1e0 = pitch4*h.
 * Green-gray = DCE reads chroma from Y (offset 0 or w*h). Force UV @ pitch4*h
 * aligned with hrep4 blit. Do not touch pitch (1.60 → image shifted left).
 */
static void patch_port_uv_offset(int32_t handle, uint32_t width, uint32_t height,
                                 FILE *f) {
    void *inner = port_inner(handle);
    if (!inner || width == 0 || height == 0)
        return;

    uint32_t uv_off = width * 4u * height; /* 7680*1080 with hrep4 */
    static const uint32_t k_uv_slots[] = { 0x1a8u, 0x1d0u };
    int n = 0;

    if (f) {
        fprintf(f, "uvfix: want=%u (pitch4*h) before:", uv_off);
        for (size_t i = 0; i < sizeof(k_uv_slots) / sizeof(k_uv_slots[0]); i++)
            fprintf(f, " +0x%x=%u", k_uv_slots[i], load_u32(inner, k_uv_slots[i]));
        fprintf(f, " +0x1e0=%u\n", load_u32(inner, 0x1e0u));
    }

    for (size_t i = 0; i < sizeof(k_uv_slots) / sizeof(k_uv_slots[0]); i++) {
        uint32_t off = k_uv_slots[i];
        uint32_t was = load_u32(inner, off);
        if (was != uv_off) {
            store_u32(inner, off, uv_off);
            n++;
        }
        if (f)
            fprintf(f, "uvfix: +0x%x %u -> %u\n", off, was, load_u32(inner, off));
    }

    /* If +0x1e0 is not the Y hrep4 size, align it too. */
    {
        uint32_t was = load_u32(inner, 0x1e0u);
        if (was != uv_off) {
            store_u32(inner, 0x1e0u, uv_off);
            if (f)
                fprintf(f, "uvfix: +0x1e0 %u -> %u (plane size)\n",
                        was, load_u32(inner, 0x1e0u));
            n++;
        }
    }

    if (f)
        fprintf(f, "uvfix: patched=%d\n", n);
    klog("[ycbcr_unlock] uvfix slots→%u n=%d\n", uv_off, n);
}

/*
 * Compact post-fix dump (diagnostics).
 */
static void dump_port_uv_candidates(int32_t handle, uint32_t width, uint32_t height,
                                    FILE *f) {
    void *inner = port_inner(handle);
    if (!inner || !f || width == 0)
        return;

    fprintf(f, "uvcand: w=%u h=%u\n", width, height);
    for (uint32_t off = 0x190u; off < 0x1f0u; off += 4u) {
        uint32_t v = load_u32(inner, off);
        fprintf(f, "  port+0x%x = %u (0x%x)\n", off, v, v);
    }
}

static int patch_e1_sources(FILE *f) {
    int ok = 0;
    if (!s_fn_validate) return 0;
    static const uint8_t nop6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    // 1) NOP jne +0x7C0 -> +0x3C (format class bit31)
    uint8_t *jne = s_fn_validate + OFF_FMT_JNE_3C;
    if (jne[0] == 0x0F && jne[1] == 0x85) {
        int32_t rel;
        memcpy(&rel, jne + 2, 4);
        size_t tgt = (size_t)((intptr_t)OFF_FMT_JNE_3C + 6 + rel);
        if (tgt == OFF_EARLY_01) {
            memcpy(s_orig_jne3c, jne, 6);
            poke(jne, nop6, 6);
            s_have_jne3c = 1;
            ok++;
            if (f) fprintf(f, "E1: NOP jne@+0x7C0 (was -> +0x3C)\n");
        } else if (f) {
            fprintf(f, "E1: jne@+0x7C0 target +0x%zx (expected +0x3C)\n", tgt);
        }
    } else if (f) {
        fprintf(f, "E1: jne@+0x7C0 mismatch %02x %02x\n", jne[0], jne[1]);
    }

    // 2) Early fail stub +0x3C → jmp +0x46 (start of 41 89 f6), not +0x47
    uint8_t *early = s_fn_validate + OFF_EARLY_01;
    if (early[0] == 0xBB && early[5] == 0xE9) {
        memcpy(s_orig_early01, early, 10);
        uint8_t patch[10] = {
            0xEB, 0x08, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
        };
        poke(early, patch, 10);
        s_have_early01 = 1;
        ok++;
        if (f) fprintf(f, "E1: +0x3C fail stub -> jmp +0x46 (mov r14d,esi)\n");
    } else if (f) {
        fprintf(f, "E1: early@+0x3C mismatch %02x %02x\n", early[0], early[5]);
    }

    // 3) Clear default mov ebx, INVALID_INDEX (0x0A) @ +0x49
    uint8_t *b0a = s_fn_validate + OFF_EBX_0A;
    if (b0a[0] == 0xBB && b0a[1] == 0x0A && b0a[2] == 0x00 &&
        b0a[3] == 0x29 && b0a[4] == 0x80) {
        memcpy(s_orig_ebx0a, b0a, 5);
        static const uint8_t ebx0[5] = { 0xBB, 0x00, 0x00, 0x00, 0x00 };
        poke(b0a, ebx0, 5);
        s_have_ebx0a = 1;
        ok++;
        if (f) fprintf(f, "0A: mov ebx,0x0A -> 0 @ +0x49\n");
    } else if (f) {
        fprintf(f, "0A: ebx@+0x49 mismatch %02x %02x %02x\n",
                b0a[0], b0a[1], b0a[2]);
    }

    // Do NOT nop cmp esi,3 / jcc — 1.20 crash. Leave the check intact.
    if (f)
        hex_dump_fn(f, "AfterPrologue+0x46", s_fn_validate + 0x46, 48);
    return ok;
}

static int is_error_stub(const uint8_t *t) {
    if (!t) return 1;
    /* mov r32, 0x8029xxxx */
    if ((t[0] >= 0xB8 && t[0] <= 0xBF) && t[2] == 0x00 && t[3] == 0x29 && t[4] == 0x80)
        return 1;
    /* mov r14d, 0x8029xxxx */
    if (t[0] == 0x41 && t[1] == 0xBE && t[3] == 0x00 && t[4] == 0x29 && t[5] == 0x80)
        return 1;
    return 0;
}

/* Stubs 0x80290005: dump context. Find cmp imm 1080/1088 and NOP the reject
 * jcc (fall-through to stub or jump to stub). */
static int patch_height_05(FILE *f) {
    if (!s_fn_validate) return 0;
    int nops = 0;
    s_h05_n = 0;

    for (size_t i = 0x400; i + 8 < 0x1400; i++) {
        /* cmp r/m32, imm32: 81 /7 or 41 81 /7 — imm = 0x438 (1080) / 0x440 (1088) */
        int is_cmp = 0;
        size_t imm_off = 0;
        if (s_fn_validate[i] == 0x81 && (s_fn_validate[i + 1] & 0x38) == 0x38) {
            is_cmp = 1;
            imm_off = i + 2;
        } else if (s_fn_validate[i] == 0x41 && s_fn_validate[i + 1] == 0x81 &&
                   (s_fn_validate[i + 2] & 0x38) == 0x38) {
            is_cmp = 1;
            imm_off = i + 3;
        } else if (s_fn_validate[i] == 0x3D) {
            is_cmp = 1;
            imm_off = i + 1;
        }
        if (!is_cmp) continue;
        uint32_t imm = 0;
        memcpy(&imm, s_fn_validate + imm_off, 4);
        if (imm != 1080u && imm != 1088u) continue;
        if (f) fprintf(f, "h05: cmp imm=%u @+0x%zx\n", imm, i);

        size_t j = imm_off + 4;
        if (j + 6 > 0x1400) continue;
        int len = 0;
        if (s_fn_validate[j] == 0x0F && (s_fn_validate[j + 1] & 0xF0) == 0x80)
            len = 6;
        else if (s_fn_validate[j] >= 0x70 && s_fn_validate[j] <= 0x7F)
            len = 2;
        if (len <= 0 || s_h05_n >= H05_MAX) continue;
        s_h05[s_h05_n].site = s_fn_validate + j;
        s_h05[s_h05_n].len = len;
        memcpy(s_h05[s_h05_n].orig, s_fn_validate + j, (size_t)len);
        {
            uint8_t nops6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
            poke(s_fn_validate + j, nops6, (size_t)len);
        }
        if (f) fprintf(f, "h05: NOP jcc@+0x%zx after cmp%u len=%d\n", j, imm, len);
        s_h05_n++;
        nops++;
    }

    for (size_t i = 0x500; i + 5 < 0x1400; i++) {
        uint8_t op = s_fn_validate[i];
        if (op < 0xB8 || op > 0xBF) continue;
        if (s_fn_validate[i + 1] != 0x05 || s_fn_validate[i + 2] != 0x00 ||
            s_fn_validate[i + 3] != 0x29 || s_fn_validate[i + 4] != 0x80)
            continue;
        if (f) {
            fprintf(f, "h05: stub mov@+0x%zx (%02x)\n", i, op);
            size_t dump_from = (i > 32) ? i - 32 : 0;
            hex_dump_fn(f, "h05ctx", s_fn_validate + dump_from, 48);
        }
    }
    if (f) fprintf(f, "h05: nops=%d\n", nops);
    return nops;
}

/* Commit path: spy the real ioctl (no NOP js). */
static int install_ioctl_spy(FILE *f) {
    if (!s_fn_validate || s_have_ioctl_spy) return s_have_ioctl_spy;
    uint8_t *call = s_fn_validate + 0x1325;
    if (call[0] != 0xE8) {
        if (f) fprintf(f, "ioctl_spy: no call@+0x1325 (%02x)\n", call[0]);
        return 0;
    }
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    s_ioctl_orig_fn = call + 5 + rel;
    s_ioctl_call_site = call;
    memcpy(s_ioctl_orig_bytes, call, 5);

    intptr_t d = (uint8_t *)&ioctl_spy - (call + 5);
    if (d < (intptr_t)INT32_MIN || d > (intptr_t)INT32_MAX) {
        if (f) fprintf(f, "ioctl_spy: rel out of range\n");
        return 0;
    }
    uint8_t patch[5] = { 0xE8, 0, 0, 0, 0 };
    int32_t r32 = (int32_t)d;
    memcpy(patch + 1, &r32, 4);
    poke(call, patch, 5);
    s_have_ioctl_spy = 1;
    if (f) {
        fprintf(f, "ioctl_spy: installed; orig=%p\n", s_ioctl_orig_fn);
        hex_dump_fn(f, "ioctl_orig", (const uint8_t *)s_ioctl_orig_fn, 64);
    }
    return 1;
}

static void capture_ioctl_arg(FILE *f) {
    if (!s_have_ioctl_spy) return;
    if (f) {
        fprintf(f, "ioctl: called=%d filled=%d rdi/fd=%lld rsi/cmd=0x%llx rdx/arg=%p ret=0x%08x\n",
                s_ioctl_called, s_ioctl_arg_filled,
                (long long)s_ioctl_rdi, (unsigned long long)s_ioctl_rsi,
                (void *)(uintptr_t)s_ioctl_rdx, (unsigned)s_ioctl_ret);
        if (s_ioctl_arg_ok) {
            /* +0x00 magic; +0x08 tag (BGRA=2); +0x10/+0x18/+0x20 buffer VAs. */
            uint64_t q0 = 0, q1 = 0, q2 = 0, q8 = 0, magic = 0;
            memcpy(&magic, s_ioctl_arg_before + 0x00, 8);
            memcpy(&q8, s_ioctl_arg_before + 0x08, 8);
            memcpy(&q0, s_ioctl_arg_before + 0x10, 8);
            memcpy(&q1, s_ioctl_arg_before + 0x18, 8);
            memcpy(&q2, s_ioctl_arg_before + 0x20, 8);
            fprintf(f, "  ioctl_fields: +0x00=0x%llx +0x08=0x%llx +0x10=0x%llx +0x18=0x%llx +0x20=0x%llx\n",
                    (unsigned long long)magic,
                    (unsigned long long)q8,
                    (unsigned long long)q0, (unsigned long long)q1,
                    (unsigned long long)q2);
            hex_dump_fn(f, "ioctl_before", s_ioctl_arg_before, IOCTL_DUMP_LEN);
            hex_dump_fn(f, "ioctl_after", s_ioctl_arg_after, IOCTL_DUMP_LEN);
        }
    }
}

/* After the first spy: NOP je after cmp [r13+0x46c] in drv_method. */
static void patch_drv_method_46c(FILE *f) {
    if (s_have_drv46c || !s_drv_method_slot) return;
    uint8_t *p = (uint8_t *)s_drv_method_slot;
    for (size_t i = 0; i + 10 < 0x90; i++) {
        /* 41 83 bd 6c 04 00 00 00 74 xx */
        if (p[i] == 0x41 && p[i + 1] == 0x83 && p[i + 2] == 0xBD &&
            p[i + 3] == 0x6C && p[i + 4] == 0x04 && p[i + 5] == 0x00 &&
            p[i + 6] == 0x00 && p[i + 7] == 0x00 && p[i + 8] == 0x74) {
            uint8_t was = p[i + 9];
            uint8_t nops[2] = { 0x90, 0x90 };
            poke(p + i + 8, nops, 2);
            s_have_drv46c = 1;
            if (f)
                fprintf(f, "drv46c: NOP je @ drv+%zu (was 74 %02x)\n",
                        i + 8, was);
            return;
        }
    }
    if (f) fprintf(f, "drv46c: pattern not found in %p\n", s_drv_method_slot);
}

static int patch_jump_table_16(FILE *f) {
    if (!s_fn_validate) return 0;
    int ok = 0;

    ok += patch_height_05(f);
    ok += install_ioctl_spy(f);

    if (f) {
        fprintf(f, "commit: ioctl @+0x1325 spied; js@+0x132C INTACT (no fake)\n");
        fprintf(f, "commit: addbuf=%p regattr=%p\n", s_fn_addbuf, s_fn_regattr);
    }

    uint8_t *js = s_fn_validate + OFF_IOCTL_JS;
    if (f) {
        fprintf(f, "commit: js@+0x132C %02x %02x %02x %02x %02x %02x\n",
                js[0], js[1], js[2], js[3], js[4], js[5]);
    }
    hex_dump_fn(f, "CommitTail+0x1310", s_fn_validate + 0x1310, 64);

    s_have_ioctl_js = 0;
    s_have_jt = 0;
    (void)s_jt_ent16_site;
    (void)s_orig_jt_ent16;
    (void)s_orig_ioctl_js;
    return ok;
}

/* Capacity group only (cap ebx->0, NOP jl). bpp is patched temporarily
 * in RegisterBuffers YCbCr (lea*1 / mov rdi,rcx 8 bytes), not permanently here. */
static int patch_cap_only(FILE *f) {
    int ok = 0;
    if (!s_fn_validate) return 0;

    uint8_t *lea = s_fn_validate + OFF_LEA_BPP;
    if (f) {
        fprintf(f, "bpp: lea@+0x813 %02x %02x %02x %02x (temp bpp1 8B on YCbCr reg)\n",
                lea[0], lea[1], lea[2], lea[3]);
    }
    s_have_lea = 0;

    uint8_t *cap_ebx = s_fn_validate + OFF_CAP_EBX;
    if (cap_ebx[0] == 0xBB && cap_ebx[1] == 0x01 && cap_ebx[2] == 0x00 &&
        cap_ebx[3] == 0x29 && cap_ebx[4] == 0x80) {
        memcpy(s_orig_cap_ebx, cap_ebx, 5);
        s_cap_ebx_site = cap_ebx;
        static const uint8_t ebx0[5] = { 0xBB, 0x00, 0x00, 0x00, 0x00 };
        poke(cap_ebx, ebx0, 5);
        s_have_cap_ebx = 1;
        ok++;
        if (f) fprintf(f, "cap: mov ebx,01 -> 0 @ +0x99b\n");
    } else if (f) {
        fprintf(f, "cap: ebx@+0x99b mismatch %02x %02x %02x\n",
                cap_ebx[0], cap_ebx[1], cap_ebx[2]);
    }

    uint8_t *jl = s_fn_validate + OFF_CAP_JL;
    static const uint8_t nop6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    if (jl[0] == 0x0F && jl[1] == 0x8C) {
        memcpy(s_orig_capjl, jl, 6);
        s_capjl_site = jl;
        poke(jl, nop6, 6);
        s_have_capjl = 1;
        ok++;
        if (f) fprintf(f, "cap: NOP jl@+0x9A8\n");
    } else if (f) {
        fprintf(f, "cap: jl@+0x9A8 mismatch %02x %02x\n", jl[0], jl[1]);
    }

    return ok;
}

static int patch_format_gate(FILE *f) {
    if (!s_fn_validate) return 0;
    uint8_t *site = s_fn_validate + OFF_MOV_EBX_ERR;
    if (site[0] != 0xBB || site[1] != 0x03 || site[2] != 0x00 ||
        site[3] != 0x29 || site[4] != 0x80) {
        for (size_t i = 0x380; i + 5 < 0x500; i++) {
            if (s_fn_validate[i] == 0xBB && s_fn_validate[i + 1] == 0x03 &&
                s_fn_validate[i + 2] == 0x00 && s_fn_validate[i + 3] == 0x29 &&
                s_fn_validate[i + 4] == 0x80) {
                site = s_fn_validate + i;
                break;
            }
        }
        if (site[0] != 0xBB) {
            if (f) fprintf(f, "gate site not found\n");
            return 0;
        }
    }

    memcpy(s_orig_mov_ebx, site, 5);
    s_success_abs = s_fn_validate + OFF_SUCCESS;
    s_continue_abs = site + 5;

    intptr_t rel = (uint8_t *)&ycc_format_gate - (site + 5);
    if (rel < (intptr_t)INT32_MIN || rel > (intptr_t)INT32_MAX)
        return 0;
    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    int32_t r32 = (int32_t)rel;
    memcpy(patch + 1, &r32, 4);
    poke(site, patch, 5);

    if (f)
        fprintf(f, "gate: site=%p success=%p\n", (void *)site, (void *)s_success_abs);
    return 1;
}

static void apply_code_patches(FILE *f) {
    if (!s_fn_validate) return;
    if (s_patched) {
        /* Re-check: after Videodec2 the gate sometimes reverts to bb 03… */
        uint8_t *gate = s_fn_validate + OFF_MOV_EBX_ERR;
        if (gate[0] != 0xE9) {
            if (f) fprintf(f, "repatch: gate lost (%02x), re-applying\n", gate[0]);
            s_patched = 0;
            s_have_lea = s_have_jne3c = s_have_early01 = 0;
            s_have_ebx0a = s_have_cap_ebx = s_have_capjl = s_have_jt = 0;
            s_have_ioctl_js = 0;
            s_h05_n = 0;
        } else {
            return;
        }
    }

    /* Group M1: always (minimum to pass format validation). */
    int n20 = patch_bit_jz(s_fn_validate, 0x1000, BIT_YCC_EXT, f, "bit20");
    int n0 = patch_bit_jz(s_fn_validate, 0x1000, 0x01, f, "bit0");
    int gate = patch_format_gate(f);

    /* Groups gated by BISECT_LEVEL to isolate what breaks ioctl fill. */
    int e1 = 0, cap = 0, spy = 0;
#if BISECT_LEVEL >= 2
    e1 = patch_e1_sources(f);          /* M2: jne3c, early01, ebx0a */
#endif
#if BISECT_LEVEL >= 3
    cap = patch_cap_only(f);           /* M3: cap ebx->0, NOP jl */
#else
    if (f) fprintf(f, "bisect: cap SKIPPED (level %d)\n", BISECT_LEVEL);
#endif
#if BISECT_LEVEL >= 4
    spy = patch_jump_table_16(f);      /* M4: height05 + ioctl spy */
#else
    if (f) fprintf(f, "bisect: ioctl spy SKIPPED (level %d)\n", BISECT_LEVEL);
#endif
    s_patched = 1;

    if (f) {
        fprintf(f, "bisect_level=%d nops_bit20=%d nops_bit0=%d gate=%d "
                "e1=%d cap=%d spy=%d jne3c=%d early=%d ebx0a=%d "
                "capebx=%d capjl=%d ioctl_js=%d h05=%d\n",
                BISECT_LEVEL, n20, n0, gate, e1, cap, spy,
                s_have_jne3c, s_have_early01, s_have_ebx0a,
                s_have_cap_ebx, s_have_capjl, s_have_ioctl_js, s_h05_n);
    }
    klog("[ycbcr_unlock] 1.46 bisect=%d fix=%d gate=%d ioctl_spy=%d h05=%d\n",
         BISECT_LEVEL, FIX_MODE, gate, s_have_ioctl_spy, s_h05_n);
}

/* Ioctl spy WITHOUT validate patches — for golden BGRA dump. */
static void ensure_spy_only(void) {
    ensure_core_ready();
    if (s_have_ioctl_spy)
        return;
    resolve_validate();
    FILE *f = fopen(DIAG_PATH, "a");
    if (f)
        fprintf(f, "\n=== 1.46 spy-only (no validate patches) ===\n");
    install_ioctl_spy(f);
    if (f)
        fclose(f);
    notify("ycbcr spy-only");
}

static void apply_patches(int32_t handle) {
    resolve_validate();

    FILE *f = fopen(DIAG_PATH, "a");
    if (f) {
        fprintf(f, "\n=== 1.63 handle=%d bisect=%d fix=%d ===\n",
                (int)handle, BISECT_LEVEL, FIX_MODE);
        fprintf(f, "validate=%p patched=%d\n", (void *)s_fn_validate, s_patched);
    }

    or_safe_tags_and_flags(handle, f);
    apply_code_patches(f);

    if (f)
        fclose(f);

    notify("ycbcr 1.63");
}

static void write_diag_base(void) {
    FILE *f = fopen(DIAG_PATH, "w");
    if (!f) return;
    fprintf(f, "ver=0x%08x\n", g_pluginVersion);
    fprintf(f, "note=fix%d force ioctl addrs; UV port+AYUV probe (1.63)\n",
            FIX_MODE);
    fprintf(f, "regbufs=%p addbuf=%p regattr=%p\n",
            s_fn_regbufs, s_fn_addbuf, s_fn_regattr);
    fclose(f);
}

static void ensure_ycc_priv(int32_t handle) {
    sceVideoOutAddBufferYccPrivilege(handle);
    sceVideoOutSysUpdatePrivilege(handle);
}

static void clear_port_buffers(int32_t handle) {
    fn_unregattr_t pun = s_fn_unregattr ? (fn_unregattr_t)s_fn_unregattr : NULL;
    if (pun)
        pun(handle, 0);
    sceVideoOutUnregisterBuffers(handle, 0);
}

static int32_t __attribute__((unused))
try_addbuffer_path(int32_t handle, int32_t startIndex,
                                  void *const *addresses, int32_t bufferNum,
                                  const VoBufferAttribute *attribute, FILE *f) {
    if (!s_fn_regattr || !s_fn_addbuf || !attribute || bufferNum <= 0)
        return (int32_t)0x80020001;

    fn_regattr_t preg = (fn_regattr_t)s_fn_regattr;
    fn_addbuf_t padd = (fn_addbuf_t)s_fn_addbuf;

    clear_port_buffers(handle);

    int32_t rc = preg(handle, 0, attribute);
    if (f) fprintf(f, "addbuf: RegisterBufferAttribute => 0x%08x\n", (unsigned)rc);
    if (rc != 0)
        return rc;

    for (int32_t i = 0; i < bufferNum; i++) {
        rc = padd(handle, 0, startIndex + i, addresses[i]);
        if (f) fprintf(f, "addbuf: AddBuffer[%d] => 0x%08x\n", (int)i, (unsigned)rc);
        if (rc != 0) {
            clear_port_buffers(handle);
            return rc;
        }
    }
    if (f) fprintf(f, "addbuf: OK n=%d\n", (int)bufferNum);
    return 0;
}

static int32_t call_regbufs(int32_t handle, int32_t startIndex,
                            void *const *addresses, int32_t bufferNum,
                            const VoBufferAttribute *attribute) {
    return HOOK_CONTINUE(sceVideoOutRegisterBuffers,
                         int32_t (*)(int32_t, int32_t, void *const *, int32_t,
                                     const VoBufferAttribute *),
                         handle, startIndex, addresses, bufferNum, attribute);
}

/* 1.51–1.54: bpp1 did not remove 4-tiles. Permanent + hrep4 → neon (UV@w*h).
 * 1.58: force lea rdi,[rcx*4] after register so DCE UV uses stride×4. */
static const uint8_t k_lea_x4[8] = {
    0x48, 0x8D, 0x3C, 0x8D, 0x00, 0x00, 0x00, 0x00
};

static void bpp_force_x4(FILE *f) {
    if (!s_fn_validate)
        return;
    uint8_t *lea = s_fn_validate + OFF_LEA_BPP;
    if (lea[0] == 0x48 && lea[1] == 0x8D && lea[2] == 0x3C && lea[3] == 0x8D) {
        if (f)
            fprintf(f, "bpp: lea*[4] already @+0x813\n");
        s_have_lea = 0;
        return;
    }
    if (s_orig_lea[0] == 0x48 && s_orig_lea[1] == 0x8D)
        poke(lea, s_orig_lea, 8);
    else
        poke(lea, k_lea_x4, 8);
    s_have_lea = 0;
    if (f)
        fprintf(f, "bpp: restored lea*[4] @+0x813 (1.58)\n");
}

static void bpp_ycc_restore(FILE *f) {
    bpp_force_x4(f);
}

int32_t sceVideoOutRegisterBuffers_hook(int32_t handle, int32_t startIndex,
                                        void *const *addresses, int32_t bufferNum,
                                        const VoBufferAttribute *attribute) {
    uint32_t fmt = 0;
    if (attribute)
        memcpy(&fmt, attribute, 4);

    if (fmt != YCBCR420_BT709) {
        /* BGRA or others: restore bpp in case it was left permanent. */
        bpp_ycc_restore(NULL);
        /* Baseline BGRA without patches (app calls it before YCbCr) or
         * post-patch diag. No fixup: we want the userspace payload intact. */
        int want_dump = 0;
        if (!s_patched) {
            ensure_spy_only();
            want_dump = s_have_ioctl_spy;
        } else if (s_core_ready && s_have_ioctl_spy) {
            want_dump = 1;
        }
        if (want_dump) {
            FILE *fb = fopen(DIAG_PATH, "a");
            s_ioctl_ret = (int32_t)0xffffffff;
            s_ioctl_arg_ok = 0;
            s_ioctl_called = 0;
            s_ioctl_arg_filled = 0;
            s_fixup_done = 0;
            /* Do not inject ptrs: golden dump / post-poison without mutating. */
            s_hook_addresses = NULL;
            s_hook_bufcount = 0;
            int32_t rcb = HOOK_CONTINUE(sceVideoOutRegisterBuffers,
                                        int32_t (*)(int32_t, int32_t, void *const *, int32_t,
                                                    const VoBufferAttribute *),
                                        handle, startIndex, addresses, bufferNum, attribute);
            if (fb) {
                fprintf(fb, "%s: fmt=0x%08x tile=%d %ux%u pitch=%u n=%d patched=%d\n",
                        s_patched ? "attr_other" : "attr_baseline",
                        (unsigned)fmt,
                        attribute ? attribute->tmode : -1,
                        attribute ? attribute->width : 0,
                        attribute ? attribute->height : 0,
                        attribute ? attribute->pixelPitch : 0,
                        (int)bufferNum, s_patched);
                for (int32_t i = 0; i < bufferNum && i < 4; i++)
                    fprintf(fb, "  hook_addr[%d]=0x%llx\n", (int)i,
                            (unsigned long long)(uintptr_t)(addresses ? addresses[i] : NULL));
                fprintf(fb, "rc=0x%08x via=RegisterBuffers (non-YCbCr)\n", (unsigned)rcb);
                capture_ioctl_arg(fb);
                fclose(fb);
            }
            return rcb;
        }
        return HOOK_CONTINUE(sceVideoOutRegisterBuffers,
                             int32_t (*)(int32_t, int32_t, void *const *, int32_t,
                                         const VoBufferAttribute *),
                             handle, startIndex, addresses, bufferNum, attribute);
    }

    ensure_core_ready();
    ensure_ycc_priv(handle);
    install_call_spy();
    apply_patches(handle);

    void *inner = port_inner(handle);
    if (inner) {
        if (load_u32(inner, PORT_OFF_4A8) == 0)
            store_u32(inner, PORT_OFF_4A8, 1);
        if (load_u32(inner, PORT_OFF_46C) == 0)
            store_u32(inner, PORT_OFF_46C, 1);
        if (load_u32(inner, PORT_OFF_478) == 0)
            store_u32(inner, PORT_OFF_478, 1);
        if (load_u32(inner, PORT_OFF_480) == 0)
            store_u32(inner, PORT_OFF_480, 1);
    }
    /* Re-push privilege after port flags. */
    sceVideoOutSysUpdatePrivilege(handle);

    FILE *f = fopen(DIAG_PATH, "a");
    if (f && attribute) {
        fprintf(f, "attr: fmt=0x%08x tile=%d %ux%u pitch=%u n=%d bisect=%d\n",
                (unsigned)fmt, attribute->tmode,
                attribute->width, attribute->height,
                attribute->pixelPitch, (int)bufferNum, BISECT_LEVEL);
        /* Full FB addresses the hook receives: must appear inside the
         * ioctl struct if fill works. */
        for (int32_t i = 0; i < bufferNum && i < 4; i++)
            fprintf(f, "  hook_addr[%d]=0x%llx\n", (int)i,
                    (unsigned long long)(uintptr_t)(addresses ? addresses[i] : NULL));
    }

    s_ioctl_ret = (int32_t)0xffffffff;
    s_ioctl_arg_ok = 0;
    s_ioctl_called = 0;
    s_ioctl_arg_filled = 0;
    s_fixup_done = 0;
    s_hook_addresses = addresses;
    s_hook_bufcount = bufferNum;

    int32_t rc;
    const char *via = "RegisterBuffers";

    /* 1.58: no bpp1; force lea*[4] for UV stride coherent with hrep4. */
    clear_port_buffers(handle);
    s_ioctl_called = 0;
    s_fixup_done = 0;
    bpp_force_x4(f);
    rc = call_regbufs(handle, startIndex, addresses, bufferNum, attribute);
    bpp_force_x4(f);
    if (f) {
        fprintf(f, "bulk: n=%d => 0x%08x ioctl_called=%d\n",
                (int)bufferNum, (unsigned)rc, s_ioctl_called);
        if (s_ioctl_called)
            capture_ioctl_arg(f);
    }
    if (rc == 0) {
        via = "RegisterBuffers";
    } else if (bufferNum == 1) {
        via = "RegisterBuffers";
    } else {
        clear_port_buffers(handle);
        if (f)
            fprintf(f, "bulk failed; skip n1×N (use app n=1); last err=0x%08x\n",
                    (unsigned)rc);
        via = "RegisterBuffers";
    }

    /* Only trust ioctl if it was actually called (1.36 falsely set ret=0). */
    if (s_have_ioctl_spy && s_ioctl_called && s_ioctl_ret == 0 && rc != 0) {
        if (f)
            fprintf(f, "trust: ioctl=0 override userspace rc=0x%08x -> 0\n",
                    (unsigned)rc);
        klog("[ycbcr_unlock] trust ioctl=0 (was rc=0x%08x)\n", (unsigned)rc);
        rc = 0;
    }

    if (rc == 0) {
        or_safe_tags_and_flags(handle, f);
        force_slot_marks(handle, startIndex, bufferNum, f);
        scan_port_stride(handle, f);
        if (attribute) {
            dump_port_uv_candidates(handle, attribute->width, attribute->height, f);
            patch_port_uv_offset(handle, attribute->width, attribute->height, f);
            dump_port_uv_candidates(handle, attribute->width, attribute->height, f);
        }
        sceVideoOutSysUpdatePrivilege(handle);
    }

    klog("[ycbcr_unlock] YCbCr %s n=%d => 0x%08x\n",
         via, (int)bufferNum, (unsigned)rc);
    if (f) {
        fprintf(f, "rc=0x%08x via=%s n=%d %ux%u pitch=%u tile=%d\n",
                (unsigned)rc, via, (int)bufferNum,
                attribute ? attribute->width : 0,
                attribute ? attribute->height : 0,
                attribute ? attribute->pixelPitch : 0,
                attribute ? attribute->tmode : -1);
        fprintf(f, "fix_mode=%d fixup_done=%d\n",
                FIX_MODE, s_fixup_done);
        capture_ioctl_arg(f);
        if (s_drv_method_slot) {
            fprintf(f, "drv_method=%p\n", s_drv_method_slot);
            hex_dump_fn(f, "drv_method", (const uint8_t *)s_drv_method_slot, 128);
#if BISECT_LEVEL >= 5
            patch_drv_method_46c(f);   /* M5: NOP je in drv_method+0x46c */
#else
            fprintf(f, "bisect: drv46c SKIPPED (level %d)\n", BISECT_LEVEL);
#endif
        }
        fclose(f);
    }
    return rc;
}

int32_t sceVideoOutSubmitFlip_hook(int32_t handle, int32_t bufferIndex,
                                   uint32_t flipMode, int64_t flipArg) {
    or_safe_tags_and_flags(handle, NULL);
    if (bufferIndex >= 0)
        force_slot_marks(handle, bufferIndex, 1, NULL);
    int32_t rc = HOOK_CONTINUE(sceVideoOutSubmitFlip,
                               int32_t (*)(int32_t, int32_t, uint32_t, int64_t),
                               handle, bufferIndex, flipMode, flipArg);
    static int s_flip_logs;
    if (s_flip_logs < 8) {
        s_flip_logs++;
        klog("[ycbcr_unlock] SubmitFlip idx=%d mode=%u => 0x%08x\n",
             (int)bufferIndex, (unsigned)flipMode, (unsigned)rc);
    }
    return rc;
}

static void ensure_moonlight_dir(void) {
    sceKernelMkdir("/data", 0777);
    sceKernelMkdir("/data/moonlight", 0777);
}

static void ensure_core_ready(void) {
    if (s_core_ready)
        return;
    s_core_ready = 1;

    ensure_moonlight_dir();

    memset(&s_jb, 0, sizeof(s_jb));
    int jb = sys_sdk_jailbreak(&s_jb);
    s_jb_ok = (jb == 0);
    if (resolve_videoout() != 0)
        snprintf(s_marker_extra, sizeof(s_marker_extra), "vo=missing");
    else
        snprintf(s_marker_extra, sizeof(s_marker_extra), "vo=ok real38");

    write_diag_base();
    write_marker(jb);
    klog("[ycbcr_unlock] core ready jb=%d %s\n", jb, s_marker_extra);
}

static void write_marker(int jb_rc) {
    /* Prefer kernel API: fopen/stdio in plugin_load → SIGBUS (klog FW9). */
    char line[192];
    int n = snprintf(line, sizeof(line), "ver=0x%08x jb=%d %s\n",
                     g_pluginVersion, jb_rc, s_marker_extra);
    if (n <= 0) return;
    /* FreeBSD/Orbis: O_WRONLY|O_CREAT|O_TRUNC = 0x601 (seen in the crash). */
    int fd = sceKernelOpen(MARKER_PATH, 0x601, 0666);
    if (fd < 0) return;
    sceKernelWrite(fd, line, (size_t)n);
    sceKernelClose(fd);
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    if (s_plugin_started) return 0;
    s_plugin_started = 1;

    /* CRITICAL (klog SIGBUS 0x601): GoldHEN calls module_start BEFORE main().
     * No jailbreak/fopen/stdio here. Marker via sceKernelOpen (safe) so
     * Moonlight sees the plugin BEFORE the first RegisterBuffers. */
    ensure_moonlight_dir();
    snprintf(s_marker_extra, sizeof(s_marker_extra), "hook-only");
    write_marker(-1);
    klog("[ycbcr_unlock] 1.63 load hook RegisterBuffers+SubmitFlip\n");
    HOOK32(sceVideoOutRegisterBuffers);
    HOOK32(sceVideoOutSubmitFlip);
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    if (!s_plugin_started) return 0;
    uninstall_call_spy();
    if (s_patched && s_fn_validate) {
        poke(s_fn_validate + OFF_MOV_EBX_ERR, s_orig_mov_ebx, 5);
        if (s_have_lea)
            poke(s_fn_validate + OFF_LEA_BPP, s_orig_lea, 8);
        if (s_have_cap_ebx && s_cap_ebx_site)
            poke(s_cap_ebx_site, s_orig_cap_ebx, 5);
        if (s_have_capjl && s_capjl_site)
            poke(s_capjl_site, s_orig_capjl, 6);
        if (s_have_jne3c)
            poke(s_fn_validate + OFF_FMT_JNE_3C, s_orig_jne3c, 6);
        if (s_have_early01)
            poke(s_fn_validate + OFF_EARLY_01, s_orig_early01, 10);
        if (s_have_ebx0a)
            poke(s_fn_validate + OFF_EBX_0A, s_orig_ebx0a, 5);
        if (s_have_ioctl_js)
            poke(s_fn_validate + OFF_IOCTL_JS, s_orig_ioctl_js, 6);
        if (s_have_ioctl_spy && s_ioctl_call_site)
            poke(s_ioctl_call_site, s_ioctl_orig_bytes, 5);
        if (s_have_jt && s_jt_ent16_site)
            poke(s_jt_ent16_site, &s_orig_jt_ent16, 4);
        for (int i = 0; i < s_h05_n; i++)
            poke(s_h05[i].site, s_h05[i].orig, (size_t)s_h05[i].len);
    }
    UNHOOK(sceVideoOutSubmitFlip);
    UNHOOK(sceVideoOutRegisterBuffers);
    if (s_jb_ok) sys_sdk_unjailbreak(&s_jb);
    s_plugin_started = 0;
    s_core_ready = 0;
    return 0;
}

int32_t attr_module_hidden module_start(int64_t argc, const void *args) {
    (void)argc; (void)args;
    return plugin_load(0, NULL);
}

int32_t attr_module_hidden module_stop(int64_t argc, const void *args) {
    (void)argc; (void)args;
    return 0;
}
