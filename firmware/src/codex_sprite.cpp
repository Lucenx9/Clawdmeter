#include "codex_sprite.h"
#include "codex_art.h"
#include "clawd_policy.h"
#include "splash_geometry.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// ─── Look ────────────────────────────────────────────────────────────────────
// Body in TH_TEXT, its bottom rim in TH_DIM, the ">_" in TH_BG: on the Codex
// page's black-and-white palette that's a white cloud with a black prompt.
#define CX_MAX_W 20
#define CX_MAX_H 18
// The chevron's column offset per row (two cells wide), and the cursor.
static const int8_t CHEVRON[6] = { 0, 1, 2, 2, 1, 0 };

// ─── Timing ──────────────────────────────────────────────────────────────────
static const uint16_t HOP_MS       = 420;   // one hop: crouch, air, land
static const uint16_t BLINK_MS     = 530;   // the cursor, like a terminal's
static const uint8_t  TRAVEL_CELLS = 4;     // hop height entering / leaving
static const uint8_t  HOP_CELLS    = 6;     // a hop in the seat
// Never above the top edge: a short panel's seat has little headroom.
static int headroom_cells(int want);
static const uint8_t  STRIDE_CELLS = 12;    // ground covered per travel hop

enum Mode { CM_AWAY, CM_TRAVEL, CM_SEATED, CM_HOP, CM_NUDGE, CM_THINK, CM_ALERT };
enum Act { ACT_HOP, ACT_NUDGE, ACT_THINK, ACT_COUNT };

static lv_obj_t*      img = NULL;
static uint8_t*       buf = NULL;
static lv_image_dsc_t dsc;
static int cell = 3, slot_x = 0, feet_y = 0, screen_w = 480, screen_h = 480;
static bool round_panel = false;

static Mode     mode = CM_AWAY;
static uint32_t started = 0;        // current mode / act start
static int      x = 0;              // seat-relative left edge of the normal body, px
static int      from_x = 0, to_x = 0;
static uint8_t  hops = 1;
static bool     leaving = false;
static bool     minimal = false;
static bool     alerting = false;   // Codex needs you: the alert loop whenever it's seated
static uint32_t seated_since = 0;
static uint32_t idle_gap = 5000;
static int      last_act = -1;
static ClawdRng rng;

// What the frame on screen shows, so it's only redrawn when it changes.
struct Look { uint8_t body; int8_t dy; int8_t nudge; uint8_t dots; bool cursor; int x; };
static Look shown = { 255, 0, 0, 0, false, 0 };

static uint32_t now_ms(void) { return millis(); }

static int headroom_cells(int want) {
    const int room = (feet_y - CX_MAX_H * cell) / (cell ? cell : 1);
    return room < 1 ? 1 : room < want ? room : want;
}

static int body_w(void) { return CODEX_BODIES[CODEX_BODY_NORMAL].w * cell; }
static int off_left_x(void) {
    return offstage_left_x(screen_w, screen_h, round_panel, feet_y, CX_MAX_W * cell);
}

// ─── Drawing ─────────────────────────────────────────────────────────────────
static bool in_body(const CodexBody& b, int gx, int gy) {
    return gx >= 0 && gy >= 0 && gx < b.w && gy < b.h && b.rows[gy * b.w + gx] == '#';
}

