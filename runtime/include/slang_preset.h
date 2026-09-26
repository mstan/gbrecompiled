/**
 * @file slang_preset.h
 * @brief RetroArch .slangp presets as data, for the shader preset editor.
 *
 * A preset is read into its passes, lookup textures and parameter overrides,
 * with every path made absolute. Reading follows librashader's rules: the
 * files a preset #references come first and later values win, and each path is
 * relative to the file that names it. Writing puts out one self-contained
 * preset (no #reference) with paths relative to where it is written, which
 * librashader then loads like any other. Chains are built by adding shader
 * passes and combining presets, as RetroArch's Shader menu does.
 */
#ifndef GB_SLANG_PRESET_H
#define GB_SLANG_PRESET_H

#include <string>
#include <utility>
#include <vector>

typedef std::vector<std::pair<std::string, std::string>> SlangPresetValues;

struct SlangPresetPass {
    std::string shader;          /* absolute .slang path, '/'-separated */
    /* The pass's settings by key without its index (filter_linear,
     * scale_type, scale, wrap_mode, alias, ...), values as written. */
    SlangPresetValues settings;

    std::string get(const std::string& key) const;   /* "" when unset */
    void set(const std::string& key, const std::string& value);   /* "" unsets */
};

struct SlangPresetTexture {
    std::string name;
    std::string path;            /* absolute */
    SlangPresetValues settings;  /* linear, wrap_mode, mipmap */
};

struct SlangPreset {
    std::vector<SlangPresetPass> passes;
    std::vector<SlangPresetTexture> textures;
    SlangPresetValues parameters;   /* parameter name -> value */
};

/* Reads `path` (following #reference). False with a reason on failure. */
bool slang_preset_read(const std::string& path, SlangPreset* out, std::string* error);

/* Writes `preset` to `path`, creating its folder. False with a reason. */
bool slang_preset_write(const std::string& path, const SlangPreset& preset, std::string* error);

/* Adds `other`'s passes after this chain's (before it with `before`), and the
 * textures and parameter overrides this chain does not have yet. */
void slang_preset_combine(SlangPreset* preset, const SlangPreset& other, bool before);

/* Sets a parameter override, adding it if it is new. */
void slang_preset_set_parameter(SlangPreset* preset, const std::string& name, const std::string& value);

#endif /* GB_SLANG_PRESET_H */
