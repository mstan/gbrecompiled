#include "gbrt.h"
#include "audio.h"
#include "ppu.h"
#include "cosim_state.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define DIFF_WRAM_SIZE (0x1000u * 8u)
#define DIFF_VRAM_TOTAL_SIZE (VRAM_SIZE * 2u)
#define DIFF_OAM_SIZE OAM_SIZE
#define DIFF_HRAM_SIZE 0x7Fu
#define DIFF_IO_TOTAL_SIZE 0x81u
#define DIFF_MAX_SCRIPT_ENTRIES 256

typedef enum {
    GB_DIFF_INPUT_FRAME = 0,
    GB_DIFF_INPUT_CYCLE = 1,
} GBDiffInputAnchor;

typedef struct {
    GBDiffInputAnchor anchor;
    uint64_t start;
    uint64_t duration;
    uint8_t dpad;
    uint8_t buttons;
} GBDiffScriptEntry;

typedef struct {
    GBDiffScriptEntry entries[DIFF_MAX_SCRIPT_ENTRIES];
    size_t count;
    uint64_t frame_index;
} GBDiffInputScript;

static uint8_t gb_diff_pack_flags(const GBContext* ctx) {
    return (ctx->f_z ? 0x80 : 0) |
           (ctx->f_n ? 0x40 : 0) |
           (ctx->f_h ? 0x20 : 0) |
           (ctx->f_c ? 0x10 : 0);
}

static uint16_t gb_diff_current_bank(const GBContext* ctx) {
    return (ctx->pc < 0x4000) ? 0 : ctx->rom_bank;
}

static void gb_diff_read_opcode_bytes(const GBContext* ctx, uint8_t bytes[3]) {
    GBContext* mutable_ctx = (GBContext*)ctx;
    bytes[0] = gb_read8(mutable_ctx, ctx->pc);
    bytes[1] = gb_read8(mutable_ctx, (uint16_t)(ctx->pc + 1));
    bytes[2] = gb_read8(mutable_ctx, (uint16_t)(ctx->pc + 2));
}

static void gb_diff_print_state(FILE* stream, const char* label, const GBContext* ctx) {
    fprintf(stream,
            "[DIFF] %s PC=%03X:%04X SP=%04X "
            "A=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X F=%02X "
            "IME=%u IME_PENDING=%u HALT=%u STOP=%u STOP_MODE=%u HALT_BUG=%u DS=%u "
            "ROM=%03X RAM=%02X WRAM=%u VRAM=%u DMA=%u/%u HDMA=%04X->%04X/%u "
            "CYC=%u FRAME=%u DIV=%04X\n",
            label,
            gb_diff_current_bank(ctx),
            ctx->pc,
            ctx->sp,
            ctx->a,
            ctx->b,
            ctx->c,
            ctx->d,
            ctx->e,
            ctx->h,
            ctx->l,
            gb_diff_pack_flags(ctx),
            ctx->ime,
            ctx->ime_pending,
            ctx->halted,
            ctx->stopped,
            ctx->stop_mode_active,
            ctx->halt_bug,
            ctx->cgb_double_speed,
            ctx->rom_bank,
            ctx->ram_bank,
            ctx->wram_bank,
            ctx->vram_bank,
            ctx->dma.progress,
            ctx->dma.cycles_remaining,
            ctx->hdma.source,
            ctx->hdma.dest,
            ctx->hdma.blocks_remaining,
            ctx->cycles,
            ctx->frame_cycles,
            ctx->div_counter);
}

static void gb_diff_print_code_bytes(FILE* stream,
                                     const char* label,
                                     const GBContext* ctx,
                                     uint16_t addr,
                                     size_t count) {
    GBContext* mutable_ctx = (GBContext*)ctx;

    fprintf(stream, "[DIFF] %s bytes at %03X:%04X:", label, gb_diff_current_bank(ctx), addr);
    for (size_t i = 0; i < count; i++) {
        fprintf(stream, " %02X", gb_read8(mutable_ctx, (uint16_t)(addr + i)));
    }
    fputc('\n', stream);
}

static void gb_diff_parse_buttons(const char* buttons, uint8_t* dpad, uint8_t* joypad_buttons) {
    *dpad = 0xFF;
    *joypad_buttons = 0xFF;

    if (buttons == NULL) {
        return;
    }

    if (strchr(buttons, 'U')) *dpad &= (uint8_t)~0x04;
    if (strchr(buttons, 'D')) *dpad &= (uint8_t)~0x08;
    if (strchr(buttons, 'L')) *dpad &= (uint8_t)~0x02;
    if (strchr(buttons, 'R')) *dpad &= (uint8_t)~0x01;
    if (strchr(buttons, 'A')) *joypad_buttons &= (uint8_t)~0x01;
    if (strchr(buttons, 'B')) *joypad_buttons &= (uint8_t)~0x02;
    if (strchr(buttons, 'T')) *joypad_buttons &= (uint8_t)~0x04;
    if (strchr(buttons, 'S')) *joypad_buttons &= (uint8_t)~0x08;
}

static char* gb_diff_trim_ascii(char* text) {
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }
    if (!text || !*text) {
        return text;
    }

    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool gb_diff_parse_input_token(char* token,
                                      GBDiffScriptEntry* entry,
                                      char* buttons,
                                      size_t buttons_size) {
    char* start_text = gb_diff_trim_ascii(token);
    if (!start_text || !*start_text) {
        return false;
    }

    char* first_colon = strchr(start_text, ':');
    if (!first_colon) {
        return false;
    }
    *first_colon = '\0';

    char* buttons_text = gb_diff_trim_ascii(first_colon + 1);
    char* second_colon = strchr(buttons_text, ':');
    if (!second_colon) {
        return false;
    }
    *second_colon = '\0';

    char* duration_text = gb_diff_trim_ascii(second_colon + 1);
    start_text = gb_diff_trim_ascii(start_text);
    buttons_text = gb_diff_trim_ascii(buttons_text);
    duration_text = gb_diff_trim_ascii(duration_text);
    if (!*start_text || !*duration_text) {
        return false;
    }

    entry->anchor = GB_DIFF_INPUT_FRAME;
    if (*start_text == 'c' || *start_text == 'C') {
        entry->anchor = GB_DIFF_INPUT_CYCLE;
        start_text++;
    } else if (*start_text == 'f' || *start_text == 'F') {
        start_text++;
    }
    start_text = gb_diff_trim_ascii(start_text);
    if (!*start_text) {
        return false;
    }

    char* start_end = NULL;
    char* duration_end = NULL;
    entry->start = strtoull(start_text, &start_end, 10);
    entry->duration = strtoull(duration_text, &duration_end, 10);
    if ((start_end && *gb_diff_trim_ascii(start_end)) || (duration_end && *gb_diff_trim_ascii(duration_end))) {
        return false;
    }
    if (entry->duration == 0) {
        return false;
    }

    snprintf(buttons, buttons_size, "%s", buttons_text);
    return true;
}

