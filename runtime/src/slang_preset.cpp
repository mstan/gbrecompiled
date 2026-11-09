/*
 * See slang_preset.h. The reading mirrors librashader-presets (parse/token.rs
 * and parse/value.rs), since librashader is what loads the presets written
 * here: the same comments and quoting, #reference order, per-pass keys and
 * their aliases, and the same fallbacks for undeclared parameters and
 * textures.
 */
#include "slang_preset.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

struct Token {
    std::string key;
    std::string value;
    fs::path dir;   /* folder of the file the line is in: paths are relative to it */
};

std::string trim(const std::string& s, const char* chars = " \t\r\n") {
    const size_t b = s.find_first_not_of(chars);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(chars) - b + 1);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    if (!s.empty() && s.front() == '"') return s.substr(1);   /* presets leave quotes open */
    return s;
}

/* A value that opens with a closed quoted string is that string (comment
 * characters inside it included); otherwise it ends at a // or # comment. */
std::string lex_value(const std::string& rest) {
    if (!rest.empty() && rest[0] == '"') {
        const size_t close = rest.find('"', 1);
        if (close != std::string::npos) return rest.substr(1, close - 1);
    }
    std::string value = rest;
    for (const char* comment : { "//", "#" }) {
        const size_t at = value.find(comment);
        if (at != std::string::npos) value = value.substr(0, at);
    }
    return unquote(value);
}

/* One file's lines as (key, value); #reference lines keep that key. A line
 * with no '=' runs into the next one that has one, as librashader's lexer
 * reads a key up to the next '=' whatever lies between: that key matches
 * nothing, so neither line counts. */
SlangPresetValues lex(const std::string& text) {
    SlangPresetValues out;
    std::istringstream in(text);
    std::string line;
    bool in_block = false;
    bool run_on = false;
    while (std::getline(in, line)) {
        if (in_block) {
            const size_t end = line.find("*/");
            if (end == std::string::npos) continue;
            line = line.substr(end + 2);
            in_block = false;
        }
        std::string t = trim(line);
        while (starts_with(t, "/*")) {
            const size_t end = t.find("*/", 2);
            if (end == std::string::npos) {
                in_block = true;
                t.clear();
            } else {
                t = trim(t.substr(end + 2));
            }
        }
        if (t.empty()) continue;
        const size_t eq = t.find('=');
        if (run_on) {
            run_on = eq == std::string::npos;
            continue;
        }
        if (starts_with(t, "#reference")) {
            out.emplace_back("#reference", unquote(t.substr(10)));
            continue;
        }
        if (t[0] == '#' || starts_with(t, "//")) continue;
        if (eq == std::string::npos) {
            run_on = true;
            continue;
        }
        const std::string key = trim(t.substr(0, eq));
        if (!key.empty()) out.emplace_back(key, lex_value(trim(t.substr(eq + 1))));
    }
    return out;
}

bool read_text(const fs::path& file, std::string* text) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    *text = buf.str();
    return true;
}

std::string utf8(const fs::path& p) {
    const auto s = p.lexically_normal().generic_u8string();
    return std::string(s.begin(), s.end());
}

/* Every line of `file` and the files it #references, in librashader's order
 * (load_child_reference_strings): later lines win, the file's own last. That
 * order is not the files' order when one file has several #reference lines
 * (the first of them wins there), and the result must match what librashader
 * made of the original. */
bool collect(const fs::path& file, std::vector<Token>& tokens, std::string* error) {
    struct File {
        fs::path dir;
        SlangPresetValues lines;
    };
    auto load = [error](const fs::path& path, File* out) {
        std::string text;
        if (!read_text(path, &text)) {
            *error = "could not read " + utf8(path);
            return false;
        }
        out->dir = path.parent_path();
        out->lines = lex(text);
        return true;
    };
    auto references = [](const File& f) {
        std::vector<fs::path> paths;
        for (const auto& [key, value] : f.lines) {
            if (key == "#reference" && !value.empty()) paths.push_back(f.dir / fs::u8path(value));
        }
        return paths;
    };

    File root;
    if (!load(file, &root)) return false;
    std::deque<File> order;   /* lowest precedence first */
    std::deque<std::vector<fs::path>> pending = { references(root) };
    for (int depth = 0; !pending.empty(); depth++) {
        if (depth > 16) {
            *error = "more than 16 levels of #reference in " + utf8(file);
            return false;
        }
        const std::vector<fs::path> paths = pending.front();
        pending.pop_front();
        for (const fs::path& path : paths) {
            File child;
            if (!load(path, &child)) return false;
            std::vector<fs::path> nested = references(child);
            order.push_front(std::move(child));
            if (!nested.empty()) pending.push_front(std::move(nested));
        }
    }
    order.push_back(std::move(root));
    for (const File& f : order) {
        for (const auto& [key, value] : f.lines) {
            if (key != "#reference") tokens.push_back({ key, value, f.dir });
        }
    }
    return true;
}

