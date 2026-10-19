#include "recompiler/analyzer.h"
#include "recompiler/decoder.h"
#include "recompiler/rom.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace gbrecomp;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
int main() {
    std::vector<uint8_t> image(0x8000, 0xd3);
    std::fill(image.begin()+0x134,image.begin()+0x150,0);
    for (unsigned op=0;op<256;++op) {
        image[0x200+op*4]=op;
        image[0x201+op*4]=image[0x202+op*4]=0;
        image[0x1000+op*2]=0xcb;image[0x1001+op*2]=op;
    }
    auto rom=ROM::load_from_buffer(image,"memory metadata fixture");CHECK(rom);
    Decoder decoder(*rom);
    for (unsigned op=0;op<256;++op) {
        bool read=false,write=false;
        if(op>=0x40 && op<0x80 && op!=0x76) { read=(op&7)==6;write=((op>>3)&7)==6; }
        if(op>=0x80 && op<0xc0 && (op&7)==6) read=true;
        for(unsigned code : {0x0a,0x1a,0x2a,0x3a,0xf0,0xf2,0xfa,
                            0xc1,0xd1,0xe1,0xf1,0xc0,0xc8,0xd0,0xd8,0xc9,0xd9})
            read|=op==code;
        for(unsigned code : {0x02,0x12,0x22,0x32,0x08,0x36,0xe0,0xe2,0xea,
                            0xc5,0xd5,0xe5,0xf5,0xc4,0xcc,0xd4,0xdc,0xcd})
            write|=op==code;
        if((op&0xc7)==0xc7) write=true; // RST pushes a return address.
        if(op==0x34 || op==0x35) read=write=true;
        auto decoded=decoder.decode(0x200+op*4,0);
        if(decoded.reads_memory!=read || decoded.writes_memory!=write) {
            std::cerr<<std::hex<<op<<' '<<decoded.disassemble()<<'\n';CHECK(false);
        }
        auto cb=decoder.decode(0x1000+op*2,0);
        CHECK(cb.reads_memory==((op&7)==6));
        CHECK(cb.writes_memory==((op&7)==6 && (op<0x40 || op>=0x80)));
    }
    /* RST-table target whose first control transfer follows eight memory-
     * touching setup instructions. Unset metadata made the finder reject it. */
    image.assign(0x8000,0xd3);
    std::fill(image.begin()+0x134,image.begin()+0x150,0);
    for(unsigned vector=0;vector<=0x60;vector+=8) image[vector]=0xc9;
    const std::vector<uint8_t> rst={0x87,0xe1,0x5f,0x16,0,0x19,0x5e,0x23,0x56,0xd5,0xe1,0xe9};
    std::copy(rst.begin(),rst.end(),image.begin()+0x28);
    image[0x100]=0xc3;image[0x101]=0x80;image[0x102]=1;
    image[0x180]=0xef;image[0x181]=0x22;image[0x182]=3;
    std::vector<uint8_t> setup={0xaf,0xe0,0x40,0xf3,0xe0,0xa4,0x21,0,0xc0,
        0x06,0x9f,0x22,0x05,0x20,0xfc};
    // The initializer continues beyond the old generic scanner's 64-opcode
    // window. A short RET fixture would accidentally pass that fallback.
    for(unsigned i=0;i<70;++i) { setup.push_back(0xe0);setup.push_back(0xb4); }
    setup.push_back(0xc9);
    std::copy(setup.begin(),setup.end(),image.begin()+0x322);
    rom=ROM::load_from_buffer(image,"RST initialization target");CHECK(rom);
    AnalyzerOptions options;options.aggressive_scan=false;
    auto result=analyze(*rom,options);
    CHECK(result.get_instruction(0,0x322));
    CHECK(result.get_instruction(0,0x32f));
    image[0x330]=0xfe; // A branch to itself does not prove the store/count loop.
    rom=ROM::load_from_buffer(std::move(image),"unproven memory prefix");CHECK(rom);
    CHECK(!analyze(*rom,options).get_instruction(0,0x322));
    std::cout<<"all 512 opcode memory classifications and RST initialization target passed\n";
}