static bool gb_diff_parse_input_script(const char* script,
                                       GBDiffInputScript* parsed,
                                       char* message,
                                       size_t message_size) {
    memset(parsed, 0, sizeof(*parsed));

    if (script == NULL || *script == '\0') {
        return true;
    }

    char* copy = strdup(script);
    if (copy == NULL) {
        snprintf(message, message_size, "failed to allocate input script copy");
        return false;
    }

    char* token = strtok(copy, ",");
    while (token != NULL) {
        if (parsed->count >= DIFF_MAX_SCRIPT_ENTRIES) {
            snprintf(message, message_size, "input script exceeds %u entries", DIFF_MAX_SCRIPT_ENTRIES);
            free(copy);
            return false;
        }

        char buttons[32] = {0};

        GBDiffScriptEntry* entry = &parsed->entries[parsed->count];
        if (!gb_diff_parse_input_token(token, entry, buttons, sizeof(buttons))) {
            snprintf(message, message_size, "invalid input script token: %s", token);
            free(copy);
            return false;
        }

        parsed->count++;
        gb_diff_parse_buttons(buttons, &entry->dpad, &entry->buttons);

        token = strtok(NULL, ",");
    }

    free(copy);
    return true;
}

static void gb_diff_request_joypad_interrupt(GBContext* ctx,
                                             const GBJoypadState* previous,
                                             const GBJoypadState* current) {
    if (ctx == NULL || previous == NULL || current == NULL) {
        return;
    }

    uint8_t joyp = ctx->io[0x00];
    bool dpad_selected = (joyp & 0x10) == 0;
    bool buttons_selected = (joyp & 0x20) == 0;

    bool pressed = false;
    if (dpad_selected) {
        uint8_t falling = (previous->dpad ^ current->dpad) & previous->dpad & (uint8_t)~current->dpad;
        pressed |= (falling & 0x0F) != 0;
    }
    if (buttons_selected) {
        uint8_t falling = (previous->buttons ^ current->buttons) & previous->buttons & (uint8_t)~current->buttons;
        pressed |= (falling & 0x0F) != 0;
    }

    if (pressed) {
        ctx->io[0x0F] |= 0x10;
        if (ctx->halted) {
            ctx->halted = 0;
        }
    }
}

static void gb_diff_apply_input_state(const GBDiffInputScript* script,
                                      GBContext* ctx,
                                      GBJoypadState* state) {
    GBJoypadState next = {.dpad = 0xFF, .buttons = 0xFF};
    uint64_t current_cycles = ctx ? ctx->cycles : 0;

    if (script != NULL) {
        for (size_t i = 0; i < script->count; i++) {
            const GBDiffScriptEntry* entry = &script->entries[i];
            bool active = false;
            if (entry->anchor == GB_DIFF_INPUT_CYCLE) {
                active = current_cycles >= entry->start &&
                         current_cycles < (entry->start + entry->duration);
            } else {
                active = script->frame_index >= entry->start &&
                         script->frame_index < (entry->start + entry->duration);
            }
            if (active) {
                next.dpad &= entry->dpad;
                next.buttons &= entry->buttons;
            }
        }
    }

    gb_diff_request_joypad_interrupt(ctx, state, &next);
    *state = next;
    if (ctx != NULL) {
        ctx->last_joypad = (uint8_t)(state->dpad & state->buttons);
    }
}

static bool gb_diff_compare_region(const char* name,
                                   const uint8_t* generated,
                                   const uint8_t* interpreted,
                                   size_t size,
                                   char* message,
                                   size_t message_size) {
    if (generated == NULL || interpreted == NULL) {
        if (generated != interpreted) {
            snprintf(message, message_size, "%s presence mismatch", name);
            return false;
        }
        return true;
    }

    for (size_t i = 0; i < size; i++) {
        if (generated[i] != interpreted[i]) {
            snprintf(message, message_size,
                     "%s mismatch at offset 0x%zX: %02X != %02X",
                     name, i, generated[i], interpreted[i]);
            return false;
        }
    }

    return true;
}

