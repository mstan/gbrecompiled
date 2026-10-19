#ifndef GBRECOMP_RAM_OVERLAY_H
#define GBRECOMP_RAM_OVERLAY_H
#include "analyzer.h"
#include "rom.h"
namespace gbrecomp {
std::vector<AnalyzerOptions::RamOverlay> detect_oam_dma_overlays(const ROM& rom);
}
#endif
