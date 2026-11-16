#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

// The Codex page's corner sprite: the Codex cloud with its ">_" prompt
// (codex_art.h), in Clawd's seat while the meters show Codex. Same pixel-art
// cells as the corner Clawd. The director (clawd_director.cpp) brings it on
// once Clawd has walked off and sends it away before he comes back, so the
// two are never on screen together.
//
// Seated it idles on its own: the cursor blinks, and now and then it hops,
// nudges its prompt, or thinks (three dots). It enters and leaves with a few
// squash-and-stretch hops through the left edge.

void codex_sprite_create(lv_obj_t* parent, int slot_x, int feet_y, int cell);
void codex_sprite_tick(void);

void codex_sprite_enter(void);       // hop in from the left edge to the seat
void codex_sprite_leave(void);       // hop out through the left edge
void codex_sprite_show_now(bool on); // in the seat (or gone) at once: minimal mode, resets
void codex_sprite_hop(void);         // a hop in the seat (a tap on the meters)

bool codex_sprite_visible(void);     // any part of it on screen
bool codex_sprite_busy(void);        // entering, leaving or hopping
void codex_sprite_set_minimal(bool on);   // a still frame, no idle acts
void codex_sprite_repaint(void);     // redraw the frame on screen (a new palette)
