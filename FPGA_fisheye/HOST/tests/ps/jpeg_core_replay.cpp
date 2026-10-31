// Replay a captured JPEG through the unchanged PL JPEG core, without AXI/PS.
#include "Vjpeg_core.h"
#include "verilated.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(file)), {});
    if (jpeg.empty()) return 2;
    // Match the board wrapper: rounded DDR read, all byte strobes, EOI framing.
    const size_t actualBytes = jpeg.size();
    jpeg.resize((jpeg.size() + 3) & ~size_t(3), 0);
    const bool stalls = argc > 2;
    const bool inputGaps = argc > 3;
    Vjpeg_core dut;
    dut.clk_i = 0; dut.rst_i = 1;
    dut.inport_valid_i = 0; dut.inport_data_i = 0;
    dut.inport_strb_i = 0; dut.inport_last_i = 0; dut.outport_accept_i = 1;
    for (int i = 0; i < 8; ++i) {
        dut.clk_i = 0; dut.eval(); dut.clk_i = 1; dut.eval();
    }
    dut.rst_i = 0;
    size_t offset = 0;
    uint64_t pixels = 0, lastProgress = 0, cycle = 0;
    std::vector<uint32_t> rgb(1328*1328);
    unsigned maxRow = 0, width = 0, height = 0;
    for (; cycle < 20000000; ++cycle) {
        dut.clk_i = 0;
        // Match the AXI prefetch's empty cycle between 512-word banks.
        static size_t gapOffset = 0;
        const bool gap = inputGaps && offset && offset % 2048 == 0 && gapOffset != offset;
        dut.inport_valid_i = offset < jpeg.size() && !gap;
        uint32_t word = 0, mask = 0;
        for (unsigned n = 0; n < 4 && offset + n < jpeg.size(); ++n) {
            word |= uint32_t(jpeg[offset + n]) << (n * 8); mask |= 1u << n;
        }
        // An invalid bus may retain the previous bank's last word.
        if(gap) {
            word = uint32_t(jpeg[offset-4]) | uint32_t(jpeg[offset-3])<<8 |
                   uint32_t(jpeg[offset-2])<<16 | uint32_t(jpeg[offset-1])<<24;
            gapOffset = offset;
        }
        dut.inport_data_i = word; dut.inport_strb_i = mask;
        dut.inport_last_i = 0;
        dut.outport_accept_i = !stalls || (cycle % 97 >= 31);
        dut.eval();
        const bool input = dut.inport_valid_i && dut.inport_accept_o;
        const bool output = dut.outport_valid_o && dut.outport_accept_i;
        if (output) {
            ++pixels;
            const unsigned index=unsigned(dut.outport_pixel_y_o)*1328+unsigned(dut.outport_pixel_x_o);
            if(index<rgb.size())rgb[index]=uint32_t(dut.outport_pixel_r_o)|uint32_t(dut.outport_pixel_g_o)<<8|uint32_t(dut.outport_pixel_b_o)<<16;
            maxRow = std::max(maxRow, unsigned(dut.outport_pixel_y_o));
            width = dut.outport_width_o; height = dut.outport_height_o;
        }
        dut.clk_i = 1; dut.eval();
        if (input) offset = std::min(offset + 4, jpeg.size());
        if (input || output) lastProgress = cycle;
        if (width && pixels == uint64_t(width) * height) break;
        if (cycle - lastProgress > 50000) break;
    }
    const bool complete = width == 1328 && height == 1328 && pixels == 1328ull * 1328;
    uint64_t hash=14695981039346656037ull;
    for(auto word:rgb)for(unsigned k=0;k<4;k++){hash^=(word>>(k*8))&255;hash*=1099511628211ull;}
    std::cout << "{\"complete\":" << (complete ? "true" : "false")
              << ",\"stalls\":" << (stalls ? "true" : "false")
              << ",\"input_gaps\":" << (inputGaps ? "true" : "false")
              << ",\"rgb_fnv1a64\":\"" << std::hex << hash << std::dec << "\""
              << ",\"input_bytes\":" << offset << ",\"jpeg_bytes\":" << actualBytes
              << ",\"pixels\":" << pixels << ",\"max_row\":" << maxRow
              << ",\"cycles\":" << cycle << "}\n";
    dut.final();
    return complete ? 0 : 1;
}