static bool gb_diff_compare_ppu(const GBContext* generated_ctx,
                                const GBContext* interpreted_ctx,
                                bool compare_memory,
                                char* message,
                                size_t message_size) {
    const GBPPU* generated = (const GBPPU*)generated_ctx->ppu;
    const GBPPU* interpreted = (const GBPPU*)interpreted_ctx->ppu;

    if (generated == NULL || interpreted == NULL) {
        if (generated != interpreted) {
            snprintf(message, message_size, "PPU presence mismatch");
            return false;
        }
        return true;
    }

#define DIFF_PPU_FIELD(field, fmt) \
    do { \
        if (generated->field != interpreted->field) { \
            snprintf(message, message_size, \
                     "PPU field %s mismatch: " fmt " != " fmt, \
                     #field, generated->field, interpreted->field); \
            return false; \
        } \
    } while (0)

    DIFF_PPU_FIELD(lcdc, "%u");
    DIFF_PPU_FIELD(stat, "%u");
    DIFF_PPU_FIELD(scy, "%u");
    DIFF_PPU_FIELD(scx, "%u");
    DIFF_PPU_FIELD(ly, "%u");
    DIFF_PPU_FIELD(lyc, "%u");
    DIFF_PPU_FIELD(dma, "%u");
    DIFF_PPU_FIELD(bgp, "%u");
    DIFF_PPU_FIELD(obp0, "%u");
    DIFF_PPU_FIELD(obp1, "%u");
    DIFF_PPU_FIELD(wy, "%u");
    DIFF_PPU_FIELD(wx, "%u");
    DIFF_PPU_FIELD(bgpi, "%u");
    DIFF_PPU_FIELD(obpi, "%u");
    DIFF_PPU_FIELD(opri, "%u");
    DIFF_PPU_FIELD(latched_lcdc, "%u");
    DIFF_PPU_FIELD(latched_scy, "%u");
    DIFF_PPU_FIELD(latched_scx, "%u");
    DIFF_PPU_FIELD(latched_bgp, "%u");
    DIFF_PPU_FIELD(latched_obp0, "%u");
    DIFF_PPU_FIELD(latched_obp1, "%u");
    DIFF_PPU_FIELD(latched_wy, "%u");
    DIFF_PPU_FIELD(latched_wx, "%u");
    DIFF_PPU_FIELD(stat_irq_state, "%u");
    DIFF_PPU_FIELD(mode, "%u");
    DIFF_PPU_FIELD(mode_cycles, "%u");
    DIFF_PPU_FIELD(window_line, "%u");
    DIFF_PPU_FIELD(window_triggered, "%u");
    DIFF_PPU_FIELD(scanline_draw_cycles, "%u");
    DIFF_PPU_FIELD(scanline_hblank_cycles, "%u");
    DIFF_PPU_FIELD(scanline_sprite_count, "%u");
    DIFF_PPU_FIELD(render_x, "%d");
    DIFF_PPU_FIELD(win_rendered_line, "%u");
    DIFF_PPU_FIELD(scanline_sprite_list_count, "%d");
    DIFF_PPU_FIELD(sprite_list_built, "%u");
    DIFF_PPU_FIELD(frame_ready, "%u");

#undef DIFF_PPU_FIELD

    if (!compare_memory) {
        return true;
    }

    if (!gb_diff_compare_region("PPU framebuffer",
                                generated->framebuffer,
                                interpreted->framebuffer,
                                sizeof(generated->framebuffer),
                                message,
                                message_size)) {
        return false;
    }

    if (!gb_diff_compare_region("PPU color framebuffer",
                                (const uint8_t*)generated->color_framebuffer,
                                (const uint8_t*)interpreted->color_framebuffer,
                                sizeof(generated->color_framebuffer),
                                message,
                                message_size)) {
        return false;
    }

    if (!gb_diff_compare_region("PPU BG palette RAM",
                                generated->bg_palette_ram,
                                interpreted->bg_palette_ram,
                                sizeof(generated->bg_palette_ram),
                                message,
                                message_size)) {
        return false;
    }

    if (!gb_diff_compare_region("PPU OBJ palette RAM",
                                generated->obj_palette_ram,
                                interpreted->obj_palette_ram,
                                sizeof(generated->obj_palette_ram),
                                message,
                                message_size)) {
        return false;
    }

    return gb_diff_compare_region("PPU RGB framebuffer",
                                  (const uint8_t*)generated->rgb_framebuffer,
                                  (const uint8_t*)interpreted->rgb_framebuffer,
                                  sizeof(generated->rgb_framebuffer),
                                  message,
                                  message_size);
}

static bool gb_diff_compare_contexts(const GBContext* generated,
                                     const GBContext* interpreted,
                                     bool compare_memory,
                                     char* message,
                                     size_t message_size) {
#define DIFF_FIELD(field, fmt) \
    do { \
        if (generated->field != interpreted->field) { \
            snprintf(message, message_size, \
                     "%s mismatch: " fmt " != " fmt, \
                     #field, generated->field, interpreted->field); \
            return false; \
        } \
    } while (0)

    DIFF_FIELD(a, "%u");
    DIFF_FIELD(b, "%u");
    DIFF_FIELD(c, "%u");
    DIFF_FIELD(d, "%u");
    DIFF_FIELD(e, "%u");
    DIFF_FIELD(h, "%u");
    DIFF_FIELD(l, "%u");
    DIFF_FIELD(sp, "%u");
    DIFF_FIELD(pc, "%u");
    DIFF_FIELD(f_z, "%u");
    DIFF_FIELD(f_n, "%u");
    DIFF_FIELD(f_h, "%u");
    DIFF_FIELD(f_c, "%u");
    DIFF_FIELD(ime, "%u");
    DIFF_FIELD(ime_pending, "%u");
    DIFF_FIELD(halted, "%u");
    DIFF_FIELD(stopped, "%u");
    DIFF_FIELD(stop_mode_active, "%u");
    DIFF_FIELD(halt_bug, "%u");
    DIFF_FIELD(cgb_double_speed, "%u");
    DIFF_FIELD(rom_bank, "%u");
    DIFF_FIELD(ram_bank, "%u");
    DIFF_FIELD(wram_bank, "%u");
    DIFF_FIELD(vram_bank, "%u");
    DIFF_FIELD(config.model, "%u");
    DIFF_FIELD(config.cgb_compatibility_mode, "%u");
    DIFF_FIELD(mbc_type, "%u");
    DIFF_FIELD(ram_enabled, "%u");
    DIFF_FIELD(mbc_mode, "%u");
    DIFF_FIELD(rom_bank_upper, "%u");
    DIFF_FIELD(rtc_mode, "%u");
    DIFF_FIELD(rtc_reg, "%u");
    DIFF_FIELD(cycles, "%u");
    DIFF_FIELD(frame_cycles, "%u");
    DIFF_FIELD(last_sync_cycles, "%u");
    DIFF_FIELD(frame_done, "%u");
    DIFF_FIELD(div_counter, "%u");
    DIFF_FIELD(tima_reload_pending, "%u");
    DIFF_FIELD(serial_cycles_remaining, "%d");
    DIFF_FIELD(serial_transfer.active, "%u");
    DIFF_FIELD(serial_transfer.fast_clock, "%u");
    DIFF_FIELD(serial_transfer.cycles_remaining, "%u");
    DIFF_FIELD(serial_transfer.deferred, "%u");
    DIFF_FIELD(serial_transfer.slave_armed, "%u");
    DIFF_FIELD(serial_transfer.slave_outgoing, "%u");
    DIFF_FIELD(last_joypad, "%u");
    DIFF_FIELD(dma.active, "%u");
    DIFF_FIELD(dma.source_high, "%u");
    DIFF_FIELD(dma.progress, "%u");
    DIFF_FIELD(dma.cycles_remaining, "%u");
    DIFF_FIELD(hdma.source, "%u");
    DIFF_FIELD(hdma.dest, "%u");
    DIFF_FIELD(hdma.blocks_remaining, "%u");
    DIFF_FIELD(hdma.active, "%u");
    DIFF_FIELD(hdma.hblank_mode, "%u");
    DIFF_FIELD(rtc.s, "%u");
    DIFF_FIELD(rtc.m, "%u");
    DIFF_FIELD(rtc.h, "%u");
    DIFF_FIELD(rtc.dl, "%u");
    DIFF_FIELD(rtc.dh, "%u");
    DIFF_FIELD(rtc.latched_s, "%u");
    DIFF_FIELD(rtc.latched_m, "%u");
    DIFF_FIELD(rtc.latched_h, "%u");
    DIFF_FIELD(rtc.latched_dl, "%u");
    DIFF_FIELD(rtc.latched_dh, "%u");
    DIFF_FIELD(rtc.latch_state, "%u");
    DIFF_FIELD(rtc.last_time, "%" PRIu64);
    DIFF_FIELD(rtc.active, "%u");

#undef DIFF_FIELD

    if (gb_diff_pack_flags(generated) != gb_diff_pack_flags(interpreted)) {
        snprintf(message, message_size,
                 "flags mismatch: %02X != %02X",
                 gb_diff_pack_flags(generated),
                 gb_diff_pack_flags(interpreted));
        return false;
    }

    if (!gb_diff_compare_ppu(generated, interpreted, compare_memory, message, message_size)) {
        return false;
    }

    if (generated->apu == NULL || interpreted->apu == NULL) {
        if (generated->apu != interpreted->apu) {
            snprintf(message, message_size, "APU presence mismatch");
            return false;
        }
    } else {
        /* Compare full guest-architectural APU state (channels, envelope, LFSR,
         * wave RAM, frame sequencer) via the co-sim hash — NOT just the mixed
         * output sample, which was the pre-co-sim blind spot. */
        uint64_t generated_apu = gb_audio_cosim_hash(generated->apu);
        uint64_t interpreted_apu = gb_audio_cosim_hash(interpreted->apu);
        if (generated_apu != interpreted_apu) {
            snprintf(message, message_size,
                     "APU internal state mismatch: %016llX != %016llX",
                     (unsigned long long)generated_apu,
                     (unsigned long long)interpreted_apu);
            return false;
        }
    }

    if (!compare_memory) {
        return true;
    }

    if (generated->eram_size != interpreted->eram_size) {
        snprintf(message, message_size,
                 "eram_size mismatch: %zu != %zu",
                 generated->eram_size, interpreted->eram_size);
        return false;
    }

    if (!gb_diff_compare_region("WRAM",
                                generated->wram,
                                interpreted->wram,
                                DIFF_WRAM_SIZE,
                                message,
                                message_size) ||
        !gb_diff_compare_region("VRAM",
                                generated->vram,
                                interpreted->vram,
                                DIFF_VRAM_TOTAL_SIZE,
                                message,
                                message_size) ||
        !gb_diff_compare_region("OAM",
                                generated->oam,
                                interpreted->oam,
                                DIFF_OAM_SIZE,
                                message,
                                message_size) ||
        !gb_diff_compare_region("HRAM",
                                generated->hram,
                                interpreted->hram,
                                DIFF_HRAM_SIZE,
                                message,
                                message_size) ||
        !gb_diff_compare_region("IO",
                                generated->io,
                                interpreted->io,
                                DIFF_IO_TOTAL_SIZE,
                                message,
                                message_size)) {
        return false;
    }

    if (generated->eram_size > 0 &&
        !gb_diff_compare_region("ERAM",
                                generated->eram,
                                interpreted->eram,
                                generated->eram_size,
                                message,
                                message_size)) {
        return false;
    }

    return true;
}

bool gb_run_differential(GBContext* generated_ctx,
                         GBContext* interpreted_ctx,
                         const GBDifferentialOptions* options,
                         GBDifferentialResult* result) {
    GBDifferentialOptions effective = {
        .max_steps = 10000,
        .max_frames = 0,
        .log_interval = 1000,
        .compare_memory = true,
        .log_fallbacks = false,
        .fail_on_fallback = false,
        .input_script = NULL,
    };

    if (options != NULL) {
        effective = *options;
        if (effective.max_steps == 0 && effective.max_frames == 0) effective.max_steps = 10000;
    }

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->matched = false;
    }

    GBDiffInputScript input_script;
    char setup_message[256] = {0};
    if (!gb_diff_parse_input_script(effective.input_script,
                                    &input_script,
                                    setup_message,
                                    sizeof(setup_message))) {
        if (result != NULL) {
            snprintf(result->message, sizeof(result->message), "%s", setup_message);
        }
        fprintf(stderr, "[DIFF] %s\n", setup_message);
        return false;
    }

    uint64_t saved_instruction_count = gbrt_instruction_count;
    uint64_t saved_instruction_limit = gbrt_instruction_limit;
    void* saved_generated_joypad = generated_ctx->joypad;
    void* saved_interpreted_joypad = interpreted_ctx->joypad;
    gbrt_instruction_count = 0;
    gbrt_instruction_limit = 0;

    GBJoypadState generated_joypad = {.dpad = 0xFF, .buttons = 0xFF};
    GBJoypadState interpreted_joypad = {.dpad = 0xFF, .buttons = 0xFF};
    if (effective.input_script != NULL && *effective.input_script != '\0') {
        generated_ctx->joypad = &generated_joypad;
        interpreted_ctx->joypad = &interpreted_joypad;
        gb_diff_apply_input_state(&input_script, generated_ctx, &generated_joypad);
        gb_diff_apply_input_state(&input_script, interpreted_ctx, &interpreted_joypad);
    }

    uint64_t frames_completed = 0;
    uint64_t executed_steps = 0;
    uint8_t last_logged_fallback_bank = 0xFF;
    uint16_t last_logged_fallback_addr = 0xFFFF;
    bool unlimited_steps = (effective.max_steps == 0);
    uint64_t step_limit = effective.max_steps;
    for (uint64_t step = 0; unlimited_steps || step < step_limit; step++) {
        if (effective.input_script != NULL && *effective.input_script != '\0') {
            gb_diff_apply_input_state(&input_script, generated_ctx, &generated_joypad);
            gb_diff_apply_input_state(&input_script, interpreted_ctx, &interpreted_joypad);
        }

        executed_steps = step + 1;
        uint16_t start_pc = generated_ctx->pc;
        uint16_t start_bank = gb_diff_current_bank(generated_ctx);
        uint8_t opcode_bytes[3] = {0};
        gb_diff_read_opcode_bytes(generated_ctx, opcode_bytes);

        gb_debug_step(generated_ctx, GB_EXECUTION_GENERATED);
        gb_debug_step(interpreted_ctx, GB_EXECUTION_INTERPRETER);

        if (generated_ctx->used_dispatch_fallback) {
            if (effective.log_fallbacks &&
                (generated_ctx->dispatch_fallback_bank != last_logged_fallback_bank ||
                 generated_ctx->dispatch_fallback_addr != last_logged_fallback_addr)) {
                fprintf(stderr,
                        "[DIFF] Generated fallback to interpreter at %03X:%04X\n",
                        generated_ctx->dispatch_fallback_bank,
                        generated_ctx->dispatch_fallback_addr);
                if (generated_ctx->dispatch_fallback_addr >= 0x8000) {
                    gb_diff_print_code_bytes(stderr,
                                             "Generated fallback",
                                             generated_ctx,
                                             generated_ctx->dispatch_fallback_addr,
                                             8);
                }
                last_logged_fallback_bank = generated_ctx->dispatch_fallback_bank;
                last_logged_fallback_addr = generated_ctx->dispatch_fallback_addr;
            }
            if (effective.fail_on_fallback) {
                snprintf(setup_message, sizeof(setup_message),
                         "generated fallback to interpreter at %03X:%04X",
                         generated_ctx->dispatch_fallback_bank,
                         generated_ctx->dispatch_fallback_addr);
                if (result != NULL) {
                    result->matched = false;
                    result->steps_completed = step;
                    result->frames_completed = frames_completed;
                    result->mismatch_step = step + 1;
                    result->pc = generated_ctx->dispatch_fallback_addr;
                    result->bank = generated_ctx->dispatch_fallback_bank;
                    snprintf(result->message, sizeof(result->message), "%s", setup_message);
                }
                fprintf(stderr, "[DIFF] %s\n", setup_message);
                generated_ctx->joypad = saved_generated_joypad;
                interpreted_ctx->joypad = saved_interpreted_joypad;
                gbrt_instruction_count = saved_instruction_count;
                gbrt_instruction_limit = saved_instruction_limit;
                return false;
            }
        }

        char message[256];
        if (!gb_diff_compare_contexts(generated_ctx,
                                      interpreted_ctx,
                                      effective.compare_memory,
                                      message,
                                      sizeof(message))) {
            fprintf(stderr,
                    "[DIFF] Mismatch at step %" PRIu64 " "
                    "before %03X:%04X opcode=%02X %02X %02X\n",
                    step + 1,
                    start_bank,
                    start_pc,
                    opcode_bytes[0],
                    opcode_bytes[1],
                    opcode_bytes[2]);
            fprintf(stderr, "[DIFF] %s\n", message);
            gb_diff_print_state(stderr, "Generated", generated_ctx);
            gb_diff_print_state(stderr, "Interpreter", interpreted_ctx);
            if (start_pc >= 0x8000) {
                gb_diff_print_code_bytes(stderr, "Generated", generated_ctx, start_pc, 8);
                gb_diff_print_code_bytes(stderr, "Interpreter", interpreted_ctx, start_pc, 8);
            }

            if (result != NULL) {
                result->matched = false;
                result->steps_completed = step;
                result->frames_completed = frames_completed;
                result->mismatch_step = step + 1;
                result->pc = start_pc;
                result->bank = start_bank;
                snprintf(result->message, sizeof(result->message), "%s", message);
            }

            generated_ctx->joypad = saved_generated_joypad;
            interpreted_ctx->joypad = saved_interpreted_joypad;
            gbrt_instruction_count = saved_instruction_count;
            gbrt_instruction_limit = saved_instruction_limit;
            return false;
        }

        if (generated_ctx->frame_done && interpreted_ctx->frame_done) {
            frames_completed++;
            gb_reset_frame(generated_ctx);
            gb_reset_frame(interpreted_ctx);

            if (effective.input_script != NULL && *effective.input_script != '\0') {
                input_script.frame_index++;
                gb_diff_apply_input_state(&input_script, generated_ctx, &generated_joypad);
                gb_diff_apply_input_state(&input_script, interpreted_ctx, &interpreted_joypad);
            }

            if (effective.max_frames > 0 && frames_completed >= effective.max_frames) {
                if (result != NULL) {
                    result->matched = true;
                    result->steps_completed = step + 1;
                    result->frames_completed = frames_completed;
                    result->mismatch_step = 0;
                    result->pc = generated_ctx->pc;
                    result->bank = gb_diff_current_bank(generated_ctx);
                    snprintf(result->message, sizeof(result->message), "matched");
                }
                fprintf(stderr,
                        "[DIFF] Matched generated and interpreter execution for %" PRIu64 " steps / %" PRIu64 " frames\n",
                        step + 1,
                        frames_completed);
                generated_ctx->joypad = saved_generated_joypad;
                interpreted_ctx->joypad = saved_interpreted_joypad;
                gbrt_instruction_count = saved_instruction_count;
                gbrt_instruction_limit = saved_instruction_limit;
                return true;
            }
        }

        if (effective.log_interval > 0 && ((step + 1) % effective.log_interval) == 0) {
            if (unlimited_steps) {
                fprintf(stderr,
                        "[DIFF] Matched %" PRIu64 " steps at %03X:%04X (frames=%" PRIu64 ")\n",
                        step + 1,
                        gb_diff_current_bank(generated_ctx),
                        generated_ctx->pc,
                        frames_completed);
            } else {
                fprintf(stderr,
                        "[DIFF] Matched %" PRIu64 "/%" PRIu64 " steps at %03X:%04X (frames=%" PRIu64 ")\n",
                        step + 1,
                        step_limit,
                        gb_diff_current_bank(generated_ctx),
                        generated_ctx->pc,
                        frames_completed);
            }
        }
    }

    fprintf(stderr,
            "[DIFF] Matched generated and interpreter execution for %" PRIu64 " steps / %" PRIu64 " frames\n",
            executed_steps,
            frames_completed);

    if (result != NULL) {
        result->matched = true;
        result->steps_completed = executed_steps;
        result->frames_completed = frames_completed;
        result->mismatch_step = 0;
        result->pc = generated_ctx->pc;
        result->bank = gb_diff_current_bank(generated_ctx);
        snprintf(result->message, sizeof(result->message), "matched");
    }

    generated_ctx->joypad = saved_generated_joypad;
    interpreted_ctx->joypad = saved_interpreted_joypad;
    gbrt_instruction_count = saved_instruction_count;
    gbrt_instruction_limit = saved_instruction_limit;
    return true;
}

/* ============================================================================
 * Differential co-simulation — first-divergence decision procedure.
 * See COSIM_ORACLE.md. Steps two backends in lockstep, checkpoints the full
 * per-subsystem state hash on the shared T-cycle clock, halts at the first
 * split, and drills with the exact byte/field compare to NAME the field.
 * ========================================================================== */

#define GB_COSIM_RING_CAP 64

typedef struct {
    uint64_t index;      /* checkpoint index */
    uint32_t cycles;     /* ctx->cycles at the checkpoint (shared ruler) */
    uint64_t hash_a;     /* full-state hash of A */
    uint64_t hash_b;     /* full-state hash of B */
    uint16_t pc_a;       /* leader PC (report only) */
    uint16_t pc_b;
    uint16_t bank_a;
    bool split;          /* A and B hashes differed here */
} GBCosimRingEntry;

static const char* gb_cosim_mode_name(GBExecutionMode m) {
    return (m == GB_EXECUTION_GENERATED) ? "generated" : "interpreter";
}

static void gb_cosim_apply_inject(GBContext* ctx, GBCosimInjectTarget target) {
    switch (target) {
        case GB_COSIM_INJECT_WRAM:
            if (ctx->wram) ctx->wram[0x123] ^= 0xFF;
            break;
        case GB_COSIM_INJECT_PPU: {
            GBPPU* p = (GBPPU*)ctx->ppu;
            if (p) p->mode_cycles += 1;
            break;
        }
        case GB_COSIM_INJECT_APU:
            gb_audio_cosim_inject(ctx->apu);
            break;
        case GB_COSIM_INJECT_CPU:
            ctx->b ^= 0x01;
            break;
        case GB_COSIM_INJECT_TIMER:
            ctx->div_counter += 1;
            break;
        case GB_COSIM_INJECT_NONE:
        default:
            break;
    }
}

static void gb_cosim_dump_window(FILE* stream,
                                 const GBCosimRingEntry* ring,
                                 size_t ring_count,
                                 size_t ring_head) {
    size_t n = ring_count < GB_COSIM_RING_CAP ? ring_count : GB_COSIM_RING_CAP;
    fprintf(stream, "[COSIM] window (last %zu checkpoints):\n", n);
    for (size_t k = 0; k < n; k++) {
        /* oldest..newest */
        size_t slot = (ring_head + GB_COSIM_RING_CAP - n + k) % GB_COSIM_RING_CAP;
        const GBCosimRingEntry* e = &ring[slot];
        fprintf(stream,
                "[COSIM]   cp=%-8" PRIu64 " cyc=%-10u A=%016llX B=%016llX pcA=%03X:%04X%s\n",
                e->index, e->cycles,
                (unsigned long long)e->hash_a, (unsigned long long)e->hash_b,
                e->bank_a, e->pc_a, e->split ? "  <-- SPLIT" : "");
    }
}

bool gb_run_cosim(GBContext* ctx_a,
                  GBContext* ctx_b,
                  const GBCosimOptions* options,
                  GBCosimResult* result) {
    GBCosimOptions opt = {
        .mode_a = GB_EXECUTION_GENERATED,
        .mode_b = GB_EXECUTION_INTERPRETER,
        .checkpoint_stride = 456,
        .max_frames = 0,
        .max_checkpoints = 0,
        .audit_interval = 0,
        .log_interval = 0,
        .inject_target = GB_COSIM_INJECT_NONE,
        .inject_at_checkpoint = 0,
        .input_script = NULL,
    };
    if (options != NULL) {
        opt = *options;
        if (opt.checkpoint_stride == 0) opt.checkpoint_stride = 456;
    }

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->mismatch_subsystem = -1;
    }

    GBDiffInputScript input_script;
    char setup_message[256] = {0};
    if (!gb_diff_parse_input_script(opt.input_script, &input_script,
                                    setup_message, sizeof(setup_message))) {
        if (result != NULL) snprintf(result->message, sizeof(result->message), "%s", setup_message);
        fprintf(stderr, "[COSIM] %s\n", setup_message);
        return false;
    }
    bool scripted = (opt.input_script != NULL && *opt.input_script != '\0');

    uint64_t saved_instruction_count = gbrt_instruction_count;
    uint64_t saved_instruction_limit = gbrt_instruction_limit;
    bool saved_interp_logging = gbrt_interp_fallback_logging;
    void* saved_a_joypad = ctx_a->joypad;
    void* saved_b_joypad = ctx_b->joypad;
    gbrt_instruction_count = 0;
    gbrt_instruction_limit = 0;
    /* In interpreter mode gb_interpret is the per-instruction hot path; its
     * fallback logging (fflush + debug-server ping every entry) would dominate a
     * long attract run. The cosim doesn't need it — the ring + gates report. */
    gbrt_interp_fallback_logging = false;

    GBJoypadState joypad_a = {.dpad = 0xFF, .buttons = 0xFF};
    GBJoypadState joypad_b = {.dpad = 0xFF, .buttons = 0xFF};
    if (scripted) {
        ctx_a->joypad = &joypad_a;
        ctx_b->joypad = &joypad_b;
        gb_diff_apply_input_state(&input_script, ctx_a, &joypad_a);
        gb_diff_apply_input_state(&input_script, ctx_b, &joypad_b);
    }

    static GBCosimRingEntry ring[GB_COSIM_RING_CAP];
    size_t ring_head = 0;   /* next write slot */
    size_t ring_count = 0;

    uint64_t checkpoint_index = 0;
    uint64_t frames_completed = 0;
    uint32_t next_cp_cycles = opt.checkpoint_stride;
    uint64_t chain_hash = 1469598103934665603ULL; /* FNV offset — regression pin over A */
    bool matched = true;

    fprintf(stderr,
            "[COSIM] start pair=(%s vs %s) stride=%u audit=%" PRIu64 " inject=%d@%" PRIu64 "\n",
            gb_cosim_mode_name(opt.mode_a), gb_cosim_mode_name(opt.mode_b),
            opt.checkpoint_stride, opt.audit_interval,
            (int)opt.inject_target, opt.inject_at_checkpoint);

    for (;;) {
        if (scripted) {
            gb_diff_apply_input_state(&input_script, ctx_a, &joypad_a);
            gb_diff_apply_input_state(&input_script, ctx_b, &joypad_b);
        }

        gb_debug_step(ctx_a, opt.mode_a);
        gb_debug_step(ctx_b, opt.mode_b);

        /* Frame accounting mirrors gb_run_differential. */
        if (ctx_a->frame_done && ctx_b->frame_done) {
            frames_completed++;
            gb_reset_frame(ctx_a);
            gb_reset_frame(ctx_b);
            if (scripted) {
                input_script.frame_index++;
                gb_diff_apply_input_state(&input_script, ctx_a, &joypad_a);
                gb_diff_apply_input_state(&input_script, ctx_b, &joypad_b);
            }
            if (opt.max_frames > 0 && frames_completed >= opt.max_frames) {
                break;
            }
        }

        /* Checkpoint on the shared T-cycle clock. Both backends hold equal
         * ctx->cycles per step until the true divergence, so they cross the
         * stride boundary at the same step. */
        if (ctx_a->cycles < next_cp_cycles) {
            continue;
        }
        next_cp_cycles += opt.checkpoint_stride;

        /* Gate 3: perturb A at the requested checkpoint so the tool MUST halt. */
        if (opt.inject_target != GB_COSIM_INJECT_NONE &&
            checkpoint_index == opt.inject_at_checkpoint) {
            gb_cosim_apply_inject(ctx_a, opt.inject_target);
            fprintf(stderr, "[COSIM] injected fault target=%d at checkpoint %" PRIu64 "\n",
                    (int)opt.inject_target, checkpoint_index);
        }

        CosimSubHashes sub_a, sub_b;
        uint64_t hash_a = gb_cosim_state_hash(ctx_a, &sub_a);
        uint64_t hash_b = gb_cosim_state_hash(ctx_b, &sub_b);
        bool split = (hash_a != hash_b);

        /* Push ring entry (bounded reporting). */
        ring[ring_head] = (GBCosimRingEntry){
            .index = checkpoint_index,
            .cycles = ctx_a->cycles,
            .hash_a = hash_a,
            .hash_b = hash_b,
            .pc_a = ctx_a->pc,
            .pc_b = ctx_b->pc,
            .bank_a = gb_diff_current_bank(ctx_a),
            .split = split,
        };
        ring_head = (ring_head + 1) % GB_COSIM_RING_CAP;
        if (ring_count < GB_COSIM_RING_CAP) ring_count++;

        /* Chain hash over A's trajectory — the regression baseline pin. */
        for (int b = 0; b < 8; b++)
            chain_hash = (chain_hash ^ ((hash_a >> (8 * b)) & 0xFF)) * 1099511628211ULL;

        if (split) {
            /* Localize: first differing subsystem. */
            int subsystem = -1;
            for (int i = 0; i < GB_COSIM_SUBHASH_COUNT; i++) {
                if (gb_cosim_subhash_by_index(&sub_a, i) !=
                    gb_cosim_subhash_by_index(&sub_b, i)) {
                    subsystem = i;
                    break;
                }
            }

            /* Drill: exact byte/field compare NAMES the divergent field. */
            char message[256] = {0};
            if (gb_diff_compare_contexts(ctx_a, ctx_b, true, message, sizeof(message))) {
                /* Hash split but exact-compare clean => the hash covers a field
                 * the exact-compare does not. Report the subsystem; not a tool
                 * lie, but a surface-coverage gap to close in the exact path. */
                snprintf(message, sizeof(message),
                         "hash split in subsystem '%s' not localized by exact compare",
                         subsystem >= 0 ? gb_cosim_subhash_name(subsystem) : "?");
            }

            fprintf(stderr,
                    "[COSIM] FIRST DIVERGENCE at checkpoint %" PRIu64
                    " cyc=%u subsystem=%s\n[COSIM] %s\n",
                    checkpoint_index, ctx_a->cycles,
                    subsystem >= 0 ? gb_cosim_subhash_name(subsystem) : "?",
                    message);
            gb_cosim_dump_window(stderr, ring, ring_count, ring_head);
            gb_diff_print_state(stderr, "A", ctx_a);
            gb_diff_print_state(stderr, "B", ctx_b);

            if (result != NULL) {
                result->matched = false;
                result->checkpoints_completed = checkpoint_index;
                result->frames_completed = frames_completed;
                result->mismatch_checkpoint = checkpoint_index;
                result->mismatch_cycles = ctx_a->cycles;
                result->mismatch_subsystem = subsystem;
                result->chain_hash = chain_hash;
                result->pc_a = ctx_a->pc;
                result->pc_b = ctx_b->pc;
                snprintf(result->message, sizeof(result->message), "%s", message);
            }
            matched = false;
            break;
        }

        /* Gate 4: hash-vs-byte audit — force a full compare even when hashes
         * matched, proving the hash maintenance is faithful to the bytes. */
        if (opt.audit_interval > 0 && (checkpoint_index % opt.audit_interval) == 0) {
            char audit_msg[256] = {0};
            if (!gb_diff_compare_contexts(ctx_a, ctx_b, true, audit_msg, sizeof(audit_msg))) {
                fprintf(stderr,
                        "[COSIM] AUDIT FAILURE at checkpoint %" PRIu64
                        ": hashes matched but bytes differ (%s) — TOOL BUG\n",
                        checkpoint_index, audit_msg);
                if (result != NULL) {
                    result->matched = false;
                    result->mismatch_checkpoint = checkpoint_index;
                    result->mismatch_cycles = ctx_a->cycles;
                    result->chain_hash = chain_hash;
                    snprintf(result->message, sizeof(result->message),
                             "audit failure (hash/byte disagree): %s", audit_msg);
                }
                matched = false;
                break;
            }
        }

        if (opt.log_interval > 0 && (checkpoint_index % opt.log_interval) == 0) {
            fprintf(stderr,
                    "[COSIM] cp=%" PRIu64 " cyc=%u frames=%" PRIu64
                    " chain=%016llX A@%03X:%04X\n",
                    checkpoint_index, ctx_a->cycles, frames_completed,
                    (unsigned long long)chain_hash,
                    gb_diff_current_bank(ctx_a), ctx_a->pc);
        }

        checkpoint_index++;
        if (opt.max_checkpoints > 0 && checkpoint_index >= opt.max_checkpoints) {
            break;
        }
    }

    if (matched) {
        fprintf(stderr,
                "[COSIM] MATCHED %" PRIu64 " checkpoints / %" PRIu64
                " frames; chain=%016llX\n",
                checkpoint_index, frames_completed, (unsigned long long)chain_hash);
        if (result != NULL) {
            result->matched = true;
            result->checkpoints_completed = checkpoint_index;
            result->frames_completed = frames_completed;
            result->mismatch_checkpoint = 0;
            result->chain_hash = chain_hash;
            result->pc_a = ctx_a->pc;
            result->pc_b = ctx_b->pc;
            snprintf(result->message, sizeof(result->message), "matched");
        }
    }

    ctx_a->joypad = saved_a_joypad;
    ctx_b->joypad = saved_b_joypad;
    gbrt_instruction_count = saved_instruction_count;
    gbrt_instruction_limit = saved_instruction_limit;
    gbrt_interp_fallback_logging = saved_interp_logging;
    return matched;
}

