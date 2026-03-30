#include "recompiler/symbol_table.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
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
    bool has_explicit_annotation = false;
    AnalysisAnnotation explicit_annotation;
};

struct SymbolReferenceContext {
    std::set<uint32_t> direct_call_targets;
    std::set<uint32_t> direct_jump_targets;
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

unsigned detect_parse_base(const std::string& token, unsigned fallback_base) {
    if (token.size() > 2 && token[0] == '0' &&
        (token[1] == 'x' || token[1] == 'X')) {
        return 0;
    }
    return fallback_base;
}

bool parse_u32_token(const std::string& token, unsigned fallback_base, uint32_t& value) {
    try {
        size_t consumed = 0;
        unsigned long parsed = std::stoul(token, &consumed, detect_parse_base(token, fallback_base));
        if (consumed != token.size()) {
            return false;
        }
        value = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_banked_address(const std::string& token, uint32_t& addr) {
    const size_t colon = token.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    uint32_t bank = 0;
    uint32_t offset = 0;
    if (!parse_u32_token(token.substr(0, colon), 16, bank) ||
        !parse_u32_token(token.substr(colon + 1), 16, offset) ||
        bank > 0xFF || offset > 0xFFFF) {
        return false;
    }

    addr = AnalysisResult::make_addr(static_cast<uint8_t>(bank), static_cast<uint16_t>(offset));
    return true;
}

uint8_t infer_reference_target_bank(uint8_t source_bank, uint16_t target) {
    if (target < 0x4000) {
        return 0;
    }
    return (source_bank > 0) ? source_bank : static_cast<uint8_t>(1);
}

void seed_builtin_function_targets(SymbolReferenceContext& context) {
    context.direct_call_targets.insert(AnalysisResult::make_addr(0, 0x0100));
    for (uint16_t vector : {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
                            0x40, 0x48, 0x50, 0x58, 0x60}) {
        context.direct_call_targets.insert(AnalysisResult::make_addr(0, vector));
    }
}

SymbolReferenceContext build_symbol_reference_context(const ROM& rom) {
    SymbolReferenceContext context;
    seed_builtin_function_targets(context);

    for (uint16_t bank_index = 0; bank_index < rom.bank_count(); ++bank_index) {
        const uint8_t bank = static_cast<uint8_t>(bank_index);
        for (const Instruction& instr : decode_bank(rom, bank)) {
            if (instr.is_call) {
                if (instr.type == InstructionType::RST) {
                    context.direct_call_targets.insert(AnalysisResult::make_addr(0, instr.rst_vector));
                    continue;
                }

                if (instr.type == InstructionType::CALL_NN ||
                    instr.type == InstructionType::CALL_CC_NN) {
                    const uint8_t target_bank = infer_reference_target_bank(bank, instr.imm16);
                    context.direct_call_targets.insert(
                        AnalysisResult::make_addr(target_bank, instr.imm16));
                }
                continue;
            }

            if (!instr.is_jump) {
                continue;
            }

            if (instr.type == InstructionType::JP_NN ||
                instr.type == InstructionType::JP_CC_NN) {
                const uint8_t target_bank = infer_reference_target_bank(bank, instr.imm16);
                context.direct_jump_targets.insert(
                    AnalysisResult::make_addr(target_bank, instr.imm16));
            } else if (instr.type == InstructionType::JR_N ||
                       instr.type == InstructionType::JR_CC_N) {
                const int32_t target = static_cast<int32_t>(instr.address) +
                                       static_cast<int32_t>(instr.length) +
                                       static_cast<int32_t>(instr.offset);
                if (target >= 0 && target <= 0xFFFF) {
                    context.direct_jump_targets.insert(
                        AnalysisResult::make_addr(bank, static_cast<uint16_t>(target)));
                }
            }
        }
    }

    return context;
}

std::string strip_comment_directives(const std::string& comment) {
    std::istringstream iss(comment);
    std::ostringstream cleaned;
    std::string token;
    bool first = true;
    while (iss >> token) {
        if (token == "@function" || token == "@label" || token == "@data" ||
            token.rfind("@size=", 0) == 0) {
            continue;
        }
        if (!first) {
            cleaned << ' ';
        }
        cleaned << token;
        first = false;
    }
    return cleaned.str();
}

void apply_comment_directives(const std::string& comment,
                              PendingSymbol& pending_symbol,
                              bool* directive_error) {
    std::istringstream iss(comment);
    std::string token;
    while (iss >> token) {
        if (token == "@function") {
            pending_symbol.type = SymbolType::FUNCTION;
            pending_symbol.has_explicit_annotation = true;
            pending_symbol.explicit_annotation.kind = AnalysisAnnotationKind::FUNCTION;
            pending_symbol.explicit_annotation.size = 1;
            continue;
        }
        if (token == "@label") {
            pending_symbol.type = SymbolType::LABEL;
            pending_symbol.has_explicit_annotation = true;
            pending_symbol.explicit_annotation.kind = AnalysisAnnotationKind::LABEL;
            pending_symbol.explicit_annotation.size = 1;
            continue;
        }
        if (token == "@data") {
            pending_symbol.type = SymbolType::DATA;
            pending_symbol.has_explicit_annotation = true;
            pending_symbol.explicit_annotation.kind = AnalysisAnnotationKind::DATA;
            if (pending_symbol.explicit_annotation.size == 0) {
                pending_symbol.explicit_annotation.size = 1;
            }
            continue;
        }
        if (token.rfind("@size=", 0) == 0) {
            uint32_t size = 0;
            if (!parse_u32_token(token.substr(6), 10, size) || size == 0 || size > 0xFFFF) {
                *directive_error = true;
                return;
            }
            pending_symbol.has_explicit_annotation = true;
            pending_symbol.explicit_annotation.size = size;
            if (pending_symbol.explicit_annotation.kind != AnalysisAnnotationKind::FUNCTION &&
                pending_symbol.explicit_annotation.kind != AnalysisAnnotationKind::LABEL) {
                pending_symbol.explicit_annotation.kind = AnalysisAnnotationKind::DATA;
            }
        }
    }
}

SymbolType infer_symbol_type(uint32_t addr,
                             const std::string& source_name,
                             const SymbolReferenceContext* context) {
    const uint16_t offset = static_cast<uint16_t>(addr & 0xFFFF);
    if (offset >= 0x8000) {
        return SymbolType::DATA;
    }

    if (source_name.find('.') != std::string::npos) {
        return SymbolType::LABEL;
    }

    if (context == nullptr) {
        return SymbolType::FUNCTION;
    }

    // When we have the ROM, only trust ROM-space globals as hard function
    // entries if they already look like real branch/call targets.
    if (context->direct_call_targets.count(addr) > 0) {
        return SymbolType::FUNCTION;
    }

    if (context->direct_jump_targets.count(addr) > 0) {
        return SymbolType::LABEL;
    }

    return SymbolType::UNKNOWN;
}

void upsert_symbol(std::unordered_map<uint32_t, Symbol>& symbols,
                   const PendingSymbol& pending_symbol,
                   std::unordered_map<std::string, uint32_t>& used_names) {
    Symbol symbol;
    symbol.source_name = pending_symbol.source_name;
    symbol.addr = pending_symbol.addr;
    symbol.type = pending_symbol.type;
    symbol.comment = pending_symbol.comment;

    const std::string sanitized = sanitize_symbol_name(pending_symbol.source_name);
    symbol.c_name = make_unique_symbol_name(sanitized, pending_symbol.addr, used_names);

    symbols[pending_symbol.addr] = symbol;
    used_names[symbol.c_name] = pending_symbol.addr;
}

} // namespace

bool SymbolTable::load_sym_file(const std::string& path,
                                const ROM* rom,
                                std::string* error) {
    clear();

    std::ifstream file(path);
    if (!file) {
        if (error) {
            *error = "Failed to open symbol file: " + path;
        }
        return false;
    }

    std::vector<PendingSymbol> pending;
    const SymbolReferenceContext reference_context = rom != nullptr
        ? build_symbol_reference_context(*rom)
        : SymbolReferenceContext{};

    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        line_number++;

        const size_t comment_pos = line.find(';');
        const std::string raw_comment = (comment_pos == std::string::npos)
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

        uint32_t addr = 0;
        if (!parse_banked_address(addr_token, addr)) {
            if (error) {
                std::ostringstream ss;
                ss << "Malformed symbol file line " << line_number << " in " << path;
                *error = ss.str();
            }
            clear();
            return false;
        }

        PendingSymbol pending_symbol;
        pending_symbol.addr = addr;
        pending_symbol.source_name = name;
        pending_symbol.comment = strip_comment_directives(raw_comment);
        pending_symbol.type = infer_symbol_type(addr, name, rom != nullptr ? &reference_context : nullptr);
        pending_symbol.explicit_annotation.addr = addr;
        pending_symbol.explicit_annotation.size = 1;

        bool directive_error = false;
        apply_comment_directives(raw_comment, pending_symbol, &directive_error);
        if (directive_error) {
            if (error) {
                std::ostringstream ss;
                ss << "Malformed symbol directive on line " << line_number << " in " << path;
                *error = ss.str();
            }
            clear();
            return false;
        }

        pending.push_back(std::move(pending_symbol));
    }

    std::sort(pending.begin(), pending.end(), [](const PendingSymbol& lhs, const PendingSymbol& rhs) {
        if (lhs.addr != rhs.addr) {
            return lhs.addr < rhs.addr;
        }
        return lhs.source_name < rhs.source_name;
    });

    std::unordered_map<std::string, uint32_t> used_names;
    for (const PendingSymbol& pending_symbol : pending) {
        upsert_symbol(symbols_, pending_symbol, used_names);
        if (pending_symbol.has_explicit_annotation) {
            annotations_.push_back(pending_symbol.explicit_annotation);
        }
    }

    return true;
}

bool SymbolTable::load_annotation_file(const std::string& path, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error) {
            *error = "Failed to open annotation file: " + path;
        }
        return false;
    }

    std::unordered_map<std::string, uint32_t> used_names;
    for (const auto& [addr, symbol] : symbols_) {
        used_names[symbol.c_name] = addr;
    }

    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        line_number++;
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed.rfind("//", 0) == 0) {
            continue;
        }

        const size_t comment_pos = trimmed.find(';');
        const std::string comment = (comment_pos == std::string::npos)
            ? ""
            : trim_copy(trimmed.substr(comment_pos + 1));
        const std::string body = trim_copy(trimmed.substr(0, comment_pos));
        if (body.empty()) {
            continue;
        }

        std::istringstream iss(body);
        std::string kind_token;
        std::string addr_token;
        std::string third_token;
        std::string fourth_token;
        iss >> kind_token >> addr_token >> third_token >> fourth_token;
        if (kind_token.empty() || addr_token.empty()) {
            if (error) {
                std::ostringstream ss;
                ss << "Malformed annotation line " << line_number << " in " << path;
                *error = ss.str();
            }
            return false;
        }

        uint32_t addr = 0;
        if (!parse_banked_address(addr_token, addr)) {
            if (error) {
                std::ostringstream ss;
                ss << "Malformed banked address on line " << line_number << " in " << path;
                *error = ss.str();
            }
            return false;
        }

        AnalysisAnnotation annotation;
        annotation.addr = addr;
        annotation.size = 1;

        PendingSymbol pending_symbol;
        pending_symbol.addr = addr;
        pending_symbol.comment = comment;

        if (kind_token == "function") {
            annotation.kind = AnalysisAnnotationKind::FUNCTION;
            pending_symbol.type = SymbolType::FUNCTION;
            pending_symbol.source_name = third_token;
        } else if (kind_token == "label") {
            annotation.kind = AnalysisAnnotationKind::LABEL;
            pending_symbol.type = SymbolType::LABEL;
            pending_symbol.source_name = third_token;
        } else if (kind_token == "data") {
            uint32_t size = 0;
            if (third_token.empty() || !parse_u32_token(third_token, 10, size) ||
                size == 0 || size > 0xFFFF) {
                if (error) {
                    std::ostringstream ss;
                    ss << "Malformed data size on line " << line_number << " in " << path;
                    *error = ss.str();
                }
                return false;
            }
            annotation.kind = AnalysisAnnotationKind::DATA;
            annotation.size = size;
            pending_symbol.type = SymbolType::DATA;
            pending_symbol.source_name = fourth_token;
        } else {
            if (error) {
                std::ostringstream ss;
                ss << "Unknown annotation kind '" << kind_token << "' on line "
                   << line_number << " in " << path;
                *error = ss.str();
            }
            return false;
        }

        annotations_.push_back(annotation);

        if (!pending_symbol.source_name.empty()) {
            upsert_symbol(symbols_, pending_symbol, used_names);
        }
    }

    return true;
}

