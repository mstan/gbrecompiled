/**
 * @file gbrt.h
 * @brief GameBoy Runtime Library
 * 
 * This runtime library provides the execution environment for recompiled
 * GameBoy games. It implements memory access, CPU context, and hardware
 * emulation needed by the generated C code.
 */

#ifndef GBRT_H
#define GBRT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/* ============================================================================
 * Configuration
 * ========================================================================== */

/**
 * @brief GameBoy model selection
 */
typedef enum {
    GB_MODEL_DMG,   /**< Original GameBoy (DMG) */
    GB_MODEL_CGB,   /**< GameBoy Color (CGB) */
    GB_MODEL_SGB,   /**< Super GameBoy */
} GBModel;

/**
 * @brief User's preferred hardware mode for a given cart.
 *
 * Set on a GBContext by the platform before gb_context_load_rom runs.
 * The runtime consults it instead of unconditionally downgrading dual-
 * mode (SGB+CGB) carts to DMG.
 *
 *   AUTO — pick per cart:
 *     • DMG-only carts that support SGB → DMG hardware + SGB engine
 *     • Dual-mode SGB+CGB carts          → CGB hardware, SGB engine off
 *     • CGB-only carts                    → CGB hardware
 *
 *   DMG  — force monochrome, disable SGB engine.
 *   SGB  — force DMG hardware + SGB engine (works on any SGB cart).
 *   CGB  — force CGB hardware (silently falls back to AUTO if the cart
 *          doesn't support CGB).
 */
typedef enum {
    GB_HARDWARE_MODE_AUTO = 0,
    GB_HARDWARE_MODE_DMG,
    GB_HARDWARE_MODE_SGB,
    GB_HARDWARE_MODE_CGB,
    /* GBA mode behaves identically to CGB at the PPU/MBC level — it's
     * still a CGB cart running. The only difference is that the real
     * GBA boots a GBC cart with CPU register B = 0x01 (vs CGB's 0x00),
     * which a handful of carts read to gate GBA-specific behavior
     * (e.g. Pokemon Crystal blocks the mobile adapter on GBA, DKC GBC
     * refuses to run). Picking this mode flips that flag. */
    GB_HARDWARE_MODE_GBA,
} GBHardwareModePref;

/**
 * @brief Runtime configuration
 */
typedef struct {
    GBModel model;
    bool cgb_compatibility_mode; /**< Run CGB hardware in DMG compatibility mode */
    bool cartridge_supports_cgb; /**< Cartridge advertises CGB support */
    bool cartridge_requires_cgb; /**< Cartridge is CGB-only */
    bool cartridge_supports_sgb; /**< Cart byte 0x146 == 0x03 (uses SGB functions) */
    bool enable_bootrom;
    bool enable_audio;
    bool enable_serial;
    bool compiled_halt_bug; /**< Dispatcher has native HALT-bug instruction variants. */
    uint32_t speed_percent; /**< 100 = normal, 200 = 2x, etc */
} GBConfig;

/* ============================================================================
 * Debugging
 * ========================================================================== */

extern bool gbrt_trace_enabled;
extern bool gbrt_log_lcd_transitions;
/* Per-interpreter-entry fallback logging (stderr cap + interp_fallbacks.log).
 * Default true. The co-sim disables it around a run: in interpreter mode
 * gb_interpret is the normal path, so logging every entry dominates long-run
 * wall-clock. Fallbacks are NOT streamed to the debug server; query the
 * always-on hotspot ring (`interp_fallbacks` command) instead. */
extern bool gbrt_interp_fallback_logging;

/** Register the interpreter fallback log so it is flushed and closed at exit. */
void gbrt_register_interp_log(void* file);

/** Note a (bank, addr) interpreter entry site. Returns true the first time a
 *  site is seen, so the caller can flush the log once per distinct site
 *  instead of once per entry. */
bool gbrt_note_interp_log_site(uint8_t bank, uint16_t addr);
extern uint64_t gbrt_instruction_count;
extern uint64_t gbrt_instruction_limit;

extern void (*gbrt_instruction_limit_callback)(void);

typedef struct {
    uint8_t dpad;     /**< Active-low Right, Left, Up, Down bits */
    uint8_t buttons;  /**< Active-low A, B, Select, Start bits */
} GBJoypadState;

typedef enum {
    GB_EXECUTION_GENERATED = 0,
    GB_EXECUTION_INTERPRETER = 1,
} GBExecutionMode;

typedef struct {
    uint64_t max_steps;      /**< Number of scheduler steps to compare */
    uint64_t max_frames;     /**< Number of completed frames to compare (0 disables frame limit) */
    uint64_t log_interval;   /**< Progress log cadence (0 disables progress logs) */
    bool compare_memory;     /**< Compare mutable memory/PPU state on every step */
    bool log_fallbacks;      /**< Log generated-to-interpreter fallback events */
    bool quiet;             /**< Suppress successful per-run differential summaries. */
    bool fail_on_fallback;   /**< Treat generated-to-interpreter fallback as a mismatch */
    const char* input_script;/**< Optional scripted input in frame:buttons:duration or c<cycle>:buttons:duration format */
} GBDifferentialOptions;

typedef struct {
    bool matched;            /**< True if both paths stayed in sync */
    uint64_t steps_completed;/**< Number of completed comparison steps */
    uint64_t frames_completed;/**< Number of completed frame boundaries */
    uint64_t mismatch_step;  /**< Step index of the first mismatch */
    uint16_t pc;             /**< PC at the start of the mismatching step */
    uint16_t bank;           /**< Active ROM bank at the start of the mismatching step */
    char message[256];       /**< Short mismatch description */
} GBDifferentialResult;

#define GBRT_INTERPRETER_HOTSPOT_CAPACITY 16

typedef struct {
    uint8_t valid;           /**< Slot contains a tracked hotspot */
    uint8_t bank;            /**< Bank of the fallback entry point */
    uint16_t addr;           /**< Address of the fallback entry point */
    uint64_t entries;        /**< Number of interpreter entries at this site */
    uint64_t instructions;   /**< Interpreted instructions attributed to this site */
    uint64_t cycles;         /**< Interpreted cycles attributed to this site */
    uint64_t last_frame;     /**< Most recent guest frame that hit this site */
} GBInterpreterHotspot;