/* "scale_type12" -> ("scale_type", 12). */
bool split_indexed(const std::string& key, std::string* name, int* index) {
    size_t start = key.size();
    while (start > 0 && isdigit((unsigned char)key[start - 1])) start--;
    if (start == key.size() || start == 0) return false;
    *name = key.substr(0, start);
    *index = atoi(key.c_str() + start);
    return true;
}

/* Per-pass keys librashader reads, with the spellings presets use for some. */
const char* pass_key(const std::string& name) {
    static const char* const keys[] = {
        "filter_linear", "wrap_mode", "frame_count_mod", "srgb_framebuffer", "float_framebuffer",
        "mipmap_input", "alias", "scale_type", "scale_type_x", "scale_type_y", "scale", "scale_x",
        "scale_y",
    };
    if (name == "repeat_mode" || name == "texture_wrap_mode") return "wrap_mode";
    if (name == "mipmap") return "mipmap_input";
    for (const char* key : keys) {
        if (name == key) return key;
    }
    return NULL;
}

bool is_number(const std::string& value) {
    const std::string s = trim(value, " \t;");
    if (s.empty()) return false;
    char* end = NULL;
    strtod(s.c_str(), &end);
    return end && *end == '\0';
}

void set_value(SlangPresetValues& values, const std::string& key, const std::string& value) {
    for (auto& entry : values) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    values.emplace_back(key, value);
}

