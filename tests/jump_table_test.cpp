#include "recompiler/analyzer.h"
#include "recompiler/rom.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace gbrecomp;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
static std::vector<uint8_t> fixture(unsigned bank) {
    std::vector<uint8_t> image(0x10000, 0xd3);
    std::fill(image.begin()+0x134,image.begin()+0x150,0);
    image[0x147]=0x19;image[0x148]=1;
    image[0x100]=0x76;
    for (unsigned vector=0;vector<=0x60;vector+=8) image[vector]=0xc9;
    image[0x10]=0xe9;
    const std::vector<uint8_t> dispatcher={0xfa,0,0xc0,0x4f,0x06,0,0x21,0x20,0x40,0x09,0x2a,0x66,0x6f,0xd7,0x18,0x0e};
    size_t physical=bank*0x4000;
    std::copy(dispatcher.begin(),dispatcher.end(),image.begin()+physical);
    image[physical+0x1e]=0x18;image[physical+0x1f]=6;
    const std::vector<uint8_t> table={0x30,0x40,0x34,0x40,0x26,0x40};
    std::copy(table.begin(),table.end(),image.begin()+physical+0x20);
    image[physical+0x26]=0xc9;
    image[physical+0x30]=0x3e;image[physical+0x31]=0x11;image[physical+0x32]=0xc9;
    image[physical+0x34]=0x3e;image[physical+0x35]=0x22;image[physical+0x36]=0xc9;
    return image;
}
static AnalysisResult discover(std::vector<uint8_t> image,unsigned bank,bool aggressive=false) {
    auto rom=ROM::load_from_buffer(std::move(image),"branch bounded table fixture");
    CHECK(rom && rom->is_valid());
    AnalyzerOptions options;options.aggressive_scan=aggressive;
    options.entry_points={AnalysisResult::make_addr(bank,0x4000)};
    auto result=analyze(*rom,options);
    result.rom=nullptr;
    return result;
}
int main() {
    for(unsigned bank : {1u,2u}) {
        auto result=discover(fixture(bank),bank);
        for(unsigned target : {0x4026u,0x4030u,0x4034u}) {
            CHECK(result.computed_jump_targets.count(AnalysisResult::make_addr(bank,target)));
            CHECK(result.get_instruction(bank,target));
        }
        CHECK(!result.get_instruction(bank,0x4020));
        auto jp=fixture(bank);size_t p=bank*0x4000;
        jp[p+0x0f]=0x0d;jp[p+0x1d]=0xc3;jp[p+0x1e]=0x26;jp[p+0x1f]=0x40;
        CHECK(discover(jp,bank).computed_jump_targets.size()==3);
        auto odd=fixture(bank);odd[p+0x1f]=5;
        CHECK(discover(odd,bank).computed_jump_targets.empty());
        auto conditional=fixture(bank);conditional[p+0x1e]=0x20;
        CHECK(discover(conditional,bank).computed_jump_targets.empty());
        auto wrong_load=fixture(bank);wrong_load[p+0x0b]=0x6e;
        CHECK(discover(wrong_load,bank).computed_jump_targets.empty());
        auto wrong_trampoline=fixture(bank);wrong_trampoline[0x10]=0xc9;
        CHECK(discover(wrong_trampoline,bank).computed_jump_targets.empty());
        auto invalid=fixture(bank);invalid[p+0x23]=0xff;
        CHECK(discover(invalid,bank).computed_jump_targets.empty());
    }
    auto repeated=fixture(1);
    const std::vector<uint8_t> unreferenced={0xf5,0xc5,0xd5,0xe5,
        0x3e,0x55,0x06,1,0x0e,2,0x16,3,0x1e,4,0x26,5,0x2e,6,
        0x80,0x81,0x82,0x83,0x84,0x85,0x90,0x91,0x92,0x93,0x94,0x95,
        0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,
        0xe1,0xd1,0xc1,0xf1,0xc9};
    std::copy(unreferenced.begin(),unreferenced.end(),repeated.begin()+0x4800);
    auto first=discover(repeated,1,true);
    auto second=discover(repeated,1,true);
    CHECK(first.get_instruction(1,0x4800));
    CHECK(second.get_instruction(1,0x4800));
    CHECK(first.addr_to_index.size()==second.addr_to_index.size());
    std::cout << "decoded JR/JP bounds, all targets, bank identity and malformed table rejection passed\n";
}