static void draw(const Look& l) {
    if (!img || !buf || l.body > CODEX_BODY_STRETCH) return;   // nothing drawn yet (shown.body 255)
    const CodexBody& b = CODEX_BODIES[l.body];
    const int w = b.w * cell, h = b.h * cell;
    uint16_t* color = (uint16_t*)buf;
    uint8_t*  alpha = buf + (size_t)w * h * 2;
    const uint16_t c_body  = theme_rgb565(theme_hex(TH_TEXT));
    const uint16_t c_shade = theme_rgb565(theme_hex(TH_DIM));
    const uint16_t c_ink   = theme_rgb565(theme_hex(TH_BG));
    const int cx = b.w / 2, cy = b.h / 2;
    for (int gy = 0; gy < b.h; gy++) {
        for (int gx = 0; gx < b.w; gx++) {
            uint16_t col = 0;
            uint8_t  al = 0;
            if (in_body(b, gx, gy)) {
                al = 255;
                col = (!in_body(b, gx, gy + 1) || !in_body(b, gx + 1, gy + 1)) ? c_shade : c_body;
                bool ink = false;
                if (l.dots) {
                    for (int i = 0; i < l.dots && !ink; i++) {
                        const int dx = gx - (cx - 5 + i * 4), dy = gy - cy;
                        ink = dx >= 0 && dx < 2 && dy >= 0 && dy < 2;
                    }
                } else {
                    const int row = gy - (cy - 3);
                    if (row >= 0 && row < 6) {
                        const int c0 = cx - 5 + CHEVRON[row] + l.nudge;
                        ink = gx == c0 || gx == c0 + 1;
                    }
                    if (l.cursor && (gy == cy + 2 || gy == cy + 3) && gx >= cx && gx < cx + 5) ink = true;
                }
                if (ink) col = c_ink;
            }
            for (int dy = 0; dy < cell; dy++) {
                uint16_t* cp = &color[(gy * cell + dy) * w + gx * cell];
                uint8_t*  ap = &alpha[(gy * cell + dy) * w + gx * cell];
                for (int dx = 0; dx < cell; dx++) { cp[dx] = col; ap[dx] = al; }
            }
        }
    }
    dsc.header.w = w;
    dsc.header.h = h;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc.header.stride = w * 2;
    dsc.data = buf;
    dsc.data_size = (size_t)w * h * 3;
    lv_image_set_src(img, &dsc);
    // Centred on the normal body's footprint, standing on the feet line.
    lv_obj_set_pos(img, l.x - (w - body_w()) / 2, feet_y + l.dy * cell - h);
    lv_obj_invalidate(img);
}

static void show_look(const Look& l) {
    if (l.body == shown.body && l.dy == shown.dy && l.nudge == shown.nudge && l.dots == shown.dots &&
        l.cursor == shown.cursor && l.x == shown.x)
        return;
    shown = l;
    draw(l);
}

static void set_visible(bool on) {
    if (!img) return;
    if (on) lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
}

// One hop at phase t (0..1): crouch, stretch up, fall, land squashed.
static void hop_look(float t, int height, Look* l) {
    if (t < 0.18f) { l->body = CODEX_BODY_SQUASH; l->dy = 0; return; }
    if (t > 0.82f) { l->body = CODEX_BODY_SQUASH; l->dy = 0; return; }
    const float u = (t - 0.18f) / 0.64f;            // 0..1 in the air
    l->dy = (int8_t)(-4.0f * height * u * (1.0f - u) - 0.5f);
    l->body = u < 0.5f ? CODEX_BODY_STRETCH : CODEX_BODY_NORMAL;
}

// ─── Modes ───────────────────────────────────────────────────────────────────
static void seat(void) {
    if (alerting && !minimal) {
        mode = CM_ALERT;
        x = slot_x;
        started = now_ms();
        set_visible(true);
        return;
    }
    mode = CM_SEATED;
    x = slot_x;
    seated_since = now_ms();
    idle_gap = clawd_jitter(6000UL * clawd_tuning().idle_gap_pct / 100, &rng);
    set_visible(true);
}

static void travel(int from, int to, bool out) {
    from_x = from;
    to_x = to;
    leaving = out;
    const int dist = to > from ? to - from : from - to;
    const int stride = STRIDE_CELLS * cell;
    hops = (uint8_t)((dist + stride - 1) / stride);
    if (hops < 1) hops = 1;
    mode = CM_TRAVEL;
    started = now_ms();
    set_visible(true);
}

void codex_sprite_create(lv_obj_t* parent, int sx, int feet, int c) {
    cell = c; slot_x = sx; feet_y = feet;
    const BoardCaps& caps = board_caps();
    screen_w = caps.width;
    screen_h = caps.height;
    round_panel = caps.round_panel;
    clawd_rng_seed(&rng, 0xC0DE5u ^ (uint32_t)now_ms());
    const size_t bytes = (size_t)CX_MAX_W * cell * CX_MAX_H * cell * 3;
    buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    img = lv_image_create(parent);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    mode = CM_AWAY;
}

void codex_sprite_enter(void) {
    if (!img) return;
    if (minimal) { codex_sprite_show_now(true); return; }
    travel(mode == CM_TRAVEL ? shown.x : off_left_x(), slot_x, false);
}

