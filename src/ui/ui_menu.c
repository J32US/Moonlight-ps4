#include "ui_menu.h"
#include "ui_draw.h"
#include "../log.h"
#include "../input/input_pad.h"
#include "../video/video.h"
#include "../gamestream/gs_errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef __ORBIS__
#include <orbis/SystemService.h>
#endif

#define UI_W 1920
#define UI_H 1080
#define UI_MAX_APPS 64

/* Colors 0xAARRGGBB (BGRA LE). */
#define COL_BG      0xFF0E1218u
#define COL_PANEL   0xFF1A222Cu
#define COL_SEL     0xFF24405Au
#define COL_ACCENT  0xFF3FA7FFu
#define COL_TEXT    0xFFE8ECF0u
#define COL_DIM     0xFF8794A2u
#define COL_WARN    0xFFFFB03Fu

static int s_splash_hidden;

static void ui_hide_splash_once(void) {
#ifdef __ORBIS__
    if (s_splash_hidden)
        return;
    (void)sceSystemServiceHideSplashScreen();
    s_splash_hidden = 1;
#endif
}

void ui_show_status(const char *title, const char *line1, const char *line2) {
    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("ui_show_status: video_ui_begin failed");
        ui_hide_splash_once(); /* better than eternal splash */
        return;
    }

    size_t pitch = (size_t)UI_W * 4u;
    size_t bytes = pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(bytes);
    if (!staging) {
        ui_hide_splash_once();
        return;
    }

    ui_surface_t s = { staging, (int)pitch, UI_W, UI_H };
    ui_clear(&s, COL_BG);
    ui_text(&s, 80, 80, 5, COL_TEXT, title ? title : "MOONLIGHT PS4");
    if (line1 && line1[0])
        ui_text(&s, 80, 220, 4, COL_ACCENT, line1);
    if (line2 && line2[0])
        ui_text(&s, 80, 320, 3, COL_DIM, line2);
    ui_text(&s, 80, UI_H - 60, 2, COL_DIM, "Please wait...");

    for (int i = 0; i < 4; i++) {
        (void)video_ui_present(staging, (int)pitch);
        usleep(16000);
    }
    ui_hide_splash_once();
    free(staging);
    LOGI("ui_show_status: '%s' | '%s' | '%s'",
         title ? title : "", line1 ? line1 : "", line2 ? line2 : "");
}

typedef enum {
    TAB_APPS = 0,
    TAB_SETTINGS = 1,
} ui_tab_t;

enum {
    SET_HOST = 0,
    SET_DEBUG_HOST,
    SET_RES,
    SET_FPS,
    SET_BITRATE,
    SET_SOPS,
    SET_LOCAL_AUDIO,
    SET_PREFER_HW,
    SET_PREFER_YCBCR,
    SET_CODEC,
    SET_FILE_LOG,
    SET_SHOW_STATS,
    SET_COUNT,
};

static const char *k_set_names[SET_COUNT] = {
    "Host",
    "Debug host",
    "Resolution",
    "FPS",
    "Bitrate (kbps)",
    "SOPS",
    "Local audio",
    "Decoder HW",
    "YCbCr (experimental)",
    "Codec",
    "File logging",
    "Perf overlay",
};

/* On-screen keyboard 4x10. */
#define OSK_COLS 10
#define OSK_ROWS 4
static const char k_osk_chars[OSK_ROWS][OSK_COLS + 1] = {
    "0123456789",
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ.-:_",
};

typedef struct {
    char name[CONFIG_MAX_APP];
    int id;
} ui_app_t;

typedef struct {
    app_config_t *cfg;
    gs_server_t *server;
    const char *config_dir;

    ui_app_t apps[UI_MAX_APPS];
    int napps;
    int apps_err; /* 0 ok, 1 applist failed, 2 no connection */

    ui_tab_t tab;
    int sel_app;
    int app_first; /* scroll */
    int sel_set;

    int osk_active;
    int osk_target; /* SET_HOST / SET_DEBUG_HOST */
    int osk_row, osk_col;
    char osk_buf[CONFIG_MAX_HOST];

    int host_dirty; /* host changed: reconnect required */
    int result;     /* -1000 = stay in loop */
} ui_state_t;

