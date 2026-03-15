#ifndef RECOMPILER_SYMBOL_TABLE_H
#define RECOMPILER_SYMBOL_TABLE_H

#include "analyzer.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace gbrecomp {

enum class SymbolType {
    FUNCTION,
    LABEL,
    DATA,
    UNKNOWN,
};

struct Symbol {
    std::string source_name;
    std::string c_name;
    uint32_t addr;
    SymbolType type;
    std::string comment;
};

class SymbolTable {
public:
    bool load_sym_file(const std::string& path, std::string* error = nullptr);

    void clear();

    const Symbol* get_symbol(uint32_t addr) const;
    const Symbol* get_symbol(uint8_t bank, uint16_t addr) const;
    const std::unordered_map<uint32_t, Symbol>& symbols() const;

    bool has_symbol(uint32_t addr) const;
    size_t size() const;

private:
    std::unordered_map<uint32_t, Symbol> symbols_;
};

void apply_symbols_to_analysis(const SymbolTable& symbols, AnalysisResult& analysis);

} // namespace gbrecomp

#endif // RECOMPILER_SYMBOL_TABLE_H
