/*
 * gb_widescreen.c -- Opt-in extended horizontal view (widescreen)
 *
 * See gb_widescreen.h and docs/WIDESCREEN.md. All state here is host
 * presentation state: default OFF, never serialized, byte-identical when off.
 */
#include "gb_widescreen.h"
#include "game_extras.h"
#include "gbrt.h"
#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_gbws_extra_left = 0;
int g_gbws_extra_right = 0;
int g_gbws_active = 0;

int g_gbws_pillarbox = 0;
int g_gbws_pillarbox_left = 0;
int g_gbws_pillarbox_right = 0;

int     g_gbws_oam_sidecar = 0;
int16_t g_gbws_oam_x16[40];
int16_t g_gbws_shadow_x16[40];

int16_t g_gbws_obj_true_raw16 = 0;
uint8_t g_gbws_obj_rel8 = 0;
uint8_t g_gbws_obj_ctx_valid = 0;

uint16_t g_gbws_shadow_oam_page = 0;

static int s_cli_request = 0;
static int s_armed_once = 0;

void gb_ws_publish_obj_context(int true_screen_x, uint8_t rel8_biased) {
    g_gbws_obj_true_raw16 = (int16_t)(true_screen_x + 8);
    g_gbws_obj_rel8 = rel8_biased;
    g_gbws_obj_ctx_valid = 1;
}

void gb_ws_invalidate_obj_context(void) {
    g_gbws_obj_ctx_valid = 0;
}

void gb_ws_set_cli_request(int width) {
    s_cli_request = width;
}

int gb_ws_render_width(void) {
    return GB_SCREEN_WIDTH + g_gbws_extra_left + g_gbws_extra_right;
}

/* Pure policy: clamp the requested width to [160, min(game_max, engine_max)]
 * unless the development override is set, then split into margins (odd pixel
 * goes right, matching gbarecomp view_config.h). */
static void resolve_view_geometry(int requested, int game_max, int dev_override,
                                  int* out_left, int* out_right) {
    int ceiling = dev_override ? GB_WS_MAX_VIEW_WIDTH
                               : (game_max < GB_WS_MAX_VIEW_WIDTH ? game_max
                                                                  : GB_WS_MAX_VIEW_WIDTH);
    int width = requested;
    if (width < GB_SCREEN_WIDTH) width = GB_SCREEN_WIDTH;
    if (width > ceiling) width = ceiling;
    {
        int extra = width - GB_SCREEN_WIDTH;
        *out_left = extra / 2;
        *out_right = extra - extra / 2;
    }
}

void gb_ws_arm(struct GBContext* ctx) {
    int requested = 0;
    int dev_override = 0;
    int game_max;
    int left = 0, right = 0;

    if (!ctx || !ctx->ppu || s_armed_once) return;

    {
        const char* env = getenv("GBCRECOMP_VIEW_WIDTH");
        if (env && env[0]) requested = atoi(env);
    }
    if (requested <= 0) requested = s_cli_request;
    if (requested <= GB_SCREEN_WIDTH) return;   /* nothing asked -> stay native */

    {
        const char* wip = getenv("GBCRECOMP_WS_WIP");
        dev_override = (wip && wip[0] == '1');
    }

    game_max = (int)game_max_view_width();
    if (game_max <= GB_SCREEN_WIDTH && !dev_override) {
        fprintf(stderr,
                "[GBWS] view width %d requested but this game has not opted in; "
                "rendering faithful 160x144\n", requested);
        return;
    }

    resolve_view_geometry(requested, game_max, dev_override, &left, &right);
    if (left == 0 && right == 0) return;

    g_gbws_extra_left = left;
    g_gbws_extra_right = right;
    g_gbws_active = 1;
    /* Fail closed until the game module proves the scene each frame. Under
     * the dev override (no game module) there is no one to open the gate, so
     * start open — it is a diagnostics mode. */
    g_gbws_pillarbox = dev_override ? 0 : 1;
    g_gbws_pillarbox_left = 0;
    g_gbws_pillarbox_right = 0;
    memset(g_gbws_oam_x16, 0, sizeof(g_gbws_oam_x16));
    memset(g_gbws_shadow_x16, 0, sizeof(g_gbws_shadow_x16));

    ppu_set_view_margins((GBPPU*)ctx->ppu, left, right);
    s_armed_once = 1;

    fprintf(stderr, "[GBWS] extended view armed: %dx%d (+%d left, +%d right)%s\n",
            gb_ws_render_width(), GB_SCREEN_HEIGHT, left, right,
            dev_override ? " [WS_WIP dev override]" : "");

    game_extended_view_init(ctx, (uint32_t)left, (uint32_t)right);
}