static void fetch_apps(ui_state_t *st) {
    st->napps = 0;
    if (!st->server) {
        st->apps_err = 2;
        return;
    }
    (void)gs_refresh_status(st->server);
    app_entry_t *list = NULL;
    if (gs_applist(st->server, &list) != GS_OK) {
        LOGW("ui: gs_applist failed: %s", gs_error ? gs_error : "?");
        st->apps_err = 1;
        return;
    }
    for (app_entry_t *a = list; a && st->napps < UI_MAX_APPS; a = a->next) {
        snprintf(st->apps[st->napps].name, sizeof(st->apps[st->napps].name),
                 "%s", a->name ? a->name : "?");
        st->apps[st->napps].id = a->id;
        st->napps++;
    }
    xml_applist_free(list);
    st->apps_err = 0;
    LOGI("ui: applist %d apps currentGame=%d", st->napps,
         st->server->currentGame);
}

static const char *active_app_name(const ui_state_t *st) {
    if (!st->server || st->server->currentGame == 0)
        return NULL;
    for (int i = 0; i < st->napps; i++) {
        if (st->apps[i].id == st->server->currentGame)
            return st->apps[i].name;
    }
    return NULL;
}

static void quit_active_stream(ui_state_t *st) {
    if (!st->server || st->server->currentGame == 0)
        return;
    int id = st->server->currentGame;
    const char *name = active_app_name(st);
    LOGI("ui: closing active stream id=%d name='%s'", id, name ? name : "?");
    if (gs_quit_app(st->server) != GS_OK) {
        LOGW("ui: gs_quit_app failed: %s", gs_error ? gs_error : "?");
        (void)gs_refresh_status(st->server);
    } else {
        LOGI("ui: stream closed");
        LOGN("Stream closed");
    }
}

static void preselect_app(ui_state_t *st) {
    st->sel_app = 0;
    for (int i = 0; i < st->napps; i++) {
        if (!strcasecmp(st->apps[i].name, st->cfg->app_name)) {
            st->sel_app = i;
            break;
        }
    }
}

/* --- setting values as text --- */

static void set_value_str(const ui_state_t *st, int row, char *out, size_t cap) {
    const app_config_t *c = st->cfg;
    switch (row) {
    case SET_HOST:        snprintf(out, cap, "%s", c->host); break;
    case SET_DEBUG_HOST:  snprintf(out, cap, "%s", c->debug_host); break;
    case SET_RES:         snprintf(out, cap, "%dx%d", c->stream.width, c->stream.height); break;
    case SET_FPS:         snprintf(out, cap, "%d", c->stream.fps); break;
    case SET_BITRATE:     snprintf(out, cap, "%d", c->stream.bitrate); break;
    case SET_SOPS:        snprintf(out, cap, "%s", c->sops ? "yes" : "no"); break;
    case SET_LOCAL_AUDIO: snprintf(out, cap, "%s", c->local_audio ? "yes" : "no"); break;
    case SET_PREFER_HW:   snprintf(out, cap, "%s", c->prefer_hw ? "yes" : "no"); break;
    case SET_PREFER_YCBCR:snprintf(out, cap, "%s", c->prefer_ycbcr ? "yes" : "no"); break;
    case SET_CODEC:       snprintf(out, cap, "%s",
                                   c->codec == CODEC_HEVC ? "hevc" :
                                   (c->codec == CODEC_AUTO ? "auto" : "h264")); break;
    case SET_FILE_LOG:    snprintf(out, cap, "%s", c->enable_file_log ? "yes" : "no"); break;
    case SET_SHOW_STATS:  snprintf(out, cap, "%s", c->show_stats ? "yes" : "no"); break;
    default: out[0] = '\0'; break;
    }
}

static void cycle_res(app_config_t *c, int dir) {
    static const int presets[][2] = { {1280, 720}, {1920, 1080} };
    const int n = 2;
    int cur = 1;
    for (int i = 0; i < n; i++) {
        if (c->stream.width == presets[i][0] && c->stream.height == presets[i][1]) {
            cur = i;
            break;
        }
    }
    cur = (cur + dir + n) % n;
    c->stream.width = presets[cur][0];
    c->stream.height = presets[cur][1];
}

static void save_cfg(ui_state_t *st) {
    if (config_save(st->cfg, st->config_dir) != 0)
        LOGW("ui: config_save failed");
}

/* --- OSK --- */

