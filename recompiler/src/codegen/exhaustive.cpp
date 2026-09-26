#include "recompiler/codegen/exhaustive.h"
#include "recompiler/rom.h"
#include "gb_sha256.h"
#include <array>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace gbrecomp::codegen {
void append_exhaustive_rom(GeneratedOutput& output, const uint8_t* data, size_t size,
                           const GeneratorOptions& options) {
    if (size < 0x8000 || size > 0x400000 || size % 0x4000 || data[0x147] < 0x19 || data[0x147] > 0x1E)
        throw std::runtime_error("exhaustive_rom currently requires an MBC5 ROM of 2..256 full banks");
    auto rom = ROM::load_from_buffer(std::vector<uint8_t>(data, data + size), options.output_prefix);
    Decoder decoder(*rom);
    const std::string prefix = options.output_prefix + "_exhaustive";
    const std::string preamble = "/* All-ROM native entries, generated ahead of time. */\n#include \"" +
        options.output_prefix + "_internal.h\"\n#include <stdio.h>\n#include <stdlib.h>\n";
    std::vector<std::string> bodies;
    std::unordered_map<std::string, uint32_t> body_ids;
    std::unordered_map<uint64_t, uint32_t> instruction_ids;
    std::ostringstream variants;
    variants << "# id bank address halt_bug live_operands opcode\n";
    auto intern_body = [&](const std::string& body, const Instruction& ins, bool bug, bool live) {
        auto [it, inserted] = body_ids.emplace(body, uint32_t(bodies.size()));
        if (inserted) {
            bodies.push_back(body);
            variants << it->second << ' ' << unsigned(ins.bank) << ' ' << ins.address << ' '
                     << bug << ' ' << live << ' ' << unsigned(ins.opcode) << '\n';
        }
        return it->second;
    };
    // A CB second byte at $3FFF/$7FFF is fetched from a different memory window.
    // Precompile all 256 possibilities; the boundary wrapper only selects them.
    std::array<uint32_t, 256> cb_ids;
    for (unsigned cb = 0; cb < 256; ++cb) {
        uint8_t bytes[3] = {0xCB, uint8_t(cb), 0};
        auto ins = decoder.decode_bytes(0, 0, bytes);
        cb_ids[cb] = intern_body(emit_exhaustive_instruction(ins, options, false, false), ins, false, false);
    }
    const std::string cb_boundary = "    " + prefix + "_cb_boundary(ctx);\n";
    auto instruction_id = [&](const Instruction& ins, const uint8_t bytes[3], bool bug, bool live) {
        uint64_t key = uint64_t(bytes[0]) | (uint64_t(bug) << 24) | (uint64_t(live) << 25);
        if (!live && ins.length > 1) key |= uint64_t(bytes[1]) << 8;
        if (!live && ins.length > 2) key |= uint64_t(bytes[2]) << 16;
        const uint32_t source = (uint32_t(ins.bank) << 16) | ins.address;
        if (options.imm_override_sites.count(source)) key |= (uint64_t(source) + 1) << 32;
        auto found = instruction_ids.find(key);
        if (found != instruction_ids.end()) return found->second;
        auto body = ins.is_cb_prefixed && live ? cb_boundary : emit_exhaustive_instruction(ins, options, bug, live);
        uint32_t id = intern_body(body, ins, bug, live);
        instruction_ids.emplace(key, id);
        return id;
    };
    size_t illegal = 0;
    const unsigned banks = unsigned(size / 0x4000);
    std::ostringstream directory, report;
    directory << preamble;
    directory << "void " << prefix << "_invoke(GBContext* ctx, uint32_t id);\n";
    // Window 0 is fixed ROM; window bank+1 is the switchable ROM window,
    // including MBC5's legal bank-zero mapping.
    for (unsigned window = 0; window <= banks; ++window) {
        unsigned bank = window ? window - 1 : 0;
        unsigned base = window ? 0x4000 : 0;
        std::ostringstream table, listing;
        table << preamble << "const uint32_t " << prefix << "_map_" << window << "[32768] = {\n";
        listing << "; Conservative disassembly at EVERY byte; entries overlap and include data.\n";
        size_t bank_illegal = 0;
        for (unsigned bug = 0; bug < 2; ++bug) {
            for (unsigned off = 0; off < 0x4000; ++off) {
                size_t physical = bank * 0x4000 + off;
                uint8_t bytes[3] = {data[physical], 0, 0};
                for (unsigned j = 1; j < 3; ++j) {
                    unsigned delta = j - bug;
                    if (off + delta < 0x4000) bytes[j] = data[physical + delta];
                }
                auto ins = decoder.decode_bytes(uint16_t(base + off), uint8_t(bank), bytes);
                bool live = off + ins.length - bug > 0x4000 && ins.length > 1;
                uint32_t id = instruction_id(ins, bytes, bug, live);
                table << id << ',';
                if (off % 16 == 15) table << '\n';
                if (!bug) {
                    if (ins.type == InstructionType::UNDEFINED) ++bank_illegal;
                    listing << std::hex << std::setfill('0') << std::setw(2) << bank << ':'
                            << std::setw(4) << (base + off) << "  " << std::setw(2) << unsigned(bytes[0])
                            << "  " << ins.disassemble() << (live ? " ; operands fetched across window" : "") << '\n';
                }
            }
        }
        table << "};\n";
        output.extra_files.push_back({prefix + "_map_" + std::to_string(window) + ".c", table.str(), true});
        output.extra_files.push_back({prefix + "_bank_" + std::to_string(window) + ".asm", listing.str(), false});
        directory << "extern const uint32_t " << prefix << "_map_" << window << "[32768];\n";
        if (window != 1) illegal += bank_illegal; // Count physical bank zero once.
    }
    directory << "static const uint32_t* const maps[] = {\n";
    for (unsigned w = 0; w <= banks; ++w) directory << "    " << prefix << "_map_" << w << ",\n";
    directory << "};\n";
    directory << "uint32_t " << prefix << "_id(unsigned bank, unsigned pc, unsigned bug) {\n"
              << "    unsigned window = pc < 0x4000 ? 0 : bank + 1;\n"
              << "    if (pc >= 0x8000 || window > " << banks << ") return UINT32_MAX;\n"
              << "    return maps[window][(bug ? 16384 : 0) + (pc & 0x3FFF)];\n}\n";
    directory << "void " << prefix << "_step(GBContext* ctx) {\n"
              << "    uint32_t id = " << prefix << "_id(ctx->rom_bank, ctx->pc, ctx->halt_bug);\n"
              << "    if (id == UINT32_MAX) { fprintf(stderr, \"[NATIVE] Unmapped ROM %03X:%04X\\n\", ctx->rom_bank, ctx->pc); abort(); }\n"
              << "    " << prefix << "_invoke(ctx, id);\n}\n";
    directory << "void " << options.output_prefix << "_uncovered(GBContext* ctx) {\n"
              << "    if (ctx->pc < 0x8000) { " << prefix << "_step(ctx); return; }\n"
              << "    fprintf(stderr, \"[NATIVE] Uncompiled writable-memory execution %03X:%04X; interpreter disabled\\n\", ctx->rom_bank, ctx->pc); abort();\n}\n";
    directory << "void " << prefix << "_cb_boundary(GBContext* ctx) {\n"
              << "    static const uint32_t ids[256] = {\n";
    for (auto id : cb_ids) directory << id << ',';
    directory << "\n    };\n    " << prefix << "_invoke(ctx, ids[gb_read8(ctx, ctx->pc + 1)]);\n}\n";
    output.extra_files.push_back({prefix + "_maps.c", directory.str(), true});
    std::ostringstream invoke;
    invoke << preamble;
    const size_t chunk_size = 512;
    for (size_t begin = 0; begin < bodies.size(); begin += chunk_size) {
        size_t end = std::min(begin + chunk_size, bodies.size());
        std::string group = prefix + "_group_" + std::to_string(begin / chunk_size);
        std::ostringstream code;
        code << preamble << "void " << prefix << "_cb_boundary(GBContext* ctx);\n";
        for (size_t id = begin; id < end; ++id)
            code << "static void native_" << id << "(GBContext* ctx) {\n" << bodies[id] << "}\n";
        code << "void " << group << "(GBContext* ctx, unsigned id) {\n"
             << "    static void (*const entries[])(GBContext*) = {\n";
        for (size_t id = begin; id < end; ++id) code << "native_" << id << ',';
        code << "\n    };\n    entries[id](ctx);\n}\n";
        output.extra_files.push_back({group + ".c", code.str(), true});
        invoke << "void " << group << "(GBContext* ctx, unsigned id);\n";
    }
    invoke << "void " << prefix << "_invoke(GBContext* ctx, uint32_t id) {\n"
           << "    static void (*const groups[])(GBContext*, unsigned) = {\n";
    for (size_t begin = 0; begin < bodies.size(); begin += chunk_size)
        invoke << prefix << "_group_" << begin / chunk_size << ',';
    invoke << "\n    };\n    groups[id / 512](ctx, id % 512);\n}\n";
    output.extra_files.push_back({prefix + "_invoke.c", invoke.str(), true});
    output.extra_files.push_back({prefix + "_variants.tsv", variants.str(), false});
    char digest[65];
    gb_sha256_hex(data, size, digest);
    report << "{\n  \"rom_sha256\": \"" << digest << "\",\n  \"banks\": " << banks << ",\n  \"rom_bytes\": " << size
           << ",\n  \"mapped_pc_slots\": " << (banks + 1) * 16384
           << ",\n  \"halt_bug_variants\": true,\n  \"native_bodies\": " << bodies.size()
           << ",\n  \"illegal_opcode_positions\": " << illegal
           << ",\n  \"missing_rom_entries\": 0,\n  \"unknown_ram_policy\": \"fatal diagnostic, never interpreter\"\n}\n";
    output.extra_files.push_back({prefix + ".json", report.str(), false});
    std::cout << "Exhaustive ROM: " << banks << " banks, " << (banks + 1) * 16384
              << " PC slots plus HALT-bug variants, " << bodies.size() << " native instruction bodies\n";
}
}