bool gb_run_boot_gate(GBContext* lle_ctx,
                      GBContext* hle_ctx,
                      uint64_t cycle_cap,
                      GBCosimResult* result) {
    if (result) {
        memset(result, 0, sizeof(*result));
        result->mismatch_subsystem = -1;
    }
    if (cycle_cap == 0) cycle_cap = 50000000ull; /* generous: DMG boot ~ a few frames */

    uint64_t saved_count = gbrt_instruction_count;
    uint64_t saved_limit = gbrt_instruction_limit;
    bool saved_log = gbrt_interp_fallback_logging;
    gbrt_instruction_count = 0;
    gbrt_instruction_limit = 0;
    gbrt_interp_fallback_logging = false;

    /* Phase 1: run the real boot ROM (BIOS) to the handoff (0xFF50 unmap). */
    /* The BIOS is NOT compiled cart code — generated dispatch keys on PC and
     * would run the cartridge's 0x0000, ignoring the boot-ROM overlay (which
     * only redirects gb_read8). The interpreter fetches opcodes via gb_read8,
     * so it honors the overlay and actually executes the BIOS. */
    uint16_t prev_pc = lle_ctx->pc;
    int escapes_logged = 0;
    while (lle_ctx->boot_rom_active && lle_ctx->cycles < cycle_cap) {
        gb_debug_step(lle_ctx, GB_EXECUTION_INTERPRETER);
        /* The DMG boot ROM never executes outside 0x0000-0x00FF until it writes
         * 0xFF50 and falls through to 0x0100. If pc leaves BIOS space while the
         * BIOS is still mapped, that's a control-flow escape (the bug). */
        if (escapes_logged < 8 && lle_ctx->boot_rom_active && lle_ctx->pc >= 0x0100 &&
            prev_pc < 0x0100) {
            uint16_t sp = lle_ctx->sp;
            fprintf(stderr,
                    "[BOOTGATE] ESCAPE #%d: pc %04X -> %04X (BIOS still mapped) sp=%04X "
                    "[sp]=%02X%02X ime=%u IF=%02X IE=%02X\n",
                    escapes_logged + 1, prev_pc, lle_ctx->pc, sp,
                    gb_read8(lle_ctx, (uint16_t)(sp + 1)), gb_read8(lle_ctx, sp),
                    lle_ctx->ime, lle_ctx->io[0x0F], lle_ctx->io[0x80]);
            escapes_logged++;
        }
        prev_pc = lle_ctx->pc;
    }

    gbrt_instruction_count = saved_count;
    gbrt_instruction_limit = saved_limit;
    gbrt_interp_fallback_logging = saved_log;

    if (lle_ctx->boot_rom_active) {
        fprintf(stderr,
                "[BOOTGATE] boot ROM did NOT hand off within %" PRIu64 " cycles (pc=%04X)\n",
                cycle_cap, lle_ctx->pc);
        if (result) {
            result->matched = false;
            snprintf(result->message, sizeof(result->message), "boot did not hand off");
        }
        return false;
    }

    fprintf(stderr,
            "[BOOTGATE] boot ROM handed off at cycle %u: LLE pc=%04X sp=%04X vs HLE pc=%04X sp=%04X\n",
            lle_ctx->cycles, lle_ctx->pc, lle_ctx->sp, hle_ctx->pc, hle_ctx->sp);

    /* PASS/FAIL surface: the CPU handoff registers + IME. These are the exact
     * architectural state the cartridge inherits at 0x0100, and are what our HLE
     * skip-state hardcodes — so they MUST match a faithful BIOS. */
    int mism = 0;
#define BG_CMP(expr_l, expr_h, name, fmt) \
    do { if ((expr_l) != (expr_h)) { \
        fprintf(stderr, "[BOOTGATE]  DIFF %-12s LLE=" fmt "  HLE=" fmt "\n", name, (expr_l), (expr_h)); \
        mism++; } } while (0)
    BG_CMP(lle_ctx->pc, hle_ctx->pc, "pc", "%04X");
    BG_CMP(lle_ctx->sp, hle_ctx->sp, "sp", "%04X");
    BG_CMP(lle_ctx->a, hle_ctx->a, "a", "%02X");
    BG_CMP(gb_diff_pack_flags(lle_ctx), gb_diff_pack_flags(hle_ctx), "f", "%02X");
    BG_CMP(lle_ctx->b, hle_ctx->b, "b", "%02X");
    BG_CMP(lle_ctx->c, hle_ctx->c, "c", "%02X");
    BG_CMP(lle_ctx->d, hle_ctx->d, "d", "%02X");
    BG_CMP(lle_ctx->e, hle_ctx->e, "e", "%02X");
    BG_CMP(lle_ctx->h, hle_ctx->h, "h", "%02X");
    BG_CMP(lle_ctx->l, hle_ctx->l, "l", "%02X");
    BG_CMP(lle_ctx->ime, hle_ctx->ime, "ime", "%u");
#undef BG_CMP

    /* Informational (NOT pass/fail): the BIOS hands off mid-PPU-frame, and some
     * device state lives outside ctx->io (the APU registers, for one), so exact
     * parity with a freshly-reset HLE context is not expected here. */
    fprintf(stderr, "[BOOTGATE] (info) DIV: LLE div_counter=%04X vs HLE=%04X%s\n",
            lle_ctx->div_counter, hle_ctx->div_counter,
            lle_ctx->div_counter == hle_ctx->div_counter ? "" :
            " — boot-timing fidelity signal; SameBoy (Phase B) arbitrates whether"
            " our boot timing or the HLE DIV constant is off");
    uint64_t apu_l = lle_ctx->apu ? gb_audio_cosim_hash(lle_ctx->apu) : 0;
    uint64_t apu_h = hle_ctx->apu ? gb_audio_cosim_hash(hle_ctx->apu) : 0;
    fprintf(stderr, "[BOOTGATE] (info) APU state hash: LLE=%016llX HLE=%016llX %s\n",
            (unsigned long long)apu_l, (unsigned long long)apu_h,
            apu_l == apu_h ? "(match)" : "(differ — expected: BIOS chime vs HLE defaults)");

    fprintf(stderr,
            "[BOOTGATE] %d CPU-handoff diff(s). %s\n",
            mism,
            mism == 0 ? "HLE post-boot CPU state MATCHES the real BIOS handoff."
                      : "HLE CPU state differs from the real BIOS handoff (see DIFFs above).");

    if (result) {
        result->matched = (mism == 0);
        result->mismatch_cycles = lle_ctx->cycles;
        result->pc_a = lle_ctx->pc;
        result->pc_b = hle_ctx->pc;
        snprintf(result->message, sizeof(result->message), "%d CPU-handoff diffs", mism);
    }
    return mism == 0;
}

