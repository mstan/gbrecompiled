#include "recompiler/symbol_table.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace gbrecomp {

namespace {

struct PendingSymbol {
    uint32_t addr;
    std::string source_name;
    std::string comment;
    SymbolType type;
};

std::string trim_copy(const std::string& input) {
    const size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

bool is_c_keyword(const std::string& name) {
    static const std::unordered_set<std::string> keywords = {
        "auto", "break", "case", "char", "const", "continue", "default",
        "do", "double", "else", "enum", "extern", "float", "for", "goto",
        "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch",
        "typedef", "union", "unsigned", "void", "volatile", "while",
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
        "main"
    };
    return keywords.find(name) != keywords.end();
}

bool is_reserved_runtime_name(const std::string& name) {
    static const std::unordered_set<std::string> reserved = {
        "gb_dispatch",
        "gb_dispatch_call",
        "gb_main",
        "gbrt_jump_hl",
    };
    return reserved.find(name) != reserved.end();
}

std::string sanitize_symbol_name(const std::string& name) {
    std::string sanitized;
    sanitized.reserve(name.size() + 8);

    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '_') {
            sanitized.push_back(static_cast<char>(ch));
        } else {
            sanitized.push_back('_');
        }
    }

    if (sanitized.empty()) {
        sanitized = "sym";
    }

    if (!(std::isalpha(static_cast<unsigned char>(sanitized[0])) || sanitized[0] == '_')) {
        sanitized.insert(sanitized.begin(), '_');
    }

    while (sanitized.find("__") != std::string::npos) {
        sanitized.replace(sanitized.find("__"), 2, "_");
    }

    if (sanitized.rfind("sym_", 0) != 0) {
        sanitized = "sym_" + sanitized;
    }

    if (is_c_keyword(sanitized) || is_reserved_runtime_name(sanitized)) {
        sanitized = "sym_" + sanitized;
    }

    return sanitized;
}

std::string make_unique_symbol_name(const std::string& base_name, uint32_t addr,
                                    const std::unordered_map<std::string, uint32_t>& used_names) {
    auto existing = used_names.find(base_name);
    if (existing == used_names.end() || existing->second == addr) {
        return base_name;
    }

    std::ostringstream suffix;
    suffix << base_name
           << "_b"
           << std::hex << std::nouppercase << std::setfill('0') << std::setw(2)
           << static_cast<unsigned>(addr >> 16)
           << "_"
           << std::setw(4)
           << static_cast<unsigned>(addr & 0xFFFF);
    return suffix.str();
}

SymbolType infer_symbol_type(uint32_t addr, const std::string& source_name) {
    const uint16_t offset = static_cast<uint16_t>(addr & 0xFFFF);
    if (offset >= 0x8000) {
        return SymbolType::DATA;
    }
    return (source_name.find('.') == std::string::npos) ? SymbolType::FUNCTION : SymbolType::LABEL;
}

} // namespace