void SymbolTable::clear() {
    symbols_.clear();
    annotations_.clear();
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

const std::vector<AnalysisAnnotation>& SymbolTable::annotations() const {
    return annotations_;
}

bool SymbolTable::has_symbol(uint32_t addr) const {
    return symbols_.find(addr) != symbols_.end();
}

size_t SymbolTable::size() const {
    return symbols_.size();
}

size_t SymbolTable::annotation_count() const {
    return annotations_.size();
}

std::vector<AnalysisAnnotation> build_analysis_annotations(const SymbolTable& symbols) {
    std::map<std::pair<uint32_t, int>, AnalysisAnnotation> merged;

    for (const AnalysisAnnotation& annotation : symbols.annotations()) {
        merged[{annotation.addr, static_cast<int>(annotation.kind)}] = annotation;
    }

    for (const auto& [addr, symbol] : symbols.symbols()) {
        AnalysisAnnotation annotation;
        annotation.addr = addr;
        annotation.size = 1;
        switch (symbol.type) {
            case SymbolType::FUNCTION:
                annotation.kind = AnalysisAnnotationKind::FUNCTION;
                break;
            case SymbolType::DATA:
                annotation.kind = AnalysisAnnotationKind::DATA;
                break;
            case SymbolType::LABEL:
            case SymbolType::UNKNOWN:
            default:
                annotation.kind = AnalysisAnnotationKind::LABEL;
                break;
        }
        const auto key = std::make_pair(annotation.addr, static_cast<int>(annotation.kind));
        if (merged.find(key) == merged.end()) {
            merged[key] = annotation;
        }
    }

    std::vector<AnalysisAnnotation> annotations;
    annotations.reserve(merged.size());
    for (const auto& [unused_key, annotation] : merged) {
        (void)unused_key;
        annotations.push_back(annotation);
    }

    return annotations;
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