/* ============================================================================
 * CPU Context
 * ========================================================================== */

/**
 * @brief Forward declaration
 */
typedef struct GBContext GBContext;

/**
 * @brief Platform callbacks for I/O and rendering
 */
typedef struct {
    void (*on_vblank)(GBContext* ctx, const uint8_t* framebuffer);
    void (*on_audio_sample)(GBContext* ctx, int16_t left, int16_t right);
    uint8_t (*get_joypad)(GBContext* ctx);
    void (*on_serial_byte)(GBContext* ctx, uint8_t byte);
    
    /* Save Data / External RAM */
    bool (*load_battery_ram)(GBContext* ctx, const char* rom_name, void* data, size_t size);
    bool (*save_battery_ram)(GBContext* ctx, const char* rom_name, const void* data, size_t size);
    bool (*load_rtc_data)(GBContext* ctx, const char* rom_name, void* data, size_t size);
    bool (*save_rtc_data)(GBContext* ctx, const char* rom_name, const void* data, size_t size);
} GBPlatformCallbacks;

/**
 * @brief CPU register and state context
 * 
 * This structure is passed to all recompiled functions and contains
 * the current state of the emulated CPU.
 */
typedef struct GBContext {
    /* 8-bit registers */
    union {
        struct { uint8_t f, a; };  /**< AF register pair (little-endian) */
        uint16_t af;
    };
    union {
        struct { uint8_t c, b; };  /**< BC register pair */
        uint16_t bc;
    };
    union {
        struct { uint8_t e, d; };  /**< DE register pair */
        uint16_t de;
    };
    union {
        struct { uint8_t l, h; };  /**< HL register pair */
        uint16_t hl;
    };
    
    /* Stack pointer and program counter */
    uint16_t sp;
    uint16_t pc;
    
    /* Flag bits (unpacked for performance) */
    uint8_t f_z;  /**< Zero flag */
    uint8_t f_n;  /**< Subtract flag */
    uint8_t f_h;  /**< Half-carry flag */
    uint8_t f_c;  /**< Carry flag */
    
    /* Interrupt state */
    uint8_t ime;          /**< Interrupt Master Enable */
    uint8_t ime_pending;  /**< IME will be enabled after next instruction */
    uint8_t halted;       /**< CPU is halted */
    uint8_t stopped;      /**< Scheduler/execution slice requested to stop */
    uint8_t stop_mode_active; /**< CPU is in STOP low-power mode */
    uint8_t halt_bug;     /**< HALT bug: next instruction byte read twice */
    uint8_t single_step_mode; /**< Debug mode: execute at most one instruction */
    uint8_t cgb_double_speed; /**< CGB double-speed mode is enabled */
    uint8_t debug_dispatch_only; /**< gb_debug_step yielded on an interrupt dispatch WITHOUT
                                  *   executing an instruction (co-sim boundary alignment).
                                  *   Transient; not part of architectural state hashes. */
    
    /* OAM DMA state */
    struct {
        uint8_t active;         /**< DMA is in progress (bus blocking) */
        uint8_t pending;        /**< DMA was requested, startup delay in progress */
        uint8_t source_high;    /**< Source address >> 8 */
        uint8_t progress;       /**< Bytes copied (0-159) */
        uint16_t cycles_remaining; /**< Cycles until DMA completes */
        uint8_t startup_delay;  /**< T-cycles before DMA bus blocking starts (2 M-cycles = 8 T-cycles) */
    } dma;

    /* CGB HDMA state */
    struct {
        uint16_t source;        /**< Current source address */
        uint16_t dest;          /**< Current destination address (0x8000-0x9FF0) */
        uint8_t blocks_remaining; /**< Remaining 0x10-byte blocks */
        uint8_t active;         /**< Transfer is currently active */
        uint8_t hblank_mode;    /**< Transfer runs during HBlank only */
    } hdma;

    /* Serial transfer delay */
    int serial_cycles_remaining; /**< Cycles until serial transfer completes (-1 = inactive) */


    /* Current bank numbers */
    uint16_t rom_bank;    /**< Current ROM bank (0x4000-0x7FFF) - 9 bits for MBC5 */
    uint8_t ram_bank;     /**< Current RAM bank */
    uint8_t wram_bank;    /**< Current WRAM bank (CGB only) */
    uint8_t vram_bank;    /**< Current VRAM bank (CGB only) */

    /* Runtime configuration */
    GBConfig config;
    char save_id[64];

    /* User's hardware-mode preference for this cart. Default AUTO; the
     * platform overwrites it (typically with a per-game value from
     * runtime_prefs.ini) between gb_context_create and the recompiled
     * <game>_init call. gb_context_load_rom honors it when deciding
     * the SGB-vs-CGB conflict for dual-mode carts. */
    GBHardwareModePref hardware_mode_pref;

    /* Identify as GBA-running-GBC-cart at boot. PPU/MBC behavior is
     * identical to CGB; only effect is register B = 0x01 (vs CGB's
     * 0x00) at reset. A handful of GBC carts read this to gate
     * GBA-specific behavior (Pokemon Crystal blocks the mobile
     * adapter; DKC GBC refuses to run). Set by gb_context_load_rom
     * when hardware_mode_pref == GBA. */
    bool gba_mode;

    /* CGB BIOS palette override for mono carts running in CGB compat
     * mode. -1 = AUTO (use title-hash LUT, matches real CGB BIOS
     * behavior); 0..11 force one of the 12 canonical color presets:
     *   0 Green (Right)        1 Blue (Left)        2 Brown (Up)
     *   3 Pastel Mix (Down)    4 Dark Green (R+A)   5 Dark Blue (L+A)
     *   6 Red (U+A)            7 Orange (D+A)       8 Inverted (R+B)
     *   9 Grayscale (L+B)     10 Dark Brown (U+B)  11 Yellow (D+B)
     * Set by the platform layer between gb_context_create and the
     * recompiled <game>_init call, like hardware_mode_pref above. */
    int cgb_compat_palette_override;

    /* MBC state */
    uint8_t mbc_type;
    uint8_t ram_enabled;
    uint8_t mbc_mode;       /**< Banking mode for MBC1 (0=ROM, 1=RAM/Advanced) */
    uint8_t rom_bank_upper; /**< MBC1: Upper 2 bits of ROM bank / RAM bank selector */
    uint8_t rtc_mode;       /**< 0=RAM, 1=RTC registers (for 0xA000-0xBFFF) */
    uint8_t rtc_reg;        /**< Selected RTC register (0x08-0x0C) */
    
    /* Timing */
    uint32_t cycles;      /**< Cycles executed */
    uint32_t frame_cycles;/**< Cycles this frame */
    uint32_t last_sync_cycles; /**< Last cycles count synchronized with hardware */
    uint32_t run_cycle_budget; /**< Active gb_run_cycles() slice budget, or 0 when unbounded */
    uint32_t run_cycle_budget_start; /**< Cycle counter at the start of the active slice budget */
    uint8_t  frame_done;  /**< Frame is finished and rendered */
    uint8_t  lcd_off_active; /**< LCDC bit 7 is currently off */
    uint32_t lcd_off_start_cycles; /**< Global cycle when the current LCD-off span started */
    uint32_t lcd_off_start_frame_cycles; /**< Frame-local cycle when the current LCD-off span started */
    uint32_t frame_lcd_off_cycles; /**< Cycles spent with LCD disabled in the current rendered frame */
    uint32_t frame_lcd_transition_count; /**< LCD on/off state changes seen in the current rendered frame */
    uint32_t frame_lcd_off_span_count; /**< LCD-off spans completed in the current rendered frame */
    uint32_t last_lcd_off_span_cycles; /**< Most recent LCD-off span length */
    uint64_t total_lcd_off_cycles; /**< Total cycles spent with LCD disabled */
    uint64_t total_lcd_transition_count; /**< Total LCD on/off state changes */
    uint64_t total_lcd_off_span_count; /**< Total completed LCD-off spans */
    
    /* Timer internal state */
    uint16_t div_counter;   /**< Internal 16-bit divider counter */
    uint8_t tima_reload_pending; /**< TIMA overflow delay: > 0 means reload in N T-cycles */

    /* Serial transfer state */
    struct {
        uint8_t active;      /**< Internal-clock transfer currently in progress */
        uint8_t fast_clock;  /**< CGB fast serial clock is selected */
        uint32_t cycles_remaining; /**< Remaining CPU cycles until completion */
        uint8_t deferred;    /**< Master countdown elapsed; awaiting peer reply (link layer) */
        uint8_t slave_armed; /**< External-clock transfer armed; awaiting peer master byte */
        uint8_t slave_outgoing; /**< Byte captured from SB at the moment slave mode was armed */
    } serial_transfer;
    
    /* Memory pointers */
    uint8_t* rom;         /**< ROM data */
    size_t rom_size;
    uint8_t* eram;        /**< External RAM */
    size_t eram_size;
    uint8_t eram_dirty;   /**< Set when ERAM is written; cleared after flush */
    uint32_t eram_dirty_frame; /**< Frame when ERAM was last dirtied */
    uint8_t* wram;        /**< Work RAM */
    uint8_t* vram;        /**< Video RAM */
    uint8_t* oam;         /**< Object Attribute Memory */
    uint8_t* hram;        /**< High RAM (0xFF80-0xFFFE) */
    uint8_t* io;          /**< I/O registers (0xFF00-0xFF7F) */
    /* Memory past one WRAM bank (gb_context_set_wram_extension), or NULL. */
    uint8_t* wram_ext;
    uint32_t wram_ext_size;   /**< Bytes kept in states, mapped ones first */
    uint16_t wram_ext_mapped; /**< Bytes seen at 0xE000 while SVBK selects wram_ext_bank */
    uint8_t wram_ext_bank;
    uint16_t wram_ext_cart_mapped;  /**< Bytes seen at 0xA000 in that bank instead of cartridge RAM */
    uint32_t wram_ext_cart_offset;  /**< Where those start in wram_ext */
    uint8_t wram_ext_bank2;         /**< Another bank with a window at 0xE000, 0 = none */
    uint16_t wram_ext_mapped2;      /**< Bytes seen at 0xE000 while SVBK selects it */
    uint32_t wram_ext_offset2;      /**< Where those start in wram_ext */

    /* Boot ROM (BIOS) — LLE boot. When boot_rom is loaded and the context is
     * reset without skip_bootrom, the real boot ROM is mapped at 0x0000 and
     * executed from PC=0; a write to 0xFF50 unmaps it (boot_rom_active=0) and
     * hands off to the cartridge at 0x0100. NULL => HLE skip (production default). */
    uint8_t* boot_rom;        /**< Boot ROM buffer, or NULL */
    size_t   boot_rom_size;   /**< 256 (DMG/MGB/SGB) or 2304 (CGB) */
    uint8_t  boot_rom_active; /**< Boot ROM currently mapped + executing */
    
    /* RTC state (MBC3) */
    struct {
        uint8_t s, m, h, dl, dh;        /**< Seconds, Minutes, Hours, Days Low, Days High */
        uint8_t latched_s, latched_m, latched_h, latched_dl, latched_dh;
        uint8_t latch_state;            /**< 0=Normal, 1=Latch prepared (wrote 0) */
        uint64_t last_time;             /**< Last time update (in cycles) */
        bool active;                    /**< RTC oscillator active (DH bit 6) */
    } rtc;
    
    /* Hardware components (opaque pointers) */
    void* ppu;            /**< Pixel Processing Unit */
    void* apu;            /**< Audio Processing Unit */
    void* timer;          /**< Timer unit */
    void* serial;         /**< Serial port */
    void* joypad;         /**< Joypad input */
    void* sgb;            /**< Super GameBoy state (NULL when disabled) */
    void* ir;             /**< CGB infrared (RP) state. Always non-NULL on a running ctx. */
    uint8_t last_joypad;  /**< Last joypad state for interrupt generation */
    uint8_t used_dispatch_fallback; /**< Generated path fell back to interpreter */
    uint8_t dispatch_fallback_bank; /**< Bank used for the most recent fallback */
    uint16_t dispatch_fallback_addr; /**< PC used for the most recent fallback */
    uint32_t frame_dispatch_fallbacks; /**< Fallback count accumulated in the current frame */
    uint64_t total_dispatch_fallbacks; /**< Total generated-to-interpreter fallbacks */
    uint8_t frame_first_fallback_bank; /**< First fallback bank in the current frame */
    uint16_t frame_first_fallback_addr; /**< First fallback PC in the current frame */
    uint8_t frame_last_fallback_bank; /**< Last fallback bank in the current frame */
    uint16_t frame_last_fallback_addr; /**< Last fallback PC in the current frame */
    uint64_t total_interpreter_entries; /**< Total interpreter sessions entered */
    uint64_t total_interpreter_instructions; /**< Total instructions executed in interpreter sessions */
    uint64_t total_interpreter_cycles; /**< Total cycles executed in interpreter sessions */
    uint64_t frame_interpreter_instructions; /**< Interpreter instructions in the current frame */
    uint64_t frame_interpreter_cycles; /**< Interpreter cycles in the current frame */
    uint8_t has_unimplemented_interpreter_opcode; /**< Interpreter hit an unsupported opcode */
    uint8_t last_unimplemented_opcode; /**< Most recent unsupported opcode seen by the interpreter */
    uint8_t last_unimplemented_bank; /**< Bank of the most recent unsupported opcode */
    uint16_t last_unimplemented_addr; /**< Address of the most recent unsupported opcode */
    GBInterpreterHotspot interpreter_hotspots[GBRT_INTERPRETER_HOTSPOT_CAPACITY]; /**< Top interpreter entry hotspots */
    uint64_t completed_frames; /**< Number of completed guest frames */
    
    /* Platform interface */
    void* platform;       /**< Platform-specific data */
    GBPlatformCallbacks callbacks; /**< Platform callbacks */
    
    /* Trace context */
    void* trace_file;     /**< FILE* for trace output */
    bool trace_entries_enabled;
    void* ppu_trace_file; /**< FILE* for focused PPU trace output */

    /* Lag-frame hold (gb_frame_hold_hook). Host state; not in save states,
     * which are taken at frame boundaries where no hold is active. */
    struct {
        uint8_t active;      /**< PPU/timers/APU wait at the end of line 143 */
        uint8_t released;    /**< Hold ended: let line 143 finish once */
        uint8_t suspended;   /**< Last hold hit the limit; wait for a frame to finish unaided */
        uint32_t cycles;     /**< Dots the CPU has run during this hold */
        uint64_t holds;      /**< Holds started */
        uint64_t limit_hits; /**< Holds ended by gb_frame_hold_limit */
    } frame_hold;
} GBContext;