void codex_sprite_leave(void) {
    if (!img || mode == CM_AWAY) return;
    if (minimal) { codex_sprite_show_now(false); return; }
    travel(mode == CM_TRAVEL ? shown.x : x, off_left_x(), true);
}

void codex_sprite_show_now(bool on) {
    if (!img) return;
    shown.body = 255;   // force a redraw
    if (on) {
        seat();
        Look l = { CODEX_BODY_NORMAL, 0, 0, 0, true, slot_x };
        show_look(l);
    } else {
        mode = CM_AWAY;
        set_visible(false);
    }
}

static bool idle_now(void) { return mode == CM_SEATED || mode == CM_NUDGE || mode == CM_THINK; }

void codex_sprite_hop(void) {
    if (minimal || !idle_now()) return;
    mode = CM_HOP;
    started = now_ms();
}

void codex_sprite_react(char event) {
    if (minimal || !clawd_tuning().reactions || !idle_now()) return;
    if (event == 's') { codex_sprite_hop(); return; }
    if (event == 'p') { mode = CM_THINK; started = now_ms(); }
}

void codex_sprite_alert(bool on) {
    if (on == alerting) return;
    alerting = on;
    if (on && idle_now()) seat();                 // straight into the alert loop
    else if (!on && mode == CM_ALERT) seat();     // back to idling
}

bool codex_sprite_visible(void) { return img && mode != CM_AWAY; }
bool codex_sprite_busy(void) { return mode == CM_TRAVEL || mode == CM_HOP; }

void codex_sprite_set_minimal(bool on) {
    minimal = on;
    if (mode != CM_AWAY) codex_sprite_show_now(true);
}

void codex_sprite_repaint(void) {
    if (mode == CM_AWAY) return;
    draw(shown);
}

static void start_act(void) {
    int a;
    do { a = (int)clawd_rng_range(&rng, 0, ACT_COUNT - 1); } while (a == last_act);
    last_act = a;
    started = now_ms();
    mode = a == ACT_HOP ? CM_HOP : a == ACT_NUDGE ? CM_NUDGE : CM_THINK;
}

void codex_sprite_tick(void) {
    if (!img || mode == CM_AWAY) return;
    const uint32_t now = now_ms();
    const uint32_t el = now - started;
    Look l = { CODEX_BODY_NORMAL, 0, 0, 0, true, x };
    switch (mode) {
    case CM_TRAVEL: {
        const uint32_t total = (uint32_t)hops * HOP_MS;
        if (el >= total) {
            if (leaving) { mode = CM_AWAY; set_visible(false); return; }
            seat();
            l.x = slot_x;
            break;
        }
        const uint32_t i = el / HOP_MS;
        const float t = (float)(el % HOP_MS) / HOP_MS;
        hop_look(t, headroom_cells(TRAVEL_CELLS), &l);
        // Ground is only covered in the air.
        float air = t < 0.18f ? 0.0f : t > 0.82f ? 1.0f : (t - 0.18f) / 0.64f;
        const float done_f = ((float)i + air) / hops;
        l.x = from_x + (int)((to_x - from_x) * done_f);
        break;
    }
    case CM_HOP:
        if (el >= HOP_MS) { seat(); break; }
        hop_look((float)el / HOP_MS, headroom_cells(HOP_CELLS), &l);
        break;
    case CM_NUDGE:   // the prompt leans forward twice: "go on"
        if (el >= 600) { seat(); break; }
        l.nudge = ((el / 150) % 2 == 0) ? 1 : 0;
        break;
    case CM_THINK:   // three dots fill in, twice
        if (el >= 1800) { seat(); break; }
        l.dots = (uint8_t)(1 + (el / 300) % 3);
        break;
    case CM_ALERT: {  // "over here": a hop, then the prompt leans at you, again and again
        const uint32_t t = el % 1500;
        if (t < HOP_MS) hop_look((float)t / HOP_MS, headroom_cells(HOP_CELLS), &l);
        else if (t < 1100) l.nudge = ((t - HOP_MS) / 120) % 2 == 0 ? 1 : 0;
        break;
    }
    case CM_SEATED:
        if (!minimal && clawd_tuning().idle_acts && now - seated_since >= idle_gap) { start_act(); return; }
        break;
    default:
        break;
    }
    if (mode == CM_SEATED) l.cursor = minimal || !clawd_tuning().spinner || ((now / BLINK_MS) % 2 == 0);
    show_look(l);
}
