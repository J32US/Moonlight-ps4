// mbedTLS tweaks for PS4/OpenOrbis (applied after mbedtls_config.h
// via MBEDTLS_USER_CONFIG_FILE).
#pragma once

// No usable platform entropy (getrandom stub ENOSYS; libSceRandom does
// not resolve in homebrew). Use mbedtls_hardware_poll in ps4_entropy.c.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