static void osk_open(ui_state_t *st, int target) {
    st->osk_active = 1;
    st->osk_target = target;
    st->osk_row = 0;
    st->osk_col = 0;
    const char *cur = (target == SET_HOST) ? st->cfg->host : st->cfg->debug_host;
    snprintf(st->osk_buf, sizeof(st->osk_buf), "%s", cur);
}

static void osk_accept(ui_state_t *st) {
    if (st->osk_target == SET_HOST) {
        if (strcmp(st->cfg->host, st->osk_buf) != 0) {
            snprintf(st->cfg->host, sizeof(st->cfg->host), "%s", st->osk_buf);
            st->host_dirty = 1;
        }
        save_cfg(st);
        st->osk_active = 0;
        /* After writing host: reconnect when leaving the keyboard. */
        if (st->cfg->host[0] && st->host_dirty)
            st->result = UI_MENU_RECONNECT;
        return;
    }
    snprintf(st->cfg->debug_host, sizeof(st->cfg->debug_host), "%s", st->osk_buf);
    save_cfg(st);
    st->osk_active = 0;
}

static void osk_input(ui_state_t *st, unsigned pr) {
    if (pr & MENU_BTN_UP)    st->osk_row = (st->osk_row + OSK_ROWS - 1) % OSK_ROWS;
    if (pr & MENU_BTN_DOWN)  st->osk_row = (st->osk_row + 1) % OSK_ROWS;
    if (pr & MENU_BTN_LEFT)  st->osk_col = (st->osk_col + OSK_COLS - 1) % OSK_COLS;
    if (pr & MENU_BTN_RIGHT) st->osk_col = (st->osk_col + 1) % OSK_COLS;
    if (pr & MENU_BTN_CROSS) {
        size_t len = strlen(st->osk_buf);
        if (len + 1 < sizeof(st->osk_buf)) {
            st->osk_buf[len] = k_osk_chars[st->osk_row][st->osk_col];
            st->osk_buf[len + 1] = '\0';
        }
    }
    if (pr & MENU_BTN_SQUARE) {
        size_t len = strlen(st->osk_buf);
        if (len)
            st->osk_buf[len - 1] = '\0';
    }
    if (pr & MENU_BTN_CIRCLE)
        st->osk_active = 0;
    if (pr & MENU_BTN_OPTIONS)
        osk_accept(st);
}

/* --- tab input --- */

static void launch_selected(ui_state_t *st) {
    if (st->napps > 0) {
        snprintf(st->cfg->app_name, sizeof(st->cfg->app_name), "%s",
                 st->apps[st->sel_app].name);
    }
    save_cfg(st);
    LOGI("ui: launch app='%s' (sel=%d host_dirty=%d)",
         st->cfg->app_name, st->sel_app, st->host_dirty);
    st->result = st->host_dirty ? UI_MENU_RECONNECT : UI_MENU_LAUNCH;
}

static void apps_input(ui_state_t *st, unsigned pr) {
    if ((pr & MENU_BTN_UP) && st->napps > 0)
        st->sel_app = (st->sel_app + st->napps - 1) % st->napps;
    if ((pr & MENU_BTN_DOWN) && st->napps > 0)
        st->sel_app = (st->sel_app + 1) % st->napps;
    if (pr & (MENU_BTN_L1 | MENU_BTN_R1))
        st->tab = TAB_SETTINGS;
    if (pr & MENU_BTN_TRIANGLE) {
        if (st->host_dirty) {
            save_cfg(st);
            st->result = UI_MENU_RECONNECT;
        } else {
            fetch_apps(st);
            preselect_app(st);
        }
    }
    /* X / O: start. OPTIONS: close active stream on Sunshine. */
    if (pr & MENU_BTN_OPTIONS) {
        if (st->server && st->server->currentGame != 0)
            quit_active_stream(st);
    }
    if (pr & (MENU_BTN_CROSS | MENU_BTN_CIRCLE)) {
        if (st->napps > 0)
            launch_selected(st);
        else if (st->apps_err)
            st->result = UI_MENU_RECONNECT;
    }
}

