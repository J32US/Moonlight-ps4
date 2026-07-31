// Persistent client configuration (simple INI).
#pragma once

#include <stdbool.h>
#include <Limelight.h>

#define CONFIG_DIR_PS4 "/data/moonlight"
#define CONFIG_MAX_HOST 128
#define CONFIG_MAX_APP  128

typedef struct {
    STREAM_CONFIGURATION stream; // embedded: width/height/fps/bitrate/...

    char host[CONFIG_MAX_HOST];
    char app_name[CONFIG_MAX_APP]; // name or numeric id
    char debug_host[64];

    bool sops;
    bool local_audio;
    bool prefer_hw;
    bool videodec2_spike;
    bool prefer_ycbcr;
    bool enable_file_log; // writes /data/moonlight/debug.log
    bool paired_ok; // runtime only
} app_config_t;

void config_set_defaults(app_config_t *cfg);
void config_ensure_dir(const char *dir);
int config_load(app_config_t *cfg, const char *dir);
int config_save(const app_config_t *cfg, const char *dir);
