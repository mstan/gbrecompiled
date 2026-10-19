#include "recompiler/ram_overlay.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using namespace gbrecomp;
static std::vector<uint8_t> fixture(bool de_copy = false, bool include_di = true) {
    std::vector<uint8_t> image(0x8000);
    const std::vector<uint8_t> dma = {0xf3,0x3e,0xc2,0xe0,0x46,0x3e,0x28,0x3d,0x20,0xfd,0xd9};
    std::copy(dma.begin(), dma.end(), image.begin()+0x600);
    std::vector<uint8_t> helper = de_copy ?
        std::vector<uint8_t>{0x2a,0x12,0x13,0x0b,0x78,0xb1,0x20,0xf8,0xc9} :
        std::vector<uint8_t>{0x2a,0xe2,0x0c,0x05,0x20,0xfa,0xc9};
    std::copy(helper.begin(),helper.end(),image.begin()+0x400);
    uint8_t source = include_di ? 0 : 1;
    uint8_t count = include_di ? 11 : 10;
    std::vector<uint8_t> setup = de_copy ?
        std::vector<uint8_t>{0x21,source,0x06,0x11,0x80,0xff,0x01,count,0,0xcd,0,4} :
        std::vector<uint8_t>{0x21,source,0x06,0x06,count,0x0e,0x80,0xcd,0,4};
    std::copy(setup.begin(),setup.end(),image.begin()+0x200);
    return image;
}
static auto discover(std::vector<uint8_t> image) {
    auto rom = ROM::load_from_buffer(std::move(image), "copy fixture");
    CHECK(rom && rom->is_valid());
    return detect_oam_dma_overlays(*rom);
}
int main() {
    for (bool de : {false,true}) for (bool di : {false,true}) {
        auto result=discover(fixture(de,di));
        CHECK(result.size()==1 && result[0].ram_addr==0xff80);
        CHECK(result[0].rom_addr==(di ? 0x600u : 0x601u));
        CHECK(result[0].size==(di ? 11 : 10));
    }
    auto wrong_count=fixture();wrong_count[0x204]=9;
    CHECK(discover(wrong_count).empty());
    auto padded_copy=fixture();padded_copy[0x204]=12;
    auto padded=discover(padded_copy);
    CHECK(padded.size()==1 && padded[0].size==11 && padded[0].ram_addr==0xff80);
    auto wrong_source=fixture();wrong_source[0x201]=7;
    CHECK(discover(wrong_source).empty());
    auto wrong_helper=fixture();wrong_helper[0x402]=0;
    CHECK(discover(wrong_helper).empty());
    auto io_target=fixture();io_target[0x206]=0x10;
    CHECK(discover(io_target).empty());
    auto truncated=fixture();truncated[0x20]=0xe0;truncated[0x21]=0x46;
    CHECK(discover(truncated).size()==1);
    auto inline_copy=fixture();
    std::fill(inline_copy.begin()+0x200,inline_copy.begin()+0x210,0);
    const std::vector<uint8_t> copy={0x0e,0xa0,0x06,11,0x21,0,6,0x2a,0xe2,0x0c,0x05,0x20,0xfa,0xcd,0,4};
    std::copy(copy.begin(),copy.end(),inline_copy.begin()+0x200);
    auto result=discover(inline_copy);
    CHECK(result.size()==1 && result[0].ram_addr==0xffa0 && result[0].size==11);
    inline_copy[0x201]=0x10;
    CHECK(discover(inline_copy).empty());
    inline_copy[0x201]=0xa0;
    inline_copy[0x203]=10;
    result=discover(inline_copy);
    CHECK(result.empty());
    std::cout << "bounded C/B and DE/BC copies, DI prefix, RETI, wrong-copy and IO rejection passed\n";
}
