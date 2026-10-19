#include "recompiler/ram_overlay.h"
#include <algorithm>

namespace gbrecomp {

static std::vector<std::pair<uint16_t,uint16_t>> find_oam_dma_overlay_copies(const std::vector<uint8_t>& data,
                                                               uint16_t source_addr,
                                                               uint16_t overlay_size) {
    std::vector<std::pair<uint16_t,uint16_t>> destinations;
    if (overlay_size == 0 || overlay_size > 0xFF || data.size() < 13) {
        return destinations;
    }
    auto fits_copy = [&](uint16_t destination, uint32_t count) {
        return count >= overlay_size && destination >= 0xff80 &&
            count <= 0xffffu - destination &&
            count <= (source_addr < 0x4000 ? 0x4000u : 0x8000u) - source_addr;
    };

    for (size_t i = 0; i + 12 < data.size(); ++i) {
        if (data[i] != 0x0E ||       // LD C,imm8
            data[i + 2] != 0x06 ||   // LD B,imm8
            data[i + 4] != 0x21 ||   // LD HL,imm16
            data[i + 7] != 0x2A ||   // LDI A,(HL)
            data[i + 8] != 0xE2 ||   // LD (C),A
            data[i + 9] != 0x0C ||   // INC C
            data[i + 10] != 0x05 ||  // DEC B
            data[i + 11] != 0x20 ||  // JR NZ,rel8
            data[i + 12] != 0xFA) {  // rel8 = -6; next instruction need not be RET
            continue;
        }

        uint8_t count = data[i + 3];
        uint16_t copied_source = static_cast<uint16_t>(data[i + 5] | (data[i + 6] << 8));
        uint16_t destination = static_cast<uint16_t>(0xff00 | data[i + 1]);
        if (copied_source != source_addr || !fits_copy(destination, count) ||
            i / 0x4000 != (i + 12) / 0x4000) {
            continue;
        }

        destinations.push_back({destination,count});
    }

    // Also accept calls to an independently verified bounded copy loop. Load
    // order may vary; only constant register setup is followed, never a guessed
    // instruction count or a heuristic table end. Match the actual source and
    // exact copied byte count before creating an alias.
    const std::vector<uint8_t> copy_c_b = {0x2A,0xE2,0x0C,0x05,0x20,0xFA,0xC9};
    const std::vector<uint8_t> copy_de_bc = {0x2A,0x12,0x13,0x0B,0x78,0xB1,0x20,0xF8,0xC9};
    for (size_t start = 0; start < data.size(); ++start) {
        int b=-1,c=-1,d=-1,e=-1,h=-1,l=-1;
        size_t pc = start;
        while (pc + 2 < data.size() && pc - start < 18 && pc / 0x4000 == start / 0x4000) {
            uint8_t opcode = data[pc++];
            switch (opcode) {
                case 0x06: b=data[pc++]; continue;
                case 0x0E: c=data[pc++]; continue;
                case 0x01: c=data[pc++]; b=data[pc++]; continue;
                case 0x11: e=data[pc++]; d=data[pc++]; continue;
                case 0x21: l=data[pc++]; h=data[pc++]; continue;
                default: break;
            }
            if (opcode != 0xCD || h < 0 || l < 0 || b < 0 || c < 0 ||
                ((h << 8) | l) != source_addr) break;
            uint16_t target = static_cast<uint16_t>(data[pc] | (data[pc + 1] << 8));
            size_t physical = target < 0x4000 ? target :
                (start / 0x4000) * 0x4000 + (target - 0x4000);
            auto matches = [&](const std::vector<uint8_t>& code) {
                return physical <= data.size() && code.size() <= data.size() - physical &&
                    std::equal(code.begin(), code.end(), data.begin() + physical);
            };
            if (target >= 0x8000) break;
            if (fits_copy(static_cast<uint16_t>(0xff00 | c), b) && matches(copy_c_b)) {
                destinations.push_back({static_cast<uint16_t>(0xFF00 | c),static_cast<uint16_t>(b)});
            }
            if (d >= 0 && e >= 0 && matches(copy_de_bc)) {
                uint16_t destination = static_cast<uint16_t>((d << 8) | e);
                uint16_t count = static_cast<uint16_t>((b << 8) | c);
                if (fits_copy(destination,count)) destinations.push_back({destination,count});
            }
            break;
        }
    }

    std::sort(destinations.begin(), destinations.end());
    destinations.erase(std::unique(destinations.begin(), destinations.end()), destinations.end());
    return destinations;
}

std::vector<AnalyzerOptions::RamOverlay> detect_oam_dma_overlays(const ROM& rom) {
    const std::vector<uint8_t>& data = rom.bytes();
    const std::vector<uint8_t> wait_pattern = {0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD};
    std::vector<gbrecomp::AnalyzerOptions::RamOverlay> overlays;

    auto it = std::search(data.begin(), data.end(), wait_pattern.begin(), wait_pattern.end());
    while (it != data.end()) {

        const size_t rom_idx = static_cast<size_t>(std::distance(data.begin(), it));
        const size_t copied_end = rom_idx + wait_pattern.size() + 1;
        if (copied_end > data.size()) break;
        const uint8_t terminal = data[copied_end - 1];
        if (terminal == 0xC9 || terminal == 0xD9) {
            std::vector<size_t> starts = {rom_idx};
            if (rom_idx >= 2 && data[rom_idx - 2] == 0x3E) starts.insert(starts.begin(), rom_idx - 2);
            const size_t legacy_start = starts.front();
            if (starts.front() && data[starts.front() - 1] == 0xF3) starts.insert(starts.begin(), starts.front() - 1);
            bool matched_copy = false;
            for (size_t source : starts) {
                const uint8_t bank = static_cast<uint8_t>(source / 0x4000);
                const uint16_t addr = static_cast<uint16_t>(bank ? 0x4000 + source % 0x4000 : source);
                const uint16_t size = static_cast<uint16_t>(copied_end - source);
                auto destinations = find_oam_dma_overlay_copies(data, addr, size);
                for (auto [destination,copied_size] : destinations) {
                    size_t boundary = source < 0x4000 ? 0x4000 : ((source / 0x4000 + 1) * 0x4000);
                    if (source + copied_size <= std::min(data.size(),boundary)) {
                        // Only the recognized code through RET/RETI is native.
                        // Some copies also initialize following mutable HRAM
                        // variables; those bytes must not guard this body.
                        overlays.push_back({destination, AnalysisResult::make_addr(bank, addr), size});
                    }
                }
                if (!destinations.empty()) { matched_copy = true; break; }
            }
            // Preserve the old RET-only recognition when a copy site is not
            // proven. New interrupt-return variants require a bounded copy.
            if (!matched_copy && terminal == 0xC9) {
                const uint8_t bank = static_cast<uint8_t>(legacy_start / 0x4000);
                const uint16_t addr = static_cast<uint16_t>(bank ? 0x4000 + legacy_start % 0x4000 : legacy_start);
                overlays.push_back({0xFF80, AnalysisResult::make_addr(bank, addr),
                                    static_cast<uint16_t>(copied_end - legacy_start)});
            }
        }
        it = std::search(it + 1, data.end(), wait_pattern.begin(), wait_pattern.end());
    }

    return overlays;
}

} // namespace gbrecomp