/* ============================================================================
 * Cross-oracle co-simulation vs embedded SameBoy. Only compiled when the
 * runtime is built with -DGBC_COSIM_SAMEBOY (GBC_HAVE_SAMEBOY). See COSIM_ORACLE.md.
 * ========================================================================== */
#ifdef GBC_HAVE_SAMEBOY
#include "sameboy_oracle.h"
#include "cosim_neutral.h"

/* Advance an LLE recomp context (interpreter — the BIOS is not compiled, and the
 * interpreter exercises the shared device/timing models the oracle judges) to
 * the BIOS handoff (boot_rom_active clears). Returns the handoff T-cycle. */
static uint32_t gb_recomp_run_to_handoff(GBContext* ctx, uint64_t cap) {
    while (ctx->boot_rom_active && ctx->cycles < cap) {
        gb_debug_step(ctx, GB_EXECUTION_INTERPRETER);
    }
    return ctx->cycles;
}

void gb_sameboy_selfcheck(const uint8_t* boot_rom, size_t boot_rom_size,
                          const uint8_t* rom, size_t rom_size, int is_cgb) {
    SBOracle* o = sb_oracle_create(is_cgb ? SB_MODEL_CGB : SB_MODEL_DMG,
                                   boot_rom, boot_rom_size, rom, rom_size);
    if (!o) { fprintf(stderr, "[SBORACLE] sb_oracle_create failed\n"); return; }

    /* Run to the BIOS handoff. */
    uint64_t cap = 60000000ull;
    while (!sb_oracle_boot_done(o) && sb_oracle_tcycles(o) < cap) {
        sb_oracle_run_to_tcycle(o, sb_oracle_tcycles(o) + 1024);
    }
    fprintf(stderr,
            "[SBORACLE] SameBoy boot handoff: tcycle=%" PRIu64 " pc=%04X DIV(FF04)=%02X LY(FF44)=%02X\n",
            sb_oracle_tcycles(o), sb_oracle_pc(o),
            sb_oracle_read(o, 0xFF04), sb_oracle_read(o, 0xFF44));

    /* Run ~60 more frames and report a neutral hash + a couple of regs. */
    sb_oracle_run_to_tcycle(o, sb_oracle_tcycles(o) + 60ull * 70224ull);
    GBNeutralSubHashes sub;
    uint64_t top = sb_oracle_neutral_hash(o, &sub);
    fprintf(stderr,
            "[SBORACLE] +60 frames: tcycle=%" PRIu64 " pc=%04X neutral=%016llX cpu=%016llX wram=%016llX\n",
            sb_oracle_tcycles(o), sb_oracle_pc(o),
            (unsigned long long)top, (unsigned long long)sub.cpu, (unsigned long long)sub.wram);
    sb_oracle_destroy(o);
}