/* ============================================================================
 * Context Management
 * ========================================================================== */

/**
 * @brief Create a new GameBoy context
 * @param config Configuration settings
 * @return New context or NULL on failure
 */
GBContext* gb_context_create(const GBConfig* config);

/**
 * @brief Destroy a GameBoy context
 * @param ctx Context to destroy
 */
void gb_context_destroy(GBContext* ctx);

/**
 * @brief Reset the CPU state
 * @param ctx Context to reset
 * @param skip_bootrom If true, initialize to post-bootrom state
 */
void gb_context_reset(GBContext* ctx, bool skip_bootrom);

/* Opt-in memory for a game module whose table outgrows a WRAM bank: while
 * SVBK selects `bank`, reads and writes of 0xE000 up to 0xE000 + `mapped`
 * reach this zeroed buffer instead of echoing C000-DDFF, so a table in that
 * bank continues past DFFF. Any other bank, and the rest of 0xE000-0xFDFF,
 * keep the echo. All `size` bytes (the mapped ones first, then whatever the
 * module keeps after them) go into rollback states and save-state files;
 * a save state without them loads them zeroed. Call before the first state
 * is taken; `mapped` is at most 0x1E00 and at most `size`. */
bool gb_context_set_wram_extension(GBContext* ctx, uint8_t bank, uint16_t mapped, uint32_t size);
/* Change what the extension answers for, at any time: `mapped` bytes at 0xE000
 * as above, and `cart_mapped` bytes at 0xA000 from `cart_offset` in the
 * buffer. While SVBK selects the extension's bank, 0xA000 up to 0xA000 +
 * `cart_mapped` reads and writes the buffer instead of cartridge RAM, enabled
 * or not, so a module maps it only while the game leaves cartridge RAM alone.
 * Rollback states and save states restore neither mapping (they are host
 * settings); the module sets them again after a load. */