static void settings_input(ui_state_t *st, unsigned pr) {
    app_config_t *c = st->cfg;
    if (pr & MENU_BTN_UP)
        st->sel_set = (st->sel_set + SET_COUNT - 1) % SET_COUNT;
    if (pr & MENU_BTN_DOWN)
        st->sel_set = (st->sel_set + 1) % SET_COUNT;
    if (pr & (MENU_BTN_L1 | MENU_BTN_R1 | MENU_BTN_CIRCLE)) {
        st->tab = TAB_APPS;
        return;
    }

    int dir = 0;
    if (pr & MENU_BTN_LEFT) dir = -1;
    if (pr & MENU_BTN_RIGHT) dir = 1;
    int activate = (pr & MENU_BTN_CROSS) ? 1 : 0;
    if (!dir && !activate)
        return;

    int changed = 1;
    switch (st->sel_set) {
    case SET_HOST:
    case SET_DEBUG_HOST:
        if (activate)
            osk_open(st, st->sel_set);
        changed = 0;
        break;
    case SET_RES:
        cycle_res(c, dir ? dir : 1);
        break;
    case SET_FPS:
        c->stream.fps = (c->stream.fps == 60) ? 30 : 60;
        break;
    case SET_BITRATE: {
        int b = c->stream.bitrate + (dir ? dir : 1) * 5000;
        if (b < 5000) b = 5000;
        if (b > 50000) b = 50000;
        c->stream.bitrate = b;
        break;
    }
    case SET_SOPS:        c->sops = !c->sops; break;
    case SET_LOCAL_AUDIO: c->local_audio = !c->local_audio; break;
    case SET_PREFER_HW:   c->prefer_hw = !c->prefer_hw; break;
    case SET_PREFER_YCBCR:c->prefer_ycbcr = !c->prefer_ycbcr; break;
    case SET_CODEC: {
        int n = c->codec + (dir ? dir : 1);
        if (n > CODEC_AUTO) n = CODEC_H264;
        if (n < CODEC_H264) n = CODEC_AUTO;
        c->codec = n;
        config_apply_codec(c);
        break;
    }
    case SET_FILE_LOG:
        c->enable_file_log = !c->enable_file_log;
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/debug.log", st->config_dir);
            log_set_file_enabled(c->enable_file_log ? 1 : 0, path);
        }
        break;
    case SET_SHOW_STATS:  c->show_stats = !c->show_stats; break;
    default: changed = 0; break;
    }
    if (changed)
        save_cfg(st);
}

/* --- drawing --- */

static void draw_osk(ui_state_t *st, ui_surface_t *s) {
    const int px = 480, py = 260, pw = 960, ph = 560;
    ui_rect(s, px - 4, py - 4, pw + 8, ph + 8, COL_ACCENT);
    ui_rect(s, px, py, pw, ph, COL_PANEL);

    const char *title = (st->osk_target == SET_HOST) ? "EDIT HOST" : "EDIT DEBUG HOST";
    ui_text(s, px + 32, py + 28, 3, COL_ACCENT, title);

    /* Value with cursor */
    char line[CONFIG_MAX_HOST + 2];
    snprintf(line, sizeof(line), "%s_", st->osk_buf);
    ui_rect(s, px + 32, py + 88, pw - 64, 44, COL_BG);
    ui_text(s, px + 40, py + 98, 3, COL_TEXT, line);

    /* Grid */
    const int cell = 80, gx0 = px + 80, gy0 = py + 170;
    for (int r = 0; r < OSK_ROWS; r++) {
        for (int cidx = 0; cidx < OSK_COLS; cidx++) {
            int x = gx0 + cidx * cell;
            int y = gy0 + r * cell;
            int sel = (r == st->osk_row && cidx == st->osk_col);
            ui_rect(s, x, y, cell - 8, cell - 8, sel ? COL_SEL : COL_BG);
            if (sel)
                ui_rect(s, x, y + cell - 12, cell - 8, 4, COL_ACCENT);
            char ch[2] = { k_osk_chars[r][cidx], 0 };
            ui_text(s, x + (cell - 8) / 2 - 12, y + (cell - 8) / 2 - 12, 3,
                    sel ? COL_TEXT : COL_DIM, ch);
        }
    }

    ui_text(s, px + 32, py + ph - 40, 2, COL_DIM,
            "X add   [] backspace   O cancel   OPTIONS accept");
}