bool gb_run_sameboy_cosim(GBContext* recomp_lle_ctx,
                          const uint8_t* boot_rom, size_t boot_rom_size,
                          const GBCosimOptions* options, GBCosimResult* result) {
    GBCosimOptions opt = { .checkpoint_stride = 456, .max_checkpoints = 0, .log_interval = 0 };
    if (options) { opt = *options; if (opt.checkpoint_stride == 0) opt.checkpoint_stride = 456; }
    if (result) { memset(result, 0, sizeof(*result)); result->mismatch_subsystem = -1; }

    int is_cgb = (recomp_lle_ctx->config.model == GB_MODEL_CGB);
    SBOracle* o = sb_oracle_create(is_cgb ? SB_MODEL_CGB : SB_MODEL_DMG,
                                   boot_rom, boot_rom_size,
                                   recomp_lle_ctx->rom, recomp_lle_ctx->rom_size);
    if (!o) {
        fprintf(stderr, "[SBORACLE] sb_oracle_create failed\n");
        if (result) snprintf(result->message, sizeof(result->message), "oracle create failed");
        return false;
    }

    bool saved_log = gbrt_interp_fallback_logging;
    gbrt_interp_fallback_logging = false;

    /* Boot both through the real BIOS; report each side's handoff (the DIV/
     * timing arbitration). */
    uint32_t r_handoff = gb_recomp_run_to_handoff(recomp_lle_ctx, 60000000ull);
    while (!sb_oracle_boot_done(o) && sb_oracle_tcycles(o) < 60000000ull) {
        sb_oracle_run_to_tcycle(o, sb_oracle_tcycles(o) + 1024);
    }
    uint64_t o_handoff = sb_oracle_tcycles(o);
    fprintf(stderr,
            "[SBORACLE] boot handoff: recomp tcycle=%u DIV=%02X | SameBoy tcycle=%" PRIu64 " DIV=%02X%s\n",
            r_handoff, (uint8_t)(recomp_lle_ctx->div_counter >> 8),
            o_handoff, sb_oracle_read(o, 0xFF04),
            (r_handoff == o_handoff) ? "" : "  <-- boot-timing divergence");

    /* Forward lockstep on RELATIVE post-handoff cycles (absorbs any boot-timing
     * offset): compare the neutral architectural hash at each stride. */
    uint64_t checkpoint = 0;
    bool matched = true;
    uint32_t next_rel = opt.checkpoint_stride;
    while (opt.max_checkpoints == 0 || checkpoint < opt.max_checkpoints) {
        /* advance recomp by stride (interpreter) */
        uint32_t target_r = r_handoff + next_rel;
        while (recomp_lle_ctx->cycles < target_r) {
            gb_debug_step(recomp_lle_ctx, GB_EXECUTION_INTERPRETER);
        }
        sb_oracle_run_to_tcycle(o, o_handoff + next_rel);

        GBNeutralSubHashes rs, os;
        uint64_t rh = gb_cosim_neutral_hash(recomp_lle_ctx, &rs);
        uint64_t oh = sb_oracle_neutral_hash(o, &os);
        if (rh != oh) {
            int sub = -1;
            for (int i = 0; i < GB_NEUTRAL_SUBHASH_COUNT; i++) {
                if (gb_neutral_subhash_by_index(&rs, i) != gb_neutral_subhash_by_index(&os, i)) { sub = i; break; }
            }
            fprintf(stderr,
                    "[SBORACLE] FIRST DIVERGENCE at rel-cycle %u (recomp cyc=%u, SameBoy cyc=%" PRIu64
                    ") subsystem=%s\n[SBORACLE]  recomp pc=%04X DIV=%02X LY=%02X | SameBoy pc=%04X DIV=%02X LY=%02X\n",
                    next_rel, recomp_lle_ctx->cycles, sb_oracle_tcycles(o),
                    sub >= 0 ? gb_neutral_subhash_name(sub) : "?",
                    recomp_lle_ctx->pc, (uint8_t)(recomp_lle_ctx->div_counter >> 8),
                    ((GBPPU*)recomp_lle_ctx->ppu)->ly,
                    sb_oracle_pc(o), sb_oracle_read(o, 0xFF04), sb_oracle_read(o, 0xFF44));
            if (result) {
                result->matched = false;
                result->mismatch_checkpoint = checkpoint;
                result->mismatch_cycles = recomp_lle_ctx->cycles;
                result->mismatch_subsystem = sub;
                snprintf(result->message, sizeof(result->message),
                         "neutral divergence in %s at rel-cycle %u",
                         sub >= 0 ? gb_neutral_subhash_name(sub) : "?", next_rel);
            }
            matched = false;
            break;
        }
        checkpoint++;
        next_rel += opt.checkpoint_stride;
        if (opt.log_interval && (checkpoint % opt.log_interval) == 0) {
            fprintf(stderr, "[SBORACLE] matched %" PRIu64 " checkpoints (rel-cycle %u)\n",
                    checkpoint, next_rel);
        }
    }

    if (matched) {
        fprintf(stderr, "[SBORACLE] MATCHED %" PRIu64 " checkpoints vs SameBoy\n", checkpoint);
        if (result) { result->matched = true; result->checkpoints_completed = checkpoint;
                      snprintf(result->message, sizeof(result->message), "matched vs SameBoy"); }
    }
    gbrt_interp_fallback_logging = saved_log;
    sb_oracle_destroy(o);
    return matched;
}
#endif /* GBC_HAVE_SAMEBOY */
