// Replay a captured board JPEG through the production AXI decoder wrapper.
// The slave keeps responses stable until handshake and varies DDR backpressure.
#include "Vjpeg_decoder.h"
#include "Vjpeg_decoder___024root.h"
#include "Vjpeg_decoder_jpeg_decoder.h"
#include "Vjpeg_decoder_jpeg_core__S1.h"
#include "verilated.h"
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

struct Burst { uint32_t addr; unsigned words, id; };
struct Replay {
    Vjpeg_decoder d;
    std::vector<uint8_t> jpeg;
    std::vector<uint32_t> rgb = std::vector<uint32_t>(1328*1328);
    std::vector<bool> written = std::vector<bool>(1328*1328);
    std::deque<Burst> reads, writes;
    std::deque<uint64_t> responses;
    uint64_t cycle=0, pixels=0, lastProgress=0, inputWords=0;
    uint32_t random=1;
    unsigned mode=0;
    bool rHeld=false, bHeld=false;
    static constexpr uint32_t src=0x10000000, dst=0x20000000;
    Replay(const char *path, unsigned seed, unsigned stalls): random(seed),mode(stalls) {
        std::ifstream f(path,std::ios::binary);
        jpeg.assign(std::istreambuf_iterator<char>(f),{});
        if(jpeg.empty()) throw std::runtime_error("empty input");
        d.clk_i=0; d.rst_i=1; d.cfg_bready_i=1; d.cfg_rready_i=1;
        for(int n=0;n<8;n++) tick();
        d.rst_i=0;
    }
    uint32_t rng(){random^=random<<13;random^=random>>17;random^=random<<5;return random;}
    bool ready(unsigned channel) {
        if(!mode) return true;
        if(mode==2 && cycle%18000<7000) return false;
        return (rng()%100) >= channel;
    }
    void tick() {
        d.clk_i=0;
        d.outport_awready_i=writes.size()<8 && ready(35);
        d.outport_wready_i=!writes.empty() && ready(40);
        d.outport_arready_i=reads.size()<2 && ready(25);
        if(!rHeld && !reads.empty() && ready(30)) rHeld=true;
        if(rHeld) {
            auto &r=reads.front(); uint32_t data=0;
            for(unsigned k=0;k<4;k++) {
                uint32_t offset=r.addr-src+k;
                if(offset<jpeg.size()) data|=uint32_t(jpeg[offset])<<(8*k);
            }
            d.outport_rdata_i=data; d.outport_rid_i=r.id;
            d.outport_rlast_i=r.words==1;
        }
        d.outport_rvalid_i=rHeld; d.outport_rresp_i=0;
        if(!bHeld && !responses.empty() && responses.front()<=cycle && ready(35)) bHeld=true;
        d.outport_bvalid_i=bHeld; d.outport_bresp_i=0; d.outport_bid_i=0;
        d.eval();
        auto &v=*d.rootp->jpeg_decoder;
        if(v.u_core->inport_valid_i && v.u_core->inport_accept_o) {
            uint32_t expected=0;
            for(unsigned k=0;k<4;k++)if(inputWords*4+k<jpeg.size())expected|=uint32_t(jpeg[inputWords*4+k])<<(8*k);
            if(v.u_core->inport_data_i!=expected)throw std::runtime_error("prefetch data differs at word "+std::to_string(inputWords));
            ++inputWords;
        }
        bool ar=d.outport_arvalid_o&&d.outport_arready_i;
        bool aw=d.outport_awvalid_o&&d.outport_awready_i;
        bool w=d.outport_wvalid_o&&d.outport_wready_i;
        bool r=d.outport_rvalid_i&&d.outport_rready_o;
        bool b=d.outport_bvalid_i&&d.outport_bready_o;
        Burst rb{d.outport_araddr_o,unsigned(d.outport_arlen_o)+1,unsigned(d.outport_arid_o)};
        Burst wb{d.outport_awaddr_o,unsigned(d.outport_awlen_o)+1,unsigned(d.outport_awid_o)};
        if(w) {
            auto &q=writes.front();
            if(bool(d.outport_wlast_o)!=(q.words==1) || q.addr<dst || q.addr>=dst+1328*1328*4)
                throw std::runtime_error("invalid AXI write");
            const unsigned index=(q.addr-dst)/4;
            if(written[index])throw std::runtime_error("duplicate pixel address");
            written[index]=true;rgb[index]=d.outport_wdata_o;
            ++pixels; q.addr+=4;
            if(!--q.words) {writes.pop_front();responses.push_back(cycle+(mode?rng()%150:1));}
        }
        if(r) {auto &q=reads.front();q.addr+=4;if(!--q.words) reads.pop_front();rHeld=false;}
        if(b) {responses.pop_front();bHeld=false;}
        if(ar) reads.push_back(rb);
        if(aw) writes.push_back(wb);
        if(ar||aw||w||r||b) lastProgress=cycle;
        d.clk_i=1;d.eval();++cycle;
    }
    void reg(uint32_t address,uint32_t value) {
        d.cfg_awaddr_i=address;d.cfg_wdata_i=value;d.cfg_wstrb_i=15;
        bool a=false,w=false;
        for(unsigned n=0;n<100;n++) {
            d.cfg_awvalid_i=!a;d.cfg_wvalid_i=!w;
            d.clk_i=0;d.eval();
            bool at=d.cfg_awvalid_i&&d.cfg_awready_o,wt=d.cfg_wvalid_i&&d.cfg_wready_o;
            tick();a|=at;w|=wt;
            if(a&&w) break;
        }
        if(!a||!w) throw std::runtime_error("config handshake timeout");
        d.cfg_awvalid_i=0;d.cfg_wvalid_i=0;
        for(int n=0;n<5;n++)tick();
    }
    bool run() {
        reg(8,src);reg(12,dst);reg(0,uint32_t(jpeg.size())|0x80000000u);
        for(;cycle<30000000;){
            tick();
            if(d.progress_done_o || d.progress_error_o || cycle-lastProgress>200000)break;
        }
        bool ok=d.progress_done_o&&!d.progress_error_o&&pixels==1328ull*1328;
        auto &v=*d.rootp->jpeg_decoder;
        uint64_t hash=14695981039346656037ull;
        for(auto word:rgb)for(unsigned k=0;k<4;k++){hash^=(word>>(k*8))&255;hash*=1099511628211ull;}
        std::cout<<"{\"complete\":"<<(ok?"true":"false")<<",\"mode\":"<<mode
                 <<",\"rgb_fnv1a64\":\""<<std::hex<<hash<<std::dec<<"\""
                 <<",\"rows\":"<<d.progress_rows_committed_o<<",\"pixels\":"<<pixels
                 <<",\"cycles\":"<<cycle<<",\"error\":"<<unsigned(d.progress_error_o)
                 <<",\"pending_reads\":"<<reads.size()<<",\"pending_writes\":"<<writes.size()
                 <<",\"pending_responses\":"<<responses.size()
                 <<",\"input_words\":"<<inputWords<<",\"state\":"<<unsigned(v.state_q)
                 <<",\"fence\":"<<unsigned(v.progress_fence_q)
                 <<",\"pixel_pipe\":"<<unsigned(v.pixel_pipe_valid)<<"}\n";
        return ok;
    }
};
int main(int argc,char **argv) {
    Verilated::commandArgs(argc,argv);
    if(argc<2)return 2;
    try {Replay test(argv[1],argc>2?std::stoul(argv[2]):1,argc>3?std::stoul(argv[3]):0);return test.run()?0:1;}
    catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 2;}
}
