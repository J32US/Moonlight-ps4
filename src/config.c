#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

void config_set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    LiInitializeStreamConfiguration(&cfg->stream);
    cfg->stream.width = 1920;
    cfg->stream.height = 1080;
    cfg->stream.fps = 60;
    cfg->stream.bitrate = 20000;
    cfg->stream.packetSize = 1024;
    cfg->stream.streamingRemotely = STREAM_CFG_LOCAL;
    cfg->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg->stream.supportedVideoFormats = VIDEO_FORMAT_H264;
    cfg->stream.encryptionFlags = ENCFLG_NONE;
    cfg->stream.colorSpace = COLORSPACE_REC_709;
    cfg->stream.colorRange = COLOR_RANGE_LIMITED;
    cfg->stream.clientRefreshRateX100 = 5994;
    cfg->sops = true;
    cfg->local_audio = false;
    cfg->prefer_hw = true;
    cfg->videodec2_spike = false;
    cfg->prefer_ycbcr = false;
    cfg->enable_file_log = false;
    snprintf(cfg->app_name, sizeof(cfg->app_name), "Steam Big Picture");
}

void config_ensure_dir(const char *dir) {
#ifdef __ORBIS__
    // Create basic parent dirs if missing (/data).
    sceKernelMkdir("/data", 0777);
    sceKernelMkdir(dir, 0777);
#else
    mkdir(dir, 0755);
#endif
    (void)errno;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int parse_bool(const char *v) {
    return !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes");
}

int config_load(app_config_t *cfg, const char *dir) {
    char path[512];
    char line[512];

    config_set_defaults(cfg);
    LOGI("config_load: dir=%s", dir);

    // Compat: host.txt / debug_host.txt from phases 0-2
    snprintf(path, sizeof(path), "%s/host.txt", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(cfg->host, sizeof(cfg->host), f))
            cfg->host[strcspn(cfg->host, " \r\n")] = '\0';
        fclose(f);
        LOGI("config: host.txt => '%s'", cfg->host);
    }
    snprintf(path, sizeof(path), "%s/debug_host.txt", dir);
    f = fopen(path, "r");
    if (f) {
        if (fgets(cfg->debug_host, sizeof(cfg->debug_host), f))
            cfg->debug_host[strcspn(cfg->debug_host, " \r\n")] = '\0';
        fclose(f);
        LOGI("config: debug_host.txt => '%s'", cfg->debug_host);
    }

    snprintf(path, sizeof(path), "%s/moonlight.ini", dir);
    f = fopen(path, "r");
    if (!f) {
        LOGW("config: no hay %s", path);
        return cfg->host[0] ? 0 : -1;
    }
    LOGI("config: leyendo %s", path);

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (!strcmp(key, "host"))
            snprintf(cfg->host, sizeof(cfg->host), "%s", val);
        else if (!strcmp(key, "app"))
            snprintf(cfg->app_name, sizeof(cfg->app_name), "%s", val);
        else if (!strcmp(key, "debug_host"))
            snprintf(cfg->debug_host, sizeof(cfg->debug_host), "%s", val);
        else if (!strcmp(key, "width"))
            cfg->stream.width = atoi(val);
        else if (!strcmp(key, "height"))
            cfg->stream.height = atoi(val);
        else if (!strcmp(key, "fps"))
            cfg->stream.fps = atoi(val);
        else if (!strcmp(key, "bitrate"))
            cfg->stream.bitrate = atoi(val);
        else if (!strcmp(key, "sops"))
            cfg->sops = parse_bool(val);
        else if (!strcmp(key, "local_audio"))
            cfg->local_audio = parse_bool(val);
        else if (!strcmp(key, "prefer_hw"))
            cfg->prefer_hw = parse_bool(val);
        else if (!strcmp(key, "videodec2_spike"))
            cfg->videodec2_spike = parse_bool(val);
        else if (!strcmp(key, "prefer_ycbcr"))
            cfg->prefer_ycbcr = parse_bool(val);
        else if (!strcmp(key, "enable_file_log"))
            cfg->enable_file_log = parse_bool(val);
        else
            LOGW("config: clave desconocida '%s'", key);
    }
    fclose(f);
    LOGI("config: host=%s app=%s debug=%s %dx%d@%d br=%d hw=%d ycbcr=%d file_log=%d",
         cfg->host, cfg->app_name, cfg->debug_host,
         cfg->stream.width, cfg->stream.height, cfg->stream.fps, cfg->stream.bitrate,
         cfg->prefer_hw, cfg->prefer_ycbcr, cfg->enable_file_log);
    return 0;
}

int config_save(const app_config_t *cfg, const char *dir) {
    char path[512];
    config_ensure_dir(dir);
    snprintf(path, sizeof(path), "%s/moonlight.ini", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOGE("config: could not write %s", path);
        return -1;
    }
    fprintf(f,
            "# moonlight-ps4\n"
            "host = %s\n"
            "app = %s\n"
            "debug_host = %s\n"
            "width = %d\n"
            "height = %d\n"
            "fps = %d\n"
            "bitrate = %d\n"
            "sops = %s\n"
            "local_audio = %s\n"
            "prefer_hw = %s\n"
            "videodec2_spike = %s\n"
            "prefer_ycbcr = %s\n"
            "enable_file_log = %s\n",
            cfg->host, cfg->app_name, cfg->debug_host,
            cfg->stream.width, cfg->stream.height, cfg->stream.fps, cfg->stream.bitrate,
            cfg->sops ? "true" : "false",
            cfg->local_audio ? "true" : "false",
            cfg->prefer_hw ? "true" : "false",
            cfg->videodec2_spike ? "true" : "false",
            cfg->prefer_ycbcr ? "true" : "false",
            cfg->enable_file_log ? "true" : "false");
    fclose(f);
    return 0;
}