bool gb_context_map_wram_extension(GBContext* ctx, uint16_t mapped, uint16_t cart_mapped,
                                   uint32_t cart_offset);
/* A window for a second bank, for a table there that also outgrows it: while
 * SVBK selects `bank`, 0xE000 up to 0xE000 + `mapped` reads and writes the
 * buffer from `offset` instead of the echo. `bank` is not the extension's own;
 * `mapped` 0 removes the window. A host setting like the others. */
bool gb_context_map_wram_extension_bank(GBContext* ctx, uint8_t bank, uint16_t mapped,
                                        uint32_t offset);

/**
 * @brief Load a ROM into the context
 * @param ctx Target context
 * @param data ROM data
 * @param size ROM size in bytes
 * @return true on success
 */
bool gb_context_load_rom(GBContext* ctx, const uint8_t* data, size_t size);

/**
 * @brief Load a boot ROM (BIOS) for LLE boot. Copies `data`. After this, a
 * gb_context_reset with skip_bootrom=false maps and executes it.
 * @param size 256 (DMG/MGB/SGB) or 2304 (CGB)
 * @return true on success
 */
bool gb_context_load_boot_rom(GBContext* ctx, const uint8_t* data, size_t size);

/**
 * @brief Save battery-backed RAM to persistent storage
 * @param ctx Target context
 * @return true on success
 */
bool gb_context_save_ram(GBContext* ctx);

/**
 * @brief Set a stable save identifier to use instead of the cartridge header title
 * @param ctx Target context
 * @param save_id Generated-project or ROM basename
 */
void gb_context_set_save_id(GBContext* ctx, const char* save_id);

/**
 * @brief Save a full emulator snapshot to a file
 * @param ctx Target context
 * @param path Output path
 * @return true on success
 */
bool gb_context_save_state_file(GBContext* ctx, const char* path);

/**
 * @brief Load a full emulator snapshot from a file
 * @param ctx Target context
 * @param path Input path
 * @return true on success
 */
bool gb_context_load_state_file(GBContext* ctx, const char* path);

