#pragma once
#include <cstdint>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

// Little-endian local IPC, 40-byte header. No C/C++ struct layout on the wire.
// 0:magic 4:kind 8:payload bytes 12:width 16:height 20:codec
// 24:camera timestamp 32:flags. kind:1 encoded AU,2 corrected RGBA,3 EOS,
// 4: raw SDK exposure telemetry (two little-endian IEEE doubles).
namespace x5 {
constexpr uint32_t magic = 0x31463558; // X5F1
constexpr size_t header_size = 40;
constexpr uint32_t max_payload = 64 * 1024 * 1024;
constexpr uint64_t invalid_timestamp = 1; // Startup/cache packet: decode/archive, do not pair.
inline void put(uint8_t* p, uint64_t v, unsigned n) {
    for (unsigned i=0;i<n;++i) p[i]=static_cast<uint8_t>(v>>(8*i));
}
inline uint64_t get(const uint8_t* p, unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;++i) v|=uint64_t(p[i])<<(8*i); return v;
}
struct Message {
    uint32_t kind=0, width=0, height=0, codec=0; // codec 1 H264, 2 HEVC
    int64_t timestamp=0;
    uint64_t flags=0;
    std::vector<uint8_t> data;
    std::array<uint8_t,header_size> header() const {
        if(data.size()>max_payload) throw std::runtime_error("IPC payload too large");
        std::array<uint8_t,header_size> b{};
        put(b.data(),magic,4); put(b.data()+4,kind,4); put(b.data()+8,data.size(),4);
        put(b.data()+12,width,4); put(b.data()+16,height,4); put(b.data()+20,codec,4);
        put(b.data()+24,static_cast<uint64_t>(timestamp),8);
        put(b.data()+32,flags,8);
        return b;
    }
    std::vector<uint8_t> bytes() const {
        auto h=header();std::vector<uint8_t> b(header_size+data.size());
        std::memcpy(b.data(),h.data(),h.size());
        if(!data.empty())std::memcpy(b.data()+header_size,data.data(),data.size());
        return b;
    }
};
}
