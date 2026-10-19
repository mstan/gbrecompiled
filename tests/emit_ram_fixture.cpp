#include "recompiler/rom.h"
#include "recompiler/analyzer.h"
#include "recompiler/ir/ir_builder.h"
#include "recompiler/codegen/c_emitter.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    std::vector<uint8_t> image(0x8000, 0xff);
    std::fill(image.begin()+0x134, image.begin()+0x150, 0);
    image[0x100] = 0x76; // HALT at the synthetic cartridge return address
    for (size_t vector : {0x40,0x48,0x50,0x58,0x60}) image[vector] = 0xc9;
    const std::vector<uint8_t> helper = {0xf3,0x3e,0xc2,0xe0,0x46,0x3e,0x28,0x3d,0x20,0xfd,0xd9};
    std::copy(helper.begin(), helper.end(), image.begin()+0x600);
    image[0x700]=0x3e;image[0x701]=0xff;
    auto rom = gbrecomp::ROM::load_from_buffer(std::move(image), "native RAM fixture");
    if (!rom || !rom->is_valid()) return 1;
    gbrecomp::AnalyzerOptions analysis_options;
    analysis_options.aggressive_scan = false;
    analysis_options.ram_overlays.push_back({0xff80,0x600,(uint16_t)helper.size()});
    analysis_options.ram_overlays.push_back({0xff90,0x700,1});
    // Exercise duplicate RAM labels discovered from differently banked callers.
    analysis_options.entry_points = {(10u<<16)|0xff87u,(43u<<16)|0xff88u};
    auto analysis = gbrecomp::analyze(*rom, analysis_options);
    if (analysis.get_instruction(0,0xff90)) return 1; // Operand was never copied.
    gbrecomp::ir::IRBuilder builder;
    auto program = builder.build(analysis, rom->name());
    gbrecomp::codegen::GeneratorOptions options;
    options.output_prefix = "ram_fixture";
    options.output_dir = argv[1];
    options.emit_main_entry_point = false;
    options.emit_cmake = false;
    options.parallel_codegen_jobs = 1;
    options.ram_overlays.push_back({0xff80, helper});
    auto other_image = helper;
    other_image[2] = 0xc3;
    options.ram_overlays.push_back({0xff80, other_image});
    options.ram_overlays.push_back({0xff90,{0x3e}});
    auto output = gbrecomp::codegen::generate_output(program, rom->data(), rom->size(), options);
    if (!gbrecomp::codegen::write_output(output, options.output_dir)) return 1;
    std::ofstream native(std::filesystem::path(options.output_dir)/"ram_fixture_native.c");
    native << output.source_content << '\n' << output.rom_data_content << '\n';
    for (const auto& [bank, source] : output.bank_sources) native << source << '\n';
    for (const auto& extra : output.extra_files) if (extra.is_source) native << extra.content << '\n';
    return native ? 0 : 1;
}