/* In-memory machine state for rollback (preemptive frames): the context,
 * memories, PPU and APU a save state holds, plus what later frames also depend
 * on -- the lag-frame hold, the extended view's OAM sidecar and a game
 * module's own state (gb_game_state). Loading one keeps host settings, the
 * memory layout and the extended view's geometry, so restoring a state and
 * running the same frames with the same input reproduces them. IR and SGB
 * state are left out, as in save states. */
size_t gb_state_size(const GBContext* ctx);
void gb_state_save(const GBContext* ctx, void* out);
void gb_state_load(GBContext* ctx, const void* in);
/* The same, passing over the PPU's finished picture (its framebuffers): save
 * leaves those bytes of the buffer as they are, load keeps the live picture.
 * Nothing reads the picture back, and a frame with the LCD on draws a whole
 * new one, so such a frame ends exactly as from the full state. Rewind
 * stores these, as RetroArch lets
 * a core give it a lighter same-instance state; the picture is most of what
 * changes from frame to frame. */
void gb_state_save_no_picture(const GBContext* ctx, void* out);
void gb_state_load_no_picture(GBContext* ctx, const void* in);

/* A game module's host state that later frames depend on (hook bookkeeping,
 * a custom view's latched lists), saved and loaded with gb_state_*. Zero size
 * when the game has none. A save state file holds none of it: file_loaded
 * (optional) runs after gb_context_load_state_file to rebuild what it can. */
typedef struct {
    size_t size;
    void (*save)(const GBContext* ctx, void* out);
    void (*load)(GBContext* ctx, const void* in);
    void (*file_loaded)(GBContext* ctx);
} GBGameState;
extern GBGameState gb_game_state;

/* Called by gb_reset_frame() before a context runs its first frame: the cart
 * is loaded and reset but has not run, which is the machine a host restores to
 * restart the game in place. Called again for a context set back to that point
 * (completed_frames 0, nothing run); NULL by default. */
extern void (*gb_before_first_frame)(GBContext* ctx);

/* ============================================================================
 * Memory Access
 * ========================================================================== */

/**
 * @brief Read a byte from memory
 * @param ctx CPU context
 * @param addr 16-bit address
 * @return Byte at address
 */
uint8_t gb_read8(GBContext* ctx, uint16_t addr);

/**
 * @brief Write a byte to memory
 * @param ctx CPU context
 * @param addr 16-bit address
 * @param value Byte to write
 */
void gb_write8(GBContext* ctx, uint16_t addr, uint8_t value);

/**
 * @brief Read a 16-bit word from memory (little-endian)
 * @param ctx CPU context
 * @param addr 16-bit address
 * @return Word at address
 */
uint16_t gb_read16(GBContext* ctx, uint16_t addr);

/**
 * @brief Write a 16-bit word to memory (little-endian)
 * @param ctx CPU context
 * @param addr 16-bit address
 * @param value Word to write
 */
void gb_write16(GBContext* ctx, uint16_t addr, uint16_t value);

/* ============================================================================
 * Stack Operations
 * ========================================================================== */

/**
 * @brief Push a 16-bit value onto the stack
 */
void gb_push16(GBContext* ctx, uint16_t value);

/**
 * @brief Pop a 16-bit value from the stack
 */
uint16_t gb_pop16(GBContext* ctx);

/* ============================================================================
 * ALU Operations (with flag updates)
 * ========================================================================== */

void gb_add8(GBContext* ctx, uint8_t value);
void gb_adc8(GBContext* ctx, uint8_t value);
void gb_sub8(GBContext* ctx, uint8_t value);
void gb_sbc8(GBContext* ctx, uint8_t value);
void gb_and8(GBContext* ctx, uint8_t value);
void gb_or8(GBContext* ctx, uint8_t value);
void gb_xor8(GBContext* ctx, uint8_t value);
void gb_cp8(GBContext* ctx, uint8_t value);
uint8_t gb_inc8(GBContext* ctx, uint8_t value);
uint8_t gb_dec8(GBContext* ctx, uint8_t value);

void gb_add16(GBContext* ctx, uint16_t value);
void gb_add_sp(GBContext* ctx, int8_t offset);
void gb_ld_hl_sp_n(GBContext* ctx, int8_t offset);

/* ============================================================================
 * Rotate/Shift Operations
 * ========================================================================== */

uint8_t gb_rlc(GBContext* ctx, uint8_t value);
uint8_t gb_rrc(GBContext* ctx, uint8_t value);
uint8_t gb_rl(GBContext* ctx, uint8_t value);
uint8_t gb_rr(GBContext* ctx, uint8_t value);
uint8_t gb_sla(GBContext* ctx, uint8_t value);
uint8_t gb_sra(GBContext* ctx, uint8_t value);
uint8_t gb_srl(GBContext* ctx, uint8_t value);
uint8_t gb_swap(GBContext* ctx, uint8_t value);

void gb_rlca(GBContext* ctx);
void gb_rrca(GBContext* ctx);
void gb_rla(GBContext* ctx);
void gb_rra(GBContext* ctx);

/* ============================================================================
 * Bit Operations
 * ========================================================================== */

void gb_bit(GBContext* ctx, uint8_t bit, uint8_t value);

/* ============================================================================
 * Misc Operations
 * ========================================================================== */

void gb_daa(GBContext* ctx);

/* ============================================================================
 * Control Flow
 * ========================================================================== */

/**
 * @brief Call a function at the given address
 */
void gb_call(GBContext* ctx, uint16_t addr);

/**
 * @brief Return from a function
 */
void gb_ret(GBContext* ctx);

/** Execute RET's stack bus phases and its full 16 (or taken-conditional 20) cycles. */
void gb_ret_timed(GBContext* ctx, uint32_t cycles);

/**
 * @brief RST vector call
 */
void gb_rst(GBContext* ctx, uint8_t vector);

/**
 * @brief Seed the MBC3 RTC wall clock for deterministic/replay runs.
 *
 * By default the RTC reads the host clock (time(NULL)); injecting a fixed
 * unix-seconds epoch makes RTC-cart elapsed-time advance reproducible.
 * Equivalent to the GBRT_RTC_EPOCH env var; an explicit call wins over env.
 */
void gbrt_set_rtc_epoch(int64_t unix_seconds);