std::vector<std::string> split_names(const std::string& list) {
    std::vector<std::string> names;
    std::stringstream in(list);
    std::string name;
    while (std::getline(in, name, ';')) {
        name = trim(name);
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

/* A texture's own setting, or "" when `key` is not one: "<name>_linear" and
 * so on. */
std::string texture_setting(const std::string& key, const std::string& texture) {
    static const std::pair<const char*, const char*> suffixes[] = {
        { "_linear", "linear" }, { "_wrap_mode", "wrap_mode" }, { "_repeat_mode", "wrap_mode" },
        { "_mipmap", "mipmap" },
    };
    for (const auto& [suffix, setting] : suffixes) {
        if (key.size() == texture.size() + strlen(suffix) && starts_with(key, texture.c_str()) &&
            ends_with(key, suffix)) {
            return setting;
        }
    }
    return "";
}

/* `target` relative to `base` where the two share a folder, so they can move
 * together; absolute when they share only the drive (or not even that). */
std::string relative_to(const std::string& target, const fs::path& base) {
    const fs::path path = fs::u8path(target);
    const fs::path rel = path.lexically_relative(base);
    size_t ups = 0, depth = 0;
    for (const fs::path& part : rel) {
        if (part != "..") break;
        ups++;
    }
    const fs::path base_folders = base.relative_path();
    for (auto it = base_folders.begin(); it != base_folders.end(); ++it) depth++;
    return utf8(rel.empty() || ups >= depth ? path : rel);
}

}  /* anonymous namespace */

std::string SlangPresetPass::get(const std::string& key) const {
    for (const auto& [k, v] : settings) {
        if (k == key) return v;
    }
    return "";
}

void SlangPresetPass::set(const std::string& key, const std::string& value) {
    if (value.empty()) {
        settings.erase(std::remove_if(settings.begin(), settings.end(),
                                      [&](const auto& entry) { return entry.first == key; }),
                       settings.end());
        return;
    }
    set_value(settings, key, value);
}

bool slang_preset_read(const std::string& path, SlangPreset* out, std::string* error) {
    std::string ignored;
    if (!error) error = &ignored;
    std::vector<Token> tokens;
    std::error_code ec;
    fs::path file = fs::absolute(fs::u8path(path), ec);
    if (ec) file = fs::u8path(path);
    if (!collect(file, tokens, error)) return false;

    /* librashader's rules, which differ by kind: the first shaders count,
     * shaderN and per-pass value counts (resolve_values takes the first it
     * finds), parameters are overrides where the last wins, and a texture
     * named twice binds its last definition, which takes the settings lines
     * of its name in turn (the first definition the first lines). */
    std::vector<std::string> texture_names, parameter_names;
    std::vector<bool> used(tokens.size(), false);
    for (size_t i = 0; i < tokens.size(); i++) {
        std::vector<std::string>* names = tokens[i].key == "textures"     ? &texture_names
                                        : tokens[i].key == "parameters" ? &parameter_names
                                                                        : NULL;
        if (!names) continue;
        used[i] = true;
        for (const std::string& name : split_names(tokens[i].value)) {
            if (std::find(names->begin(), names->end(), name) == names->end()) names->push_back(name);
        }
    }
    auto listed = [](const std::vector<std::string>& names, const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    auto first_value = [](SlangPresetValues& values, const std::string& key, const std::string& value) {
        for (const auto& entry : values) {
            if (entry.first == key) return;
        }
        values.emplace_back(key, value);
    };

    std::vector<std::pair<int, std::string>> shaders;
    std::vector<SlangPresetTexture> definitions;
    for (size_t i = 0; i < tokens.size(); i++) {
        std::string name;
        int index = 0;
        if (used[i]) continue;
        if (split_indexed(tokens[i].key, &name, &index) && name == "shader") {
            shaders.emplace_back(index, utf8(tokens[i].dir / fs::u8path(tokens[i].value)));
            used[i] = true;
        }
    }
    for (size_t i = 0; i < tokens.size(); i++) {
        if (used[i] || !listed(texture_names, tokens[i].key)) continue;
        definitions.push_back({ tokens[i].key, utf8(tokens[i].dir / fs::u8path(tokens[i].value)), {} });
        used[i] = true;
    }
    /* Each definition takes the first unused settings line of each kind. */
    auto take_settings = [&](SlangPresetTexture& texture, std::vector<bool>& taken,
                             const std::vector<size_t>& from) {
        for (const char* kind : { "mipmap", "linear", "wrap_mode" }) {
            for (size_t i : from) {
                if (taken[i] || texture_setting(tokens[i].key, texture.name) != kind) continue;
                texture.settings.emplace_back(kind, trim(tokens[i].value, " \t;"));
                taken[i] = true;
                break;
            }
        }
    };
    std::vector<size_t> all(tokens.size());
    for (size_t i = 0; i < all.size(); i++) all[i] = i;
    for (SlangPresetTexture& texture : definitions) take_settings(texture, used, all);

    SlangPreset preset;
    std::vector<std::string> counts;
    std::map<int, SlangPresetValues> pass_settings;
    std::vector<size_t> rest;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (used[i]) continue;
        const Token& t = tokens[i];
        std::string name;
        int index = 0;
        if (listed(parameter_names, t.key)) {
            set_value(preset.parameters, t.key, is_number(t.value) ? trim(t.value, " \t;") : "0");
        } else if (t.key == "shaders") {
            counts.push_back(t.value);
        } else if (t.key == "feedback_pass") {
        } else if (split_indexed(t.key, &name, &index) && pass_key(name) &&
                   name != "scale" && name != "scale_x" && name != "scale_y") {
            first_value(pass_settings[index], pass_key(name), trim(t.value, " \t;"));
        } else {
            rest.push_back(i);
        }
    }
    /* Then scales, parameters nobody declared (anything that reads as a
     * number), and textures nobody declared (anything with an extension). */
    std::vector<SlangPresetTexture> undeclared;
    for (size_t i : rest) {
        const Token& t = tokens[i];
        std::string name;
        int index = 0;
        if (split_indexed(t.key, &name, &index) && (name == "scale" || name == "scale_x" || name == "scale_y")) {
            first_value(pass_settings[index], name, trim(t.value, " \t;"));
        } else if (is_number(t.value)) {
            set_value(preset.parameters, t.key, trim(t.value, " \t;"));
        } else if (!fs::u8path(t.value).extension().empty() && !ends_with(t.key, "_mipmap") &&
                   !ends_with(t.key, "_linear") && !ends_with(t.key, "_wrap_mode") &&
                   !ends_with(t.key, "_repeat_mode")) {
            undeclared.push_back({ t.key, utf8(t.dir / fs::u8path(t.value)), {} });
        }
    }
    std::vector<bool> taken(tokens.size(), false);
    for (SlangPresetTexture& texture : undeclared) {
        take_settings(texture, taken, rest);
        definitions.push_back(std::move(texture));
    }
    for (const SlangPresetTexture& texture : definitions) {
        auto bound = std::find_if(preset.textures.begin(), preset.textures.end(),
                                  [&](const SlangPresetTexture& t) { return t.name == texture.name; });
        if (bound == preset.textures.end()) preset.textures.push_back(texture);
        else *bound = texture;
    }

    int count = 0;
    if (!counts.empty()) {
        char* end = NULL;
        const std::string value = trim(counts.front(), " \t;");
        const double parsed = strtod(value.c_str(), &end);   /* "3.0" happens too */
        if (value.empty() || !end || *end != '\0' || parsed < 0) {
            *error = utf8(file) + ": shaders = \"" + counts.front() + "\" is not a count";
            return false;
        }
        count = (int)parsed;
    }
    for (int i = 0; i < count; i++) {
        const auto shader = std::find_if(shaders.begin(), shaders.end(),
                                         [i](const auto& s) { return s.first == i; });
        if (shader == shaders.end()) continue;   /* librashader leaves the pass out */
        SlangPresetPass pass;
        pass.shader = shader->second;
        pass.settings = pass_settings[i];
        preset.passes.push_back(std::move(pass));
    }
    *out = std::move(preset);
    return true;
}

bool slang_preset_write(const std::string& path, const SlangPreset& preset, std::string* error) {
    std::string ignored;
    if (!error) error = &ignored;
    std::error_code ec;
    fs::path file = fs::absolute(fs::u8path(path), ec);
    if (ec) file = fs::u8path(path);
    const fs::path dir = file.parent_path();
    fs::create_directories(dir, ec);

    std::ostringstream out;
    auto line = [&out](const std::string& key, const std::string& value) {
        out << key << " = \"" << value << "\"\n";
    };
    line("shaders", std::to_string(preset.passes.size()));
    for (size_t i = 0; i < preset.passes.size(); i++) {
        const SlangPresetPass& pass = preset.passes[i];
        out << '\n';
        line("shader" + std::to_string(i), relative_to(pass.shader, dir));
        for (const auto& [key, value] : pass.settings) line(key + std::to_string(i), value);
    }
    if (!preset.textures.empty()) {
        std::string names;
        for (const SlangPresetTexture& texture : preset.textures) {
            names += (names.empty() ? "" : ";") + texture.name;
        }
        out << '\n';
        line("textures", names);
        for (const SlangPresetTexture& texture : preset.textures) {
            line(texture.name, relative_to(texture.path, dir));
            for (const auto& [key, value] : texture.settings) line(texture.name + "_" + key, value);
        }
    }
    if (!preset.parameters.empty()) {
        std::string names;
        for (const auto& parameter : preset.parameters) names += (names.empty() ? "" : ";") + parameter.first;
        out << '\n';
        line("parameters", names);
        for (const auto& [name, value] : preset.parameters) line(name, value);
    }

    std::ofstream file_out(file, std::ios::binary | std::ios::trunc);
    if (!file_out || !(file_out << out.str()) || !file_out.flush()) {
        *error = "could not write " + utf8(file);
        return false;
    }
    return true;
}

void slang_preset_combine(SlangPreset* preset, const SlangPreset& other, bool before) {
    if (!preset) return;
    preset->passes.insert(before ? preset->passes.begin() : preset->passes.end(),
                          other.passes.begin(), other.passes.end());
    for (const SlangPresetTexture& texture : other.textures) {
        const bool known = std::any_of(preset->textures.begin(), preset->textures.end(),
                                       [&](const SlangPresetTexture& t) { return t.name == texture.name; });
        if (!known) preset->textures.push_back(texture);
    }
    for (const auto& [name, value] : other.parameters) {
        const bool known = std::any_of(preset->parameters.begin(), preset->parameters.end(),
                                       [&](const auto& p) { return p.first == name; });
        if (!known) preset->parameters.emplace_back(name, value);
    }
}

void slang_preset_set_parameter(SlangPreset* preset, const std::string& name, const std::string& value) {
    if (preset) set_value(preset->parameters, name, value);
}