void gb_ws_reapply(struct GBContext* ctx) {
    if (!g_gbws_active || !ctx || !ctx->ppu) return;
    ppu_set_view_margins((GBPPU*)ctx->ppu, g_gbws_extra_left, g_gbws_extra_right);
    memset(g_gbws_oam_x16, 0, sizeof(g_gbws_oam_x16));
    memset(g_gbws_shadow_x16, 0, sizeof(g_gbws_shadow_x16));
    g_gbws_obj_ctx_valid = 0;
}

/* Unwrap a shadow-OAM X write into the 16-bit sidecar: the byte is the low
 * 8 bits of (context + small per-tile layout offset). Without a valid
 * context, or with an implausible offset, fall back to the plain byte
 * (= vanilla placement). Mirrors nesrecomp runtime.c ws_sidecar_track. */
void gb_ws_sidecar_track(int slot, uint8_t val) {
    int16_t wide = val;
    if (slot < 0 || slot >= 40) return;
    if (g_gbws_obj_ctx_valid) {
        int delta = (int8_t)(uint8_t)(val - g_gbws_obj_rel8);
        /* Metasprite layouts span a few tiles around the object origin. */
        if (delta >= -32 && delta <= 56) {
            int w = (int)g_gbws_obj_true_raw16 + delta;
            if (w >= -256 && w < 512) wide = (int16_t)w;
        }
    }
    g_gbws_shadow_x16[slot] = wide;
}

/* OAM DMA fired: pair the render-side sidecar with the OAM snapshot the PPU
 * will see. DMA from the tracked shadow page carries the unwrapped values.
 * Auto-latches the page on first use so tracking starts without game-module
 * help. Returns 0 for a foreign-page DMA — the caller (gbrt.c) then fills
 * g_gbws_oam_x16 with the raw source X bytes (= vanilla placement). */
int gb_ws_sidecar_dma_match(uint8_t src_page) {
    if (!g_gbws_oam_sidecar) return 1;
    if (g_gbws_shadow_oam_page == 0) {
        g_gbws_shadow_oam_page = (uint16_t)src_page;
    }
    if ((uint16_t)src_page == g_gbws_shadow_oam_page) {
        memcpy(g_gbws_oam_x16, g_gbws_shadow_x16, sizeof(g_gbws_oam_x16));
        return 1;
    }
    return 0;
}

/* WRAM write tap (called from gb_write8 only while the view is armed):
 * shadow-OAM X tracking plus the game module's watch-range hook (used for
 * context publish from the game's own screen-X computation globals). */
void (*g_gbws_wram_write_hook)(struct GBContext* ctx, uint16_t addr, uint8_t val) = 0;
uint16_t g_gbws_watch_lo = 1;   /* empty range by default */
uint16_t g_gbws_watch_hi = 0;

void gb_ws_wram_write_tap(struct GBContext* ctx, uint16_t addr, uint8_t val) {
    if (g_gbws_oam_sidecar && (addr >> 8) == g_gbws_shadow_oam_page &&
        (addr & 3) == 1 && (addr & 0xFF) < 0xA0) {
        gb_ws_sidecar_track((addr & 0xFF) >> 2, val);
    }
    if (g_gbws_wram_write_hook && addr >= g_gbws_watch_lo && addr <= g_gbws_watch_hi) {
        g_gbws_wram_write_hook(ctx, addr, val);
    }
}

/* Direct CPU write into OAM (no DMA): vanilla placement for that slot. */
void gb_ws_sidecar_direct_oam(int slot, uint8_t val) {
    if (!g_gbws_oam_sidecar) return;
    if (slot < 0 || slot >= 40) return;
    g_gbws_oam_x16[slot] = val;
    g_gbws_shadow_x16[slot] = val;
}