/**
 * @brief Dump final CPU registers to GBRT_REGS_LOG (headless test-ROM grading).
 *
 * No-op unless the GBRT_REGS_LOG env var names a writable path. Used by the
 * accuracy harness to read mooneye's pass/fail register magic at the frame
 * limit without a debugger.
 */
void gbrt_dump_final_regs(GBContext* ctx);

/**
 * @brief Jump to address in HL (JP HL)
 */
void gbrt_jump_hl(GBContext* ctx);

/**
 * @brief Dispatch to recompiled function at address
 */
void gb_dispatch(GBContext* ctx, uint16_t addr);

/**
 * @brief Dispatch a CALL to unanalyzed code (pushes return address first)
 */
void gb_dispatch_call(GBContext* ctx, uint16_t addr);

/**
 * @brief Route the runtime's own entry into recompiled code
 *
 * Multi-body seam (see gb_body.h). A generated project normally defines the
 * plain gb_dispatch above and the runtime calls it directly; that is the
 * default and nothing has to call this. An executable carrying more than one
 * recompiled body namespaces all but one of them, so gb_dispatch belongs to
 * the primary body only: gb_body_resolve() calls this with the selected body's
 * own dispatch pair. Passing NULL restores the direct gb_dispatch path.
 */
typedef void (*GBDispatchFn)(GBContext* ctx, uint16_t addr);
void gb_set_dispatch(GBDispatchFn dispatch, GBDispatchFn dispatch_call);

/**
 * @brief The dispatch entry the runtime will use (registered body, else gb_dispatch)
 */
void gbrt_dispatch(GBContext* ctx, uint16_t addr);
void gbrt_dispatch_call(GBContext* ctx, uint16_t addr);

/**
 * @brief Fallback interpreter for uncompiled code
 */
void gb_interpret(GBContext* ctx, uint16_t addr);

/**
 * @brief Execute known copied HRAM helper stubs in-place when possible
 * @return 1 if a known HRAM stub instruction was executed, 0 otherwise
 */
uint8_t gbrt_try_execute_hram_stub(GBContext* ctx, uint16_t addr);

/**
 * @brief Execute simple copied helper stubs in the FF00-FF7F high-memory range
 * @return 1 if a known helper instruction was executed, 0 otherwise
 */
uint8_t gbrt_try_execute_highmem_stub(GBContext* ctx, uint16_t addr);

/**
 * @brief Execute simple copied RAM helper stubs in-place when possible
 * @return 1 if a known RAM stub instruction was executed, 0 otherwise
 */
uint8_t gbrt_try_execute_ram_stub(GBContext* ctx, uint16_t addr);

/* ============================================================================
 * CPU State
 * ========================================================================== */

/**
 * @brief Halt the CPU until interrupt
 */
void gb_halt(GBContext* ctx);

/**
 * @brief Stop the CPU (and LCD)
 */
void gb_stop(GBContext* ctx);

/* ============================================================================
 * Flag Helpers
 * ========================================================================== */

/**
 * @brief Pack individual flags into F register
 */
static inline void gb_pack_flags(GBContext* ctx) {
    ctx->f = (ctx->f_z ? 0x80 : 0) |
             (ctx->f_n ? 0x40 : 0) |
             (ctx->f_h ? 0x20 : 0) |
             (ctx->f_c ? 0x10 : 0);
}



/**
 * @brief Unpack F register into individual flags
 */
static inline void gb_unpack_flags(GBContext* ctx) {
    ctx->f_z = (ctx->f & 0x80) != 0;
    ctx->f_n = (ctx->f & 0x40) != 0;
    ctx->f_h = (ctx->f & 0x20) != 0;
    ctx->f_c = (ctx->f & 0x10) != 0;
}

/* ============================================================================
 * Serial port
 * ========================================================================== */

/**
 * @brief Complete a deferred or slave serial transfer with the byte received
 * from the peer. Updates SB, clears SC bit 7, raises the SIO interrupt, and
 * resets the in-flight transfer state. Safe to call from either an
 * external-clock (slave) path or a deferred internal-clock (master) path.
 */
void gb_serial_complete_transfer(GBContext* ctx, uint8_t received_byte);

/**
 * @brief If an external-clock (slave) transfer is currently armed, copy out
 * the byte the game placed in SB and return true. Caller is expected to
 * forward that byte to the link peer and then call
 * gb_serial_complete_transfer() when the peer's master byte arrives.
 * Returns false if no slave transfer is armed.
 */
bool gb_serial_take_slave_byte(GBContext* ctx, uint8_t* outgoing_out);

/* ============================================================================
 * Timing
 * ========================================================================== */

/**
 * @brief Add cycles to the timing counters
 */
void gb_add_cycles(GBContext* ctx, uint32_t cycles);

/**
 * @brief Run one HBlank HDMA block if a transfer is pending
 */
void gbrt_hdma_hblank(GBContext* ctx);

/**
 * @brief Check if a frame worth of cycles has elapsed
 */
bool gb_frame_complete(GBContext* ctx);

/**
 * @brief Get the current framebuffer
 * @param ctx CPU context
 * @return Pointer to 160x144 ARGB8888 framebuffer, or NULL if not ready
 */
const uint32_t* gb_get_framebuffer(GBContext* ctx);

/**
 * @brief Reset the frame ready flag for the next frame
 * @param ctx CPU context
 */
void gb_reset_frame(GBContext* ctx);

/**
 * @brief Process hardware for the given number of cycles
 */
void gb_tick(GBContext* ctx, uint32_t cycles);

/* ============================================================================
 * Platform Interface
 * ========================================================================== */

/* Moved GBPlatformCallbacks definition to top to resolve circular dependency */

/**
 * @brief Set platform callbacks
 */
void gb_set_platform_callbacks(GBContext* ctx, const GBPlatformCallbacks* callbacks);

/* ============================================================================
 * Execution
 * ========================================================================== */

/**
 * @brief Run one frame of emulation
 * @return Number of cycles executed
 */
uint32_t gb_run_frame(GBContext* ctx);

/**
 * @brief Run emulation until a frame completes or the cycle budget is spent
 * @param max_cycles Maximum cycles to execute before returning (0 = no limit)
 * @return Number of cycles executed
 */
uint32_t gb_run_cycles(GBContext* ctx, uint32_t max_cycles);