static void draw_apps(ui_state_t *st, ui_surface_t *s) {
    const int y0 = 250, row_h = 44, visible = 16;

    if (st->apps_err == 2) {
        ui_text(s, 120, y0, 3, COL_WARN, "NO CONNECTION TO HOST");
        ui_text(s, 120, y0 + 50, 2, COL_DIM,
                "Check Host in SETTINGS and press TRIANGLE to retry");
        return;
    }
    if (st->apps_err == 1) {
        ui_text(s, 120, y0, 3, COL_WARN, "FAILED TO FETCH APP LIST");
        ui_text(s, 120, y0 + 50, 2, COL_DIM, "TRIANGLE to retry");
        return;
    }
    if (st->napps == 0) {
        ui_text(s, 120, y0, 3, COL_DIM, "No apps on the server");
        return;
    }

    if (st->sel_app < st->app_first)
        st->app_first = st->sel_app;
    if (st->sel_app >= st->app_first + visible)
        st->app_first = st->sel_app - visible + 1;

    for (int i = 0; i < visible && st->app_first + i < st->napps; i++) {
        int idx = st->app_first + i;
        int y = y0 + i * row_h;
        int sel = (idx == st->sel_app);
        if (sel)
            ui_rect(s, 100, y - 6, UI_W - 200, row_h - 4, COL_SEL);
        int is_running = (st->server && st->server->currentGame != 0 &&
                          st->apps[idx].id == st->server->currentGame);
        int is_cur = !strcasecmp(st->apps[idx].name, st->cfg->app_name);
        ui_text(s, 120, y, 3, sel ? COL_TEXT : COL_DIM, st->apps[idx].name);
        if (is_running)
            ui_text(s, UI_W - 280, y, 3, COL_WARN, "LIVE");
        else if (is_cur)
            ui_text(s, UI_W - 220, y, 3, COL_ACCENT, "*");
    }
    if (st->napps > visible) {
        char sc[32];
        snprintf(sc, sizeof(sc), "%d/%d", st->sel_app + 1, st->napps);
        ui_text(s, UI_W - 220, y0 - 50, 2, COL_DIM, sc);
    }
}

static void draw_settings(ui_state_t *st, ui_surface_t *s) {
    const int y0 = 250, row_h = 52;
    for (int i = 0; i < SET_COUNT; i++) {
        int y = y0 + i * row_h;
        int sel = (i == st->sel_set);
        if (sel)
            ui_rect(s, 100, y - 8, UI_W - 200, row_h - 6, COL_SEL);
        ui_text(s, 120, y, 3, sel ? COL_TEXT : COL_DIM, k_set_names[i]);
        char val[160];
        set_value_str(st, i, val, sizeof(val));
        ui_text(s, 820, y, 3, sel ? COL_ACCENT : COL_DIM, val);
    }
    if (st->host_dirty)
        ui_text(s, 120, y0 + SET_COUNT * row_h + 20, 2, COL_WARN,
                "Host changed: TRIANGLE on APPS reconnects");
}

static void draw_all(ui_state_t *st, ui_surface_t *s) {
    ui_clear(s, COL_BG);

    ui_text(s, 80, 48, 5, COL_TEXT, "MOONLIGHT PS4");

    char status[256];
    if (st->server) {
        snprintf(status, sizeof(status), "Host: %s  (%s)", st->cfg->host,
                 st->server->paired ? "paired" : "not paired");
        ui_text(s, 80, 112, 2, COL_DIM, status);
        if (st->server->currentGame != 0) {
            const char *an = active_app_name(st);
            char live[256];
            if (an)
                snprintf(live, sizeof(live), "Stream active: %s", an);
            else
                snprintf(live, sizeof(live), "Stream active (id=%d)",
                         st->server->currentGame);
            ui_text(s, 80, 136, 2, COL_WARN, live);
        }
    } else {
        snprintf(status, sizeof(status), "Host: %s  (NO CONNECTION)", st->cfg->host);
        ui_text(s, 80, 112, 2, COL_WARN, status);
    }

    /* Tabs */
    ui_text(s, 120, 170, 3, st->tab == TAB_APPS ? COL_TEXT : COL_DIM, "APPS");
    if (st->tab == TAB_APPS)
        ui_rect(s, 120, 202, ui_text_w(3, "APPS"), 4, COL_ACCENT);
    ui_text(s, 320, 170, 3, st->tab == TAB_SETTINGS ? COL_TEXT : COL_DIM, "SETTINGS");
    if (st->tab == TAB_SETTINGS)
        ui_rect(s, 320, 202, ui_text_w(3, "SETTINGS"), 4, COL_ACCENT);

    if (st->tab == TAB_APPS)
        draw_apps(st, s);
    else
        draw_settings(st, s);

    const char *hint;
    if (st->osk_active)
        hint = "";
    else if (st->tab == TAB_APPS) {
        if (st->server && st->server->currentGame != 0)
            hint = "X/O start   OPTIONS close stream   L1/R1 settings   /\\ reload";
        else
            hint = "X/O start   L1/R1 settings   /\\ reload   up/down select";
    } else
        hint = "X edit/toggle   left/right value   O back   L1/R1 apps";
    ui_text(s, 80, UI_H - 60, 2, COL_DIM, hint);

    if (st->osk_active)
        draw_osk(st, s);
}