bool SymbolTable::load_sym_file(const std::string& path, std::string* error) {
    clear();

    std::ifstream file(path);
    if (!file) {
        if (error) {
            *error = "Failed to open symbol file: " + path;
        }
        return false;
    }

    std::vector<PendingSymbol> pending;
    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        line_number++;

        const size_t comment_pos = line.find(';');
        const std::string comment = (comment_pos == std::string::npos)
            ? ""
            : trim_copy(line.substr(comment_pos + 1));
        const std::string body = trim_copy(line.substr(0, comment_pos));

        if (body.empty()) {
            continue;
        }

        std::istringstream iss(body);
        std::string addr_token;
        std::string name;
        iss >> addr_token >> name;
        if (addr_token.empty() || name.empty()) {
            continue;
        }

        const size_t colon = addr_token.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        try {
            const uint8_t bank = static_cast<uint8_t>(std::stoul(addr_token.substr(0, colon), nullptr, 16));
            const uint16_t offset = static_cast<uint16_t>(std::stoul(addr_token.substr(colon + 1), nullptr, 16));
            pending.push_back({
                AnalysisResult::make_addr(bank, offset),
                name,
                comment,
                infer_symbol_type(AnalysisResult::make_addr(bank, offset), name),
            });
        } catch (...) {
            if (error) {
                std::ostringstream ss;
                ss << "Malformed symbol file line " << line_number << " in " << path;
                *error = ss.str();
            }
            clear();
            return false;
        }
    }

    std::sort(pending.begin(), pending.end(), [](const PendingSymbol& lhs, const PendingSymbol& rhs) {
        if (lhs.addr != rhs.addr) {
            return lhs.addr < rhs.addr;
        }
        return lhs.source_name < rhs.source_name;
    });

    std::unordered_map<std::string, uint32_t> used_names;
    for (const PendingSymbol& pending_symbol : pending) {
        Symbol symbol;
        symbol.source_name = pending_symbol.source_name;
        symbol.addr = pending_symbol.addr;
        symbol.type = pending_symbol.type;
        symbol.comment = pending_symbol.comment;

        const std::string sanitized = sanitize_symbol_name(pending_symbol.source_name);
        symbol.c_name = make_unique_symbol_name(sanitized, pending_symbol.addr, used_names);

        symbols_[pending_symbol.addr] = symbol;
        used_names[symbol.c_name] = pending_symbol.addr;
    }

    return true;
}

void SymbolTable::clear() {
    symbols_.clear();
}

const Symbol* SymbolTable::get_symbol(uint32_t addr) const {
    const auto it = symbols_.find(addr);
    return (it == symbols_.end()) ? nullptr : &it->second;
}

const Symbol* SymbolTable::get_symbol(uint8_t bank, uint16_t addr) const {
    return get_symbol(AnalysisResult::make_addr(bank, addr));
}

const std::unordered_map<uint32_t, Symbol>& SymbolTable::symbols() const {
    return symbols_;
}

bool SymbolTable::has_symbol(uint32_t addr) const {
    return symbols_.find(addr) != symbols_.end();
}

size_t SymbolTable::size() const {
    return symbols_.size();
}

void apply_symbols_to_analysis(const SymbolTable& symbols, AnalysisResult& analysis) {
    for (const auto& [addr, symbol] : symbols.symbols()) {
        AddressSymbolMetadata metadata;
        metadata.source_name = symbol.source_name;
        metadata.emitted_name = symbol.c_name;
        metadata.provenance = "imported";
        metadata.comment = symbol.comment;

        if (analysis.functions.find(addr) != analysis.functions.end()) {
            metadata.kind = "function";
        } else if (analysis.blocks.find(addr) != analysis.blocks.end() ||
                   analysis.label_addresses.find(addr) != analysis.label_addresses.end()) {
            metadata.kind = "label";
        } else {
            metadata.kind = "data";
        }

        analysis.symbol_metadata[addr] = metadata;
    }

    std::set<uint32_t> ordered_addresses;
    for (const auto& [addr, unused] : analysis.functions) {
        (void)unused;
        ordered_addresses.insert(addr);
    }

    std::unordered_map<std::string, uint32_t> used_names;
    for (uint32_t addr : ordered_addresses) {
        auto func_it = analysis.functions.find(addr);
        if (func_it == analysis.functions.end()) {
            continue;
        }

        Function& func = func_it->second;
        std::string candidate_name = func.name;

        if (const Symbol* symbol = symbols.get_symbol(addr)) {
            candidate_name = symbol->c_name;
        }

        candidate_name = make_unique_symbol_name(candidate_name, addr, used_names);
        func.name = candidate_name;
        used_names[candidate_name] = addr;

        AddressSymbolMetadata& metadata = analysis.symbol_metadata[addr];
        if (metadata.provenance.empty()) {
            metadata.provenance = "autogenerated";
        }
        if (metadata.kind.empty()) {
            metadata.kind = "function";
        }
        metadata.emitted_name = candidate_name;
    }
}

} // namespace gbrecomp