/**
 * @brief Run a single step (one instruction or until interrupt)
 * @return Number of cycles executed
 */
uint32_t gb_step(GBContext* ctx);

/**
 * @brief Run one scheduler step in a specific execution mode
 * @param mode Generated dispatch or interpreter execution
 * @return Number of cycles executed
 */
uint32_t gb_debug_step(GBContext* ctx, GBExecutionMode mode);

/**
 * @brief Compare generated and interpreter execution in lockstep
 * @return true if both paths stayed in sync for the full run
 */
bool gb_run_differential(GBContext* generated_ctx,
                         GBContext* interpreted_ctx,
                         const GBDifferentialOptions* options,
                         GBDifferentialResult* result);

/* ============================================================================
 * Differential co-simulation (first-divergence decision procedure)
 * See COSIM_ORACLE.md. Full-state canonical hash + per-subsystem sub-hashes,
 * checkpointed on the shared guest T-cycle clock, halting at the first split.
 * ========================================================================== */

/* Gate-3 fault-injection target: after checkpoint `inject_at_checkpoint`, one
 * field in context A is perturbed so the tool MUST halt at ~that checkpoint and
 * name the owning subsystem. Proves the tool is not silently blind. */
typedef enum {
    GB_COSIM_INJECT_NONE = 0,
    GB_COSIM_INJECT_WRAM = 1,  /* flip one WRAM byte */
    GB_COSIM_INJECT_PPU  = 2,  /* bump PPU mode_cycles */
    GB_COSIM_INJECT_APU  = 3,  /* flip ch4 LFSR bit */
    GB_COSIM_INJECT_CPU  = 4,  /* flip a bit in register B */
    GB_COSIM_INJECT_TIMER = 5, /* bump div_counter */
} GBCosimInjectTarget;

typedef struct {
    GBExecutionMode mode_a;   /**< backend for context A (default GENERATED) */
    GBExecutionMode mode_b;   /**< backend for context B (default INTERPRETER) */
    uint32_t checkpoint_stride; /**< T-cycles between full-state checkpoints (0 => 456, one scanline) */
    uint64_t max_frames;      /**< stop after this many completed frames (0 => use max_checkpoints) */
    uint64_t max_checkpoints; /**< stop after this many checkpoints (0 => unbounded) */
    uint64_t audit_interval;  /**< Gate 4: force a full byte/field compare every N checkpoints (0 => off) */
    uint64_t log_interval;    /**< progress log cadence in checkpoints (0 => off) */
    GBCosimInjectTarget inject_target; /**< Gate 3 fault-injection target (default NONE) */
    uint64_t inject_at_checkpoint;     /**< checkpoint index at which to inject */
    const char* input_script; /**< optional scripted input (same format as differential) */
} GBCosimOptions;

typedef struct {
    bool matched;             /**< true if both backends stayed hash-identical the whole run */
    uint64_t checkpoints_completed;
    uint64_t frames_completed;
    uint64_t mismatch_checkpoint; /**< checkpoint index of the first split (0 if matched) */
    uint32_t mismatch_cycles;     /**< ctx->cycles at the first split */
    int mismatch_subsystem;       /**< sub-hash index of the first split, or -1 */
    uint64_t chain_hash;          /**< cumulative chain hash at the end (regression pin) */
    uint16_t pc_a;                /**< PC of context A at the split (report only) */
    uint16_t pc_b;                /**< PC of context B at the split (report only) */
    char message[256];            /**< short description (named field from the exact-compare drill) */
} GBCosimResult;

/**
 * @brief Run the full-state co-simulation decision procedure in lockstep.
 * Both contexts must be freshly initialized with identical ROM/config. Returns
 * true iff the two backends' full-state hash matched at every checkpoint.
 */
bool gb_run_cosim(GBContext* ctx_a,
                  GBContext* ctx_b,
                  const GBCosimOptions* options,
                  GBCosimResult* result);

/**
 * @brief LLE-vs-HLE boot gate. `lle_ctx` must be reset with a boot ROM loaded
 * (boot_rom_active=1); `hle_ctx` reset with skip_bootrom=true. Runs the real
 * boot ROM to handoff, then compares the post-boot architectural state (CPU
 * registers + IME + DIV + I/O page) against the HLE post-boot constants,
 * reporting every field that differs. Returns true iff they match (our HLE
 * post-boot state is faithful to the real BIOS handoff).
 */
bool gb_run_boot_gate(GBContext* lle_ctx,
                      GBContext* hle_ctx,
                      uint64_t cycle_cap,
                      GBCosimResult* result);

/* ---- Cross-oracle co-sim vs embedded SameBoy (only defined when the runtime
 * is built with -DGBC_COSIM_SAMEBOY; see COSIM_ORACLE.md). ---- */

/* Boot SameBoy on the same BIOS+ROM in-process and report its boot-handoff
 * T-cycle / PC / DIV / LY, then a neutral-state hash after a few frames. Proves
 * the embedded oracle runs and yields the reference boot timing (arbitrates the
 * LLE-vs-HLE DIV question). */
void gb_sameboy_selfcheck(const uint8_t* boot_rom, size_t boot_rom_size,
                          const uint8_t* rom, size_t rom_size, int is_cgb);

/* Cycle-0 lockstep of an LLE-booted recomp context against SameBoy on the same
 * BIOS+ROM, comparing the implementation-neutral architectural hash. Reports the
 * boot-handoff timing of each side and the first neutral-state divergence.
 * `recomp_lle_ctx` must be LLE-reset (boot ROM loaded, skip_bootrom=false). */
bool gb_run_sameboy_cosim(GBContext* recomp_lle_ctx,
                          const uint8_t* boot_rom, size_t boot_rom_size,
                          const GBCosimOptions* options, GBCosimResult* result);

/**
 * @brief Record a generated-dispatch fallback into the interpreter
 */
void gbrt_note_dispatch_fallback(GBContext* ctx, uint8_t bank, uint16_t addr);

/**
 * @brief Record one completed interpreter session for hotspot tracking
 */
void gbrt_note_interpreter_session(GBContext* ctx,
                                   uint8_t bank,
                                   uint16_t addr,
                                   uint32_t instructions,
                                   uint32_t cycles);

/**
 * @brief Record an unsupported opcode observed by the interpreter
 */
