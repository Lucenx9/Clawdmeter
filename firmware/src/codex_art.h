#pragma once
#include <stdint.h>
// The Codex page's corner sprite: the Codex cloud as pixel art, generated
// from its 24x24 mark (filled, glyph cut-outs closed) at three body sizes for
// squash and stretch. '#' = body. The ">_" glyph, shading and every
// animation are drawn at run time (codex_sprite.cpp).
// Regenerate: tools/codex_art.py.

struct CodexBody { uint8_t w, h; const char* rows; };

static const char CODEX_BODY_NORMAL_ROWS[] =
    "......####........"
    "....##########...."
    "....############.."
    "..##############.."
    ".################."
    "#################."
    "#################."
    "##################"
    "##################"
    ".#################"
    ".#################"
    ".################."
    "..##############.."
    "..############...."
    "....##########...."
    "........####......";
static const char CODEX_BODY_SQUASH_ROWS[] =
    "......#####........."
    "....############...."
    "....##############.."
    "..#################."
    "###################."
    "###################."
    "####################"
    "####################"
    ".###################"
    ".###################"
    ".#################.."
    "..##############...."
    "....###########....."
    ".........#####......";
static const char CODEX_BODY_STRETCH_ROWS[] =
    ".....####......."
    "....########...."
    "...###########.."
    "...###########.."
    ".##############."
    ".##############."
    "###############."
    "###############."
    "################"
    "################"
    ".###############"
    ".###############"
    ".##############."
    ".##############."
    "..###########..."
    "..###########..."
    "....########...."
    ".......####.....";

static const CodexBody CODEX_BODIES[3] = {
    { 18, 16, CODEX_BODY_NORMAL_ROWS },
    { 20, 14, CODEX_BODY_SQUASH_ROWS },
    { 16, 18, CODEX_BODY_STRETCH_ROWS },
};
enum { CODEX_BODY_NORMAL, CODEX_BODY_SQUASH, CODEX_BODY_STRETCH };
