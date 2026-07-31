// Entropy source for mbedTLS on PS4.
// libSceRandom is not loaded automatically in homebrew and causes
// PRX_NOT_RESOLVED_FUNCTION; musl getrandom/getentropy are stubbed
// (ENOSYS). Mix clock, TSC, and the kernel sandbox random word.

#include <orbis/libkernel.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void mix_u64(uint64_t *state, uint64_t v) {
    *state ^= v;
    (void)splitmix64(state);
}

static void mix_bytes(uint64_t *state, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        *state ^= (uint64_t)b[i] << ((i & 7u) * 8u);
        if ((i & 7u) == 7u)
            (void)splitmix64(state);
    }
    (void)splitmix64(state);
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    if (!output || !olen || len == 0)
        return -1;

    uint64_t state = sceKernelReadTsc();
    mix_u64(&state, sceKernelGetProcessTime());
    mix_u64(&state, sceKernelGetProcessTimeCounter());
    mix_u64(&state, (uint64_t)(uintptr_t)&state);
    mix_u64(&state, (uint64_t)(uintptr_t)output);
    mix_u64(&state, (uint64_t)len);

    OrbisKernelTimeval tv;
    if (sceKernelGettimeofday(&tv) == 0)
        mix_u64(&state, ((uint64_t)tv.tv_sec << 32) ^ (uint64_t)tv.tv_usec);

    const char *sandbox = sceKernelGetFsSandboxRandomWord();
    if (sandbox)
        mix_bytes(&state, sandbox, strlen(sandbox));

    // Timing jitter: TSC varies between reads.
    for (int i = 0; i < 96; i++) {
        uint64_t a = sceKernelReadTsc();
        volatile int sink = 0;
        for (int j = 0; j < (32 + (i & 15)); j++)
            sink += j * (i + 1);
        uint64_t b = sceKernelReadTsc();
        mix_u64(&state, a ^ (b << 1) ^ (uint64_t)(unsigned)sink);
    }

    size_t off = 0;
    while (off < len) {
        uint64_t r = splitmix64(&state);
        size_t n = len - off;
        if (n > sizeof(r))
            n = sizeof(r);
        memcpy(output + off, &r, n);
        off += n;
    }

    *olen = len;
    return 0;
}