void gbrt_note_unimplemented_interpreter_opcode(GBContext* ctx,
                                                uint8_t bank,
                                                uint16_t addr,
                                                uint8_t opcode);

/**
 * @brief Helper to invoke audio callback
 */
void gb_audio_callback(GBContext* ctx, int16_t left, int16_t right);

/**
 * @brief Set input automation script.
 *
 * Legacy entries use "frame:buttons:duration". Cycle-anchored entries use
 * "c<cycle>:buttons:<duration_cycles>".
 */
void gb_platform_set_input_script(const char* script);

/**
 * @brief Record live keyboard/controller input to a replayable script file
 */
void gb_platform_set_input_record_file(const char* path);

/**
 * @brief Set frames to dump screenshots (format: "frame1,frame2,...")
 */
void gb_platform_set_dump_frames(const char* frames);

/* Opt-in extended view (widescreen): CLI width request, resolved against the
 * game capability at gb_platform_register_context. See gb_widescreen.h. */
void gb_ws_set_cli_request(int width);

/* Runtime chokepoint for config-declared [[imm_override]] sites (ALU-immediate
 * or LD r,n8 instructions): the generator emits gbrt_imm_override8(ctx, bank, pc, orig) instead
 * of the literal at reviewed instructions. With no hook installed the
 * original immediate is returned (behavior unchanged). An opt-in game module
 * (e.g. widescreen) installs the hook to widen specific bounds. */
typedef uint8_t (*GBImmOverrideHook)(GBContext* ctx, uint8_t bank,
                                     uint16_t pc, uint8_t orig);
extern GBImmOverrideHook gbrt_imm_override_hook;
uint8_t gbrt_imm_override8(GBContext* ctx, uint8_t bank, uint16_t pc, uint8_t orig);

/* Optional lag-frame removal, installed by a game module. The hook returns
 * nonzero while the game's main loop has not finished the frame it is
 * computing. If it does so when the PPU completes line 143, the PPU, timers,
 * APU and serial port wait there while the CPU (with OAM DMA) keeps running,
 * until the hook returns zero, the CPU halts, or gb_frame_hold_limit dots have
 * run. The frame then enters VBlank as usual: the picture, interrupt timing and
 * audio stream are unchanged; the game only gets more CPU time per frame.
 * After a hold reaches the limit (a screen load, not a lag frame) no hold
 * starts again until the hook reports a frame that finished unaided. */
typedef int (*GBFrameHoldHook)(GBContext* ctx);
extern GBFrameHoldHook gb_frame_hold_hook;
extern uint32_t gb_frame_hold_limit;
/* Called by the PPU at the end of line 143; true = wait there. */
bool gb_frame_hold_at_vblank(GBContext* ctx);

/* Optional instruction-boundary hook, installed by a game module. The
 * scheduler calls it before dispatching the next instruction (gb_step and
 * gb_debug_step, so both backends see it at the same boundaries), after any
 * pending interrupt has been taken. Like an interrupt it may redirect
 * execution: push ctx->pc and set a new one. Generated code only returns to
 * the scheduler at such a boundary when something sets ctx->stopped, so a hook
 * that needs one requests it from a point it already controls (for example an
 * [[imm_override]] site). The generated dispatcher also runs on through calls
 * and returns by itself; it offers each address it dispatches to
 * game_dispatch_override(), which is where a game sees such a return. */
typedef void (*GBStepHook)(GBContext* ctx);
extern GBStepHook gb_step_hook;

/**
 * @brief Set guest-frame numbers to dump by elapsed guest CYCLES (frame N = N*70224
 * T-cycles), independent of rendered/VBlank frames. Robust for ROMs that keep the LCD
 * off, enable it late, or halt after rendering (e.g. Mealybug PPU tests). Format
 * "frame1,frame2,...". Pair with gb_platform_check_cycle_dump() in the run loop.
 */
void gb_platform_set_dump_cycle_frames(const char* frames);

/**
 * @brief Capture the framebuffer when guest time crosses each requested target.
 * Call once per gb_run_cycles slice. In benchmark mode, exits when all are captured.
 */
void gb_platform_check_cycle_dump(GBContext* ctx);

/**
 * @brief Set filename prefix for screenshots
 */
void gb_platform_set_screenshot_prefix(const char* prefix);

/**
 * @brief Enable entry tracing to a file
 */
void gbrt_set_trace_file(const char* filename);

/**
 * @brief Log an entry point to the trace file
 */
void gbrt_log_trace(GBContext* ctx, uint16_t bank, uint16_t addr);
void gbrt_log_ppu_scanline(GBContext* ctx,
                           uint8_t ly,
                           uint8_t mode,
                           uint8_t lcdc,
                           uint8_t stat,
                           uint8_t scx,
                           uint8_t scy,
                           uint8_t wx,
                           uint8_t wy,
                           uint8_t bgp,
                           uint8_t obp0,
                           uint8_t obp1,
                           uint8_t window_line,
                           bool window_triggered);
void gbrt_log_ppu_register_write(GBContext* ctx,
                                 uint16_t addr,
                                 uint8_t old_value,
                                 uint8_t new_value,
                                 uint8_t ly,
                                 uint8_t mode);
void gbrt_log_oam_snapshot(GBContext* ctx, const char* reason);
void gbrt_log_stat_irq_check(GBContext* ctx,
                             const char* reason,
                             uint8_t ly,
                             uint8_t mode,
                             uint8_t stat,
                             uint8_t source_state_mask,
                             uint8_t source_enable_mask,
                             uint8_t active_source_mask,
                             bool previous_line_state,
                             bool current_line_state);
void gbrt_log_stat_irq_request(GBContext* ctx,
                               const char* reason,
                               uint8_t ly,
                               uint8_t mode,
                               uint8_t stat,
                               uint8_t active_source_mask,
                               uint8_t if_before,
                               uint8_t if_after);
void gbrt_log_interrupt_service(GBContext* ctx,
                                const char* name,
                                uint16_t vector,
                                uint8_t if_before,
                                uint8_t ie_reg,
                                uint8_t interrupt_bit,
                                uint16_t pc_before,
                                uint16_t sp_before);
void gbrt_note_lcd_transition(GBContext* ctx, bool lcd_enabled, uint8_t old_lcdc, uint8_t new_lcdc, uint8_t ly, uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_H */