int ui_menu_run(app_config_t *cfg, gs_server_t *server, const char *config_dir) {
    ui_state_t st;
    memset(&st, 0, sizeof(st));
    st.cfg = cfg;
    st.server = server;
    st.config_dir = config_dir;
    st.result = -1000;
    /* No host / no server: open Settings to configure. */
    if (!cfg->host[0] || !server) {
        st.tab = TAB_SETTINGS;
        st.sel_set = SET_HOST;
    }

    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("ui: video_ui_begin failed");
        return -1;
    }
    LOGI("ui: menu open (server=%s)", server ? "OK" : "NULL");

    /* Offscreen staging: drawing NEVER touches the on-screen buffer. */
    size_t stage_pitch = (size_t)UI_W * 4u;
    size_t stage_bytes = stage_pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(stage_bytes);
    if (!staging) {
        LOGE("ui: staging malloc failed");
        video_ui_end();
        return -1;
    }

    /* First frame NOW: don't hide splash until we have an image (avoids black). */
    {
        ui_surface_t s = { staging, (int)stage_pitch, UI_W, UI_H };
        draw_all(&st, &s);
        for (int i = 0; i < 3; i++) {
            if (video_ui_present(staging, (int)stage_pitch) == 0)
                break;
            usleep(8000);
        }
        ui_hide_splash_once();
    }

    fetch_apps(&st);
    preselect_app(&st);

    /*
     * After OPTIONS+TOUCHPAD from stream, OPTIONS is still held. If s_menu_prev=0,
     * the OPTIONS/X edge would relaunch instantly. Wait for release + absorb.
     */
    input_menu_wait_release(
        MENU_BTN_OPTIONS | MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_TRIANGLE,
        2000);
    input_menu_absorb();

    int dirty = 1;
    int no_pad = 0;
    int idle_frames = 0;
    int grace = 45; /* ~0.7 s without accepting launch */

    while (st.result == -1000) {
        unsigned pr = 0, held = 0;
        if (input_menu_poll(&pr, &held) != 0) {
            if (++no_pad > 180) {
                if (cfg->host[0] && st.napps > 0) {
                    LOGW("ui: no pad; auto-launching app '%s'", cfg->app_name);
                    st.result = UI_MENU_LAUNCH;
                    break;
                }
                no_pad = 0; /* no host: stay in menu */
            }
        } else {
            no_pad = 0;
        }

        if (grace > 0) {
            grace--;
            /* During grace: navigate OK, but don't launch / change host. */
            pr &= ~(MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_OPTIONS |
                    MENU_BTN_TRIANGLE);
            if (held & (MENU_BTN_OPTIONS | MENU_BTN_CROSS | MENU_BTN_CIRCLE))
                input_menu_absorb();
        }

        if (pr) {
            if (st.osk_active)
                osk_input(&st, pr);
            else if (st.tab == TAB_APPS)
                apps_input(&st, pr);
            else
                settings_input(&st, pr);
            dirty = 1;
        }

        /* Exit NOW: don't redraw or flip (SubmitFlip VSYNC can hang
         * with high pending / cur=-1 → eternal black screen). */
        if (st.result != -1000) {
            LOGI("ui: leaving menu (result=%d)", st.result);
            break;
        }

        if (dirty || ++idle_frames >= 3) {
            idle_frames = 0;
            ui_surface_t s = { staging, (int)stage_pitch, UI_W, UI_H };
            if (dirty)
                draw_all(&st, &s);
            if (video_ui_present(staging, (int)stage_pitch) != 0)
                dirty = 1;
            else {
                dirty = 0;
                ui_hide_splash_once();
            }
        }
        usleep(8000);
    }

    LOGI("ui: freeing staging...");
    free(staging);
    LOGI("ui: closing present...");
    video_ui_end();
    LOGI("ui: menu closed => %d (app='%s' host='%s')",
         st.result, cfg->app_name, cfg->host);
    return st.result;
}
