#include "clawd_prefs.h"
#include "clawd_director.h"
#include "clawd_policy.h"
#include "ui.h"
#include "face.h"
#include "clawd_anim.h"
#include "mascot.h"
#include "performer.h"
#include "codex_sprite.h"
#include <string.h>
#include "button_action.h"
#include "hal/board_caps.h"
#include <Preferences.h>
#include <Arduino.h>

// Per ButtonSlot: its NVS key and default (`k1`..`k3`).
static const char* const BTN_KEYS[BTN_SLOT_COUNT] = { "btn_k1", "btn_k2", "btn_k3" };
static const int32_t BTN_DEFAULTS[BTN_SLOT_COUNT] = {
    BTN_ACT_PRIMARY_DEFAULT, BTN_ACT_SECONDARY_DEFAULT, BTN_ACT_PWR_DEFAULT,
};
static int32_t btn_actions[BTN_SLOT_COUNT] = {
    BTN_ACT_PRIMARY_DEFAULT, BTN_ACT_SECONDARY_DEFAULT, BTN_ACT_PWR_DEFAULT,
};

void clawd_prefs_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    const int lively = prefs.getUChar("lively", CLAWD_LIVELINESS_DEFAULT);
    const int attn = prefs.getUChar("attn_anim", CLAWD_ATTN_ANIM_DEFAULT_S);
    const bool pace = prefs.getUChar("pace_mark", 0) != 0;
    for (int i = 0; i < BTN_SLOT_COUNT; i++)
        btn_actions[i] = button_action_clamp(prefs.getInt(BTN_KEYS[i], BTN_DEFAULTS[i]), BTN_DEFAULTS[i]);
    prefs.end();
    director_set_liveliness(clawd_liveliness_clamp(lively));
    director_set_attn_anim(clawd_attn_anim_clamp(attn));
    pace_marker_set(pace);
    Serial.printf("Device prefs init: liveliness %d, attention animation %d s, pace marker %s, "
                  "buttons 0x%lx 0x%lx 0x%lx\n",
                  clawd_liveliness(), clawd_attn_anim(), pace ? "on" : "off",
                  (unsigned long)btn_actions[0], (unsigned long)btn_actions[1], (unsigned long)btn_actions[2]);
}

// The palette is kept as its `th` hex string.
void theme_prefs_load(void) {
    if (board_caps().mono_panel) return;   // 1-bit: the default palette only
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    char hex[THEME_HEX_LEN + 1] = {};
    const size_t n = prefs.isKey("theme") ? prefs.getBytes("theme", hex, THEME_HEX_LEN) : 0;
    prefs.end();
    uint32_t p[TH_COUNT];
    if (n == THEME_HEX_LEN && theme_parse_hex(hex, p)) {
        theme_palette_set(p);
        clawd_anim_recolor();
        Serial.printf("Theme: %s (saved)\n", hex);
    }
    prefs.begin("clawdmeter", true);
    if (prefs.isKey("fable_col")) theme_fable_set((uint32_t)prefs.getInt("fable_col", (int32_t)theme_fable_hex()));
    prefs.end();
}

static void save(const char* key, uint8_t v) {
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar(key, v);
    prefs.end();
}

void liveliness_set(int level) {
    level = clawd_liveliness_clamp(level);
    if (level == clawd_liveliness()) return;
    save("lively", (uint8_t)level);
    director_set_liveliness(level);
    Serial.printf("Liveliness: %d\n", level);
}

int liveliness_get(void) { return clawd_liveliness(); }

void attn_anim_set(int seconds) {
    seconds = clawd_attn_anim_clamp(seconds);
    if (seconds == clawd_attn_anim()) return;
    save("attn_anim", (uint8_t)seconds);
    director_set_attn_anim(seconds);
    Serial.printf("Attention animation: %d s\n", seconds);
}

int attn_anim_get(void) { return clawd_attn_anim(); }

static bool pace_marker = false;

void pace_marker_set(bool on) {
    ui_set_pace_marker(on);
    if (on == pace_marker) return;
    pace_marker = on;
    save("pace_mark", on ? 1 : 0);
    Serial.printf("Pace marker: %s\n", on ? "on" : "off");
}

bool pace_marker_get(void) { return pace_marker; }

void button_action_set(ButtonSlot slot, int32_t action) {
    if (slot < 0 || slot >= BTN_SLOT_COUNT) return;
    action = button_action_clamp(action, BTN_DEFAULTS[slot]);
    if (action == btn_actions[slot]) return;
    btn_actions[slot] = action;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putInt(BTN_KEYS[slot], action);
    prefs.end();
    Serial.printf("Button %d action: 0x%lx\n", (int)slot + 1, (unsigned long)action);
}

int32_t button_action_get(ButtonSlot slot) {
    return (slot >= 0 && slot < BTN_SLOT_COUNT) ? btn_actions[slot] : BTN_ACT_NONE;
}

// The palette the app chose; the live one differs only while the meters
// show the Codex page (theme_codex_view).
static uint32_t user_palette[TH_COUNT];
static bool     user_palette_known = false;
static bool     codex_view = false;

// Codex's black and white. Clawd's own colours stay the user's (he's away
// while it shows, and must come back as he left).
static void codex_palette(uint32_t out[TH_COUNT]) {
    static const uint32_t CODEX[TH_COUNT] = {
        0x000000, 0x161616, 0xf5f5f5, 0x9e9e9e, 0xffffff,
        0xe8e8e8, 0xd9a43a, 0xe5484d, 0x2b2b2b, 0, 0,
    };
    memcpy(out, CODEX, sizeof(CODEX));
    out[TH_CLAWD] = user_palette[TH_CLAWD];
    out[TH_EYE]   = user_palette[TH_EYE];
}

static void remember_live_palette(void) {
    if (user_palette_known) return;
    for (int i = 0; i < TH_COUNT; i++) user_palette[i] = theme_hex((ThemeColor)i);
    user_palette_known = true;
}

static void apply_live(const uint32_t p[TH_COUNT]) {
    if (!theme_palette_set(p)) return;
    ui_apply_theme();
    face_apply_theme();
    clawd_anim_recolor();
    mascot_repaint();
    performer_repaint();
    codex_sprite_repaint();
}

// Kept apart from the palette, as its own `fc`.
void fable_color_set(uint32_t rgb) {
    if (board_caps().mono_panel) return;   // 1-bit: no overlay to colour
    if (!theme_fable_set(rgb)) return;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putInt("fable_col", (int32_t)theme_fable_hex());
    prefs.end();
    ui_apply_theme();
    Serial.printf("Fable colour: %06lx\n", (unsigned long)theme_fable_hex());
}

void theme_set(const uint32_t p[TH_COUNT]) {
    if (board_caps().mono_panel) return;   // 1-bit: the default palette only
    remember_live_palette();
    if (memcmp(user_palette, p, sizeof(user_palette)) == 0) return;
    memcpy(user_palette, p, sizeof(user_palette));
    char hex[THEME_HEX_LEN + 1];
    theme_format_hex(p, hex);
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putBytes("theme", hex, THEME_HEX_LEN);
    prefs.end();
    if (codex_view) {
        uint32_t c[TH_COUNT];
        codex_palette(c);
        apply_live(c);
    } else {
        apply_live(p);
    }
    Serial.printf("Theme: %s\n", hex);
}

void theme_codex_view(bool on) {
    if (board_caps().mono_panel || on == codex_view) return;
    remember_live_palette();
    codex_view = on;
    if (on) {
        uint32_t c[TH_COUNT];
        codex_palette(c);
        apply_live(c);
    } else {
        apply_live(user_palette);
    }
}
