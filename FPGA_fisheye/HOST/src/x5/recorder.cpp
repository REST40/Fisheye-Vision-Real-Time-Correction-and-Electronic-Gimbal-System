#include "wire.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>

static void check(int r, const char* where) {
    if(r<0) { char b[256]; av_strerror(r,b,sizeof b); throw std::runtime_error(std::string(where)+": "+b); }
}
using Frame = std::shared_ptr<AVFrame>;
static Frame frame() {
    auto f=av_frame_alloc();if(!f)throw std::bad_alloc();
    return Frame(f,[](AVFrame* value){av_frame_free(&value);});
}

class Encoder {
    AVFormatContext* fmt=nullptr;
    AVCodecContext* ctx=nullptr;
    AVStream* stream=nullptr;
    SwsContext* sws=nullptr;
    bool header=false, finished=false;
    int fps;
    std::map<int64_t,int64_t> durations;
    void packets() {
        AVPacket* p=av_packet_alloc();
        try {
            while(true) {
                int r=avcodec_receive_packet(ctx,p);
                if(r==AVERROR(EAGAIN)||r==AVERROR_EOF) break;
                check(r,"encode receive");
                auto duration=durations.find(p->pts);
                if(duration!=durations.end()){p->duration=duration->second;durations.erase(duration);}
                else if(!p->duration) p->duration=1000000/fps;
                av_packet_rescale_ts(p,ctx->time_base,stream->time_base);
                p->stream_index=stream->index;
                check(av_interleaved_write_frame(fmt,p),"MP4 write"); av_packet_unref(p);
            }
        } catch(...) {av_packet_free(&p);throw;}
        av_packet_free(&p);
    }
public:
    Encoder(const std::string& path,int w,int h,int fps_,const std::string& codec,int bitrate=8000000,AVPixelFormat source_format=AV_PIX_FMT_YUV420P):fps(fps_) {
        try {
            check(avformat_alloc_output_context2(&fmt,nullptr,"mp4",path.c_str()),"MP4 allocate");
            const AVCodec* c=avcodec_find_encoder_by_name(codec.c_str());
            if(!c) throw std::runtime_error("encoder unavailable: "+codec);
            ctx=avcodec_alloc_context3(c); ctx->width=w; ctx->height=h;
            ctx->time_base={1,1000000}; ctx->framerate={fps,1};
            // NVENC accepts packed RGBA and performs the YUV420 conversion
            // on the GPU. Decoded YUV420 frames are submitted directly too.
            ctx->pix_fmt=codec=="h264_nvenc"&&source_format==AV_PIX_FMT_RGBA?AV_PIX_FMT_RGBA:AV_PIX_FMT_YUV420P;ctx->bit_rate=bitrate;
            ctx->gop_size=fps; ctx->max_b_frames=0; ctx->thread_count=4;
            if(codec=="mjpeg"){
                ctx->pix_fmt=AV_PIX_FMT_YUVJ420P;ctx->color_range=AVCOL_RANGE_JPEG;
                ctx->flags|=AV_CODEC_FLAG_QSCALE;ctx->global_quality=2*FF_QP2LAMBDA;
                ctx->bit_rate=0;ctx->gop_size=1;ctx->thread_count=2;
            }
            if(fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* options=nullptr;
            if(codec=="libx264") {av_dict_set(&options,"preset","veryfast",0);av_dict_set(&options,"crf","20",0);}
            if(codec=="h264_nvenc") {av_dict_set(&options,"preset","p4",0);av_dict_set(&options,"tune","ll",0);av_dict_set(&options,"rc","vbr",0);av_dict_set(&options,"cq","21",0);av_dict_set(&options,"rgb_mode","yuv420",0);}
            if(codec=="mjpeg")av_dict_set(&options,"huffman","default",0);
            int r=avcodec_open2(ctx,c,&options); av_dict_free(&options); check(r,"encoder open");
            stream=avformat_new_stream(fmt,nullptr);
            if(!stream) throw std::runtime_error("stream allocate failed");
            stream->time_base=ctx->time_base;
            check(avcodec_parameters_from_context(stream->codecpar,ctx),"encoder params");
            check(avio_open(&fmt->pb,path.c_str(),AVIO_FLAG_WRITE),"MP4 open");
            // Fragmented MP4 preserves completed fragments if power is lost.
            av_dict_set(&options,"movflags",codec=="mjpeg"?"+faststart":"frag_keyframe+empty_moov+default_base_moof",0);
            r=avformat_write_header(fmt,&options);av_dict_free(&options);check(r,"MP4 header");header=true;
        } catch(...) {release();throw;}
    }
    void write(const AVFrame* source,int64_t pts,int64_t duration_us=0) {
        if(source->width!=ctx->width||source->height!=ctx->height) throw std::runtime_error("frame size changed during recording");
        Frame out;
        if(source->format==ctx->pix_fmt){
            out=Frame(av_frame_clone(source),[](AVFrame* f){av_frame_free(&f);});
            if(!out)throw std::runtime_error("encode frame reference failed");
            out->pts=pts;
        }else{
            out=frame();out->format=ctx->pix_fmt;out->width=ctx->width;out->height=ctx->height;out->pts=pts;
            check(av_frame_get_buffer(out.get(),32),"encode frame allocation");
            sws=sws_getCachedContext(sws,source->width,source->height,static_cast<AVPixelFormat>(source->format),ctx->width,ctx->height,ctx->pix_fmt,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if(!sws) throw std::runtime_error("color converter unavailable");
            if(sws_scale(sws,source->data,source->linesize,0,source->height,out->data,out->linesize)!=source->height)throw std::runtime_error("color conversion failed");
        }
        if(duration_us>0){out->duration=duration_us;durations[pts]=duration_us;}
        if(ctx->codec_id==AV_CODEC_ID_MJPEG)out->quality=ctx->global_quality;
        check(avcodec_send_frame(ctx,out.get()),"encode send");packets();
    }
    void finish() {
        if(finished||!header) return;
        check(avcodec_send_frame(ctx,nullptr),"encoder drain");packets();
        check(av_write_trailer(fmt),"MP4 trailer");finished=true;
    }
    void release() {sws_freeContext(sws);avcodec_free_context(&ctx);if(fmt){if(fmt->pb)avio_closep(&fmt->pb);avformat_free_context(fmt);fmt=nullptr;}}
    ~Encoder() {try {finish();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';} release();}
};

class Decoder {
    AVCodecContext* ctx=nullptr;
    int codec=0;
    std::function<void(Frame)> emit;
    void receive() {
        while(true) {
            auto f=frame();int r=avcodec_receive_frame(ctx,f.get());
            if(r==AVERROR(EAGAIN)||r==AVERROR_EOF)break;
            check(r,"preview decode");
            if(f->pts==AV_NOPTS_VALUE)throw std::runtime_error("decoded frame has no camera PTS");
            emit(f);
        }
    }
public:
    explicit Decoder(std::function<void(Frame)> fn):emit(std::move(fn)){}
    void send(const x5::Message& m) {
        if(!ctx) {
            codec=m.codec;auto c=avcodec_find_decoder(codec==1?AV_CODEC_ID_H264:AV_CODEC_ID_HEVC);
            if(!c||(codec!=1&&codec!=2))throw std::runtime_error("unsupported preview codec");
            ctx=avcodec_alloc_context3(c);ctx->pkt_timebase={1,1};ctx->thread_count=2;
            // Slice workers avoid the additional frame held by frame threading.
            // Codec-required B-frame reordering is still handled by the decoder.
            ctx->thread_type=FF_THREAD_SLICE;
            ctx->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
            check(avcodec_open2(ctx,c,nullptr),"preview decoder open");
        }
        if(codec!=int(m.codec))throw std::runtime_error("preview codec changed");
        AVPacket* p=av_packet_alloc();
        try {
            check(av_new_packet(p,static_cast<int>(m.data.size())),"packet allocate");
            std::memcpy(p->data,m.data.data(),m.data.size());p->pts=m.timestamp;p->dts=AV_NOPTS_VALUE;
            p->opaque_ref=av_buffer_alloc(sizeof(m.flags));
            if(!p->opaque_ref)throw std::runtime_error("packet metadata allocation failed");
            std::memcpy(p->opaque_ref->data,&m.flags,sizeof(m.flags));
            int r=avcodec_send_packet(ctx,p);if(r==AVERROR(EAGAIN)){receive();r=avcodec_send_packet(ctx,p);}check(r,"preview packet send");
        }catch(...){av_packet_free(&p);throw;}
        av_packet_free(&p);receive();
    }
    void finish(){if(ctx){check(avcodec_send_packet(ctx,nullptr),"decoder drain");receive();}}
    ~Decoder(){avcodec_free_context(&ctx);}
};

class Preview {
    SDL_Window* win=nullptr;SDL_Renderer* renderer=nullptr;
    SDL_Texture* textures[2]={};SwsContext* sws[2]={};int widths[2]={},heights[2]={};
public:
    explicit Preview(bool enabled) {
        if(!enabled)return;
        if(SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        win=SDL_CreateWindow("Insta360 X5 | LEFT: fisheye | RIGHT: corrected | Esc: stop",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1280,480,SDL_WINDOW_RESIZABLE);
        if(!win)throw std::runtime_error(SDL_GetError());
        renderer=SDL_CreateRenderer(win,-1,SDL_RENDERER_SOFTWARE);
        if(!renderer)throw std::runtime_error(SDL_GetError());
        std::cerr<<"Preview video driver: "<<SDL_GetCurrentVideoDriver()<<'\n';
    }
    bool show(const AVFrame* left,const AVFrame* right) {
        if(!win)return true;
        SDL_Event e;while(SDL_PollEvent(&e)){if(e.type==SDL_QUIT||(e.type==SDL_KEYDOWN&&e.key.keysym.sym==SDLK_ESCAPE))return false;}
        SDL_RenderClear(renderer);int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
        const AVFrame* fs[]={left,right};
        for(int i=0;i<2;++i){auto f=fs[i];
            if(widths[i]!=f->width||heights[i]!=f->height){SDL_DestroyTexture(textures[i]);textures[i]=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,f->width,f->height);widths[i]=f->width;heights[i]=f->height;}
            if(!textures[i])throw std::runtime_error(SDL_GetError());
            std::vector<uint8_t> rgba(size_t(f->width)*f->height*4);uint8_t* dst[]={rgba.data()};int stride[]={f->width*4};
            sws[i]=sws_getCachedContext(sws[i],f->width,f->height,static_cast<AVPixelFormat>(f->format),f->width,f->height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if(!sws[i])throw std::runtime_error("preview conversion failed");
            sws_scale(sws[i],f->data,f->linesize,0,f->height,dst,stride);
            if(SDL_UpdateTexture(textures[i],nullptr,rgba.data(),stride[0]))throw std::runtime_error(SDL_GetError());
            double scale=std::min(double(w/2)/f->width,double(h)/f->height);
            SDL_Rect rect{i*(w/2)+(w/2-int(f->width*scale))/2,(h-int(f->height*scale))/2,int(f->width*scale),int(f->height*scale)};
            SDL_RenderCopy(renderer,textures[i],nullptr,&rect);
        }SDL_RenderPresent(renderer);return true;
    }
    ~Preview(){for(int i=0;i<2;++i){SDL_DestroyTexture(textures[i]);sws_freeContext(sws[i]);}SDL_DestroyRenderer(renderer);SDL_DestroyWindow(win);SDL_Quit();}
};

#include "gui_io.hpp"
#include "lens_capture.hpp"

static bool read_message(x5::Message& m) {
    uint8_t h[x5::header_size];std::cin.read(reinterpret_cast<char*>(h),sizeof h);
    if(std::cin.gcount()==0&&std::cin.eof())return false;
    if(std::cin.gcount()!=sizeof h)throw std::runtime_error("truncated IPC header");
    if(x5::get(h,4)!=x5::magic)throw std::runtime_error("invalid IPC magic");
    auto n=x5::get(h+8,4);if(n>x5::max_payload)throw std::runtime_error("oversized IPC payload");
    m.kind=x5::get(h+4,4);m.width=x5::get(h+12,4);m.height=x5::get(h+16,4);m.codec=x5::get(h+20,4);m.timestamp=static_cast<int64_t>(x5::get(h+24,8));
    m.flags=x5::get(h+32,8);
    if(m.flags&~x5::invalid_timestamp)throw std::runtime_error("unsupported IPC flags");
    m.data.resize(n);std::cin.read(reinterpret_cast<char*>(m.data.data()),n);
    if(size_t(std::cin.gcount())!=n)throw std::runtime_error("truncated IPC payload");
    return true;
}

int main(int argc,char** argv) {
    std::string output,status_path,last_clip,last_snapshot,error;
    std::unique_ptr<app::Recording> recording;
    bool interactive=false,eos=false;
    try {
        std::string encoder="libx264",preview_path,metadata_dir;int fps=30,bitrate=8000000,control_fd=-1;int64_t hz=0;bool preview=false,preview_latest=false;
        for(int i=1;i<argc;++i){std::string a=argv[i];
            auto value=[&](){if(++i>=argc)throw std::runtime_error("missing argument for "+a);return std::string(argv[i]);};
            if(a=="--output")output=value();else if(a=="--timestamp-hz")hz=std::stoll(value());
            else if(a=="--fps")fps=std::stoi(value());else if(a=="--encoder")encoder=value();else if(a=="--preview")preview=true;
            else if(a=="--interactive")interactive=true;else if(a=="--control-fd")control_fd=std::stoi(value());
            else if(a=="--preview-latest")preview_latest=true;
            else if(a=="--status-file")status_path=value();else if(a=="--preview-file")preview_path=value();
            else if(a=="--metadata-dir")metadata_dir=value();
            else if(a=="--bitrate-mbps")bitrate=std::stoi(value())*1000000;
            else throw std::runtime_error("unknown argument: "+a);
        }
        if(output.empty()||hz<=0||hz>1000000000||fps<=0||fps>240||bitrate<1000000||bitrate>100000000)throw std::runtime_error("invalid output, timestamp, FPS or bitrate");
        if(encoder!="libx264"&&encoder!="h264_nvenc")throw std::runtime_error("unsupported recording encoder");
        if(interactive&&(control_fd<0||status_path.empty()||preview_path.empty()))throw std::runtime_error("interactive mode requires control, status and preview mailbox");
        if(std::filesystem::exists(output))throw std::runtime_error("output exists; use a new directory to preserve previous recordings");
        std::filesystem::create_directories(output);
        av_log_set_level(AV_LOG_ERROR);
        if(!interactive)recording=std::make_unique<app::Recording>(output,encoder,fps,bitrate,hz,metadata_dir);
        app::Controls controls(control_fd);app::PreviewBuffer mailbox(preview_path);app::RawBuffer rawMailbox(preview_path.empty()?std::string():preview_path+".raw");app::Snapshots snapshots;app::GopCache gop;
        app::LensCapture lenses;
        Preview ui(preview);std::map<int64_t,Frame> raw,corrected;
        std::optional<int64_t> last;Frame latest_left,latest_right;
        Frame preview_left,preview_right;uint64_t preview_id=0;
        auto publish_preview=[&](){
            if(preview_latest&&preview_left&&preview_right)
                mailbox.show(preview_left.get(),preview_right.get(),++preview_id,preview_right->pts);
        };
        std::deque<std::pair<double,double>> exposure_cache;
        auto exposure_at=[&](int64_t ts)->std::optional<double>{
            for(auto i=exposure_cache.rbegin();i!=exposure_cache.rend();++i)if(i->first==double(ts))return i->second;
            return {};
        };
        uint64_t count=0,unmatched=0,warmup=0,queue_drops=0,clips=0,shot_count=0,last_recorded_count=0;
        bool ui_stop=false,draining=false,quit=false;
        int raw_w=0,raw_h=0,out_w=0,out_h=0;double processing_ms=0;
        auto last_status=app::Clock::now()-std::chrono::seconds(1);
        std::deque<app::Clock::time_point> pair_times;
        auto status=[&](bool force=false,const std::string& phase=""){
            if(status_path.empty())return;
            auto now=app::Clock::now();if(!force&&now-last_status<std::chrono::milliseconds(200))return;last_status=now;
            double rate=pair_times.size()>1?double(pair_times.size()-1)/std::chrono::duration<double>(pair_times.back()-pair_times.front()).count():0;
            std::ostringstream j;j<<std::setprecision(6)<<"{\"phase\":"<<app::quoted(phase.empty()?(quit?"stopping":recording?"recording":count?"preview":"warming"):phase)
                <<",\"total_pairs\":"<<count<<",\"recorded_frames\":"<<(recording?recording->count():last_recorded_count)
                <<",\"camera_timestamp\":"<<last.value_or(0)<<",\"epoch_timestamp\":"<<(recording?recording->epoch_timestamp():0)
                <<",\"fps\":"<<rate<<",\"processing_ms\":"<<processing_ms
                <<",\"raw_width\":"<<raw_w<<",\"raw_height\":"<<raw_h<<",\"output_width\":"<<out_w<<",\"output_height\":"<<out_h
                <<",\"unmatched\":"<<unmatched<<",\"warmup\":"<<warmup<<",\"queue_drops\":"<<queue_drops
                <<",\"recording\":"<<(recording?"true":"false")<<",\"gop_ready\":"<<(gop.ready()?"true":"false")
                <<",\"snapshot_busy\":"<<(snapshots.busy()?"true":"false")<<",\"encoder\":"<<app::quoted(encoder)
                <<",\"record_path\":"<<app::quoted(recording?recording->directory():"")<<",\"last_clip\":"<<app::quoted(last_clip)
                <<",\"snapshot_path\":"<<app::quoted(last_snapshot)<<",\"error\":"<<app::quoted(error)<<"}\n";
            auto text=j.str();text.erase(text.size()-2);
            text+=",\"lens_busy\":"+std::string(lenses.busy()?"true":"false")+
                ",\"lens_frames\":"+std::to_string(lenses.frames())+
                ",\"lens_elapsed\":"+std::to_string(lenses.elapsed())+
                ",\"lens_path\":"+app::quoted(lenses.directory())+
                ",\"lens_error\":"+app::quoted(lenses.error())+"}\n";
            app::atomic_text(status_path,text);
        };
        auto drop=[&](const char* branch,int64_t ts,const char* reason){
            ++unmatched;if(std::string(reason)=="queue_limit")++queue_drops;
            if(recording)recording->drop(branch,ts,reason);
        };
        auto trim=[&](auto& queue,const char* branch){while(queue.size()>60){drop(branch,queue.begin()->first,"queue_limit");queue.erase(queue.begin());}};
        auto stop_recording=[&](bool stream_eos,const std::string& reason){
            if(!recording)return;
            for(const auto& p:raw)if(p.first>=recording->epoch_timestamp())recording->drop("fisheye",p.first,"recording_boundary");
            for(const auto& p:corrected)if(p.first>=recording->epoch_timestamp())recording->drop("corrected",p.first,"recording_boundary");
            recording->finish(stream_eos,stream_eos||reason=="user_stop",reason);last_clip=recording->directory();last_recorded_count=recording->count();recording.reset();status(true);
        };
        auto pair=[&](){
            trim(raw,"fisheye");trim(corrected,"corrected");
            for(auto i=raw.begin();i!=raw.end();){
                auto j=corrected.find(i->first);
                if(j==corrected.end()){
                    bool expired=!corrected.empty()&&corrected.rbegin()->first-i->first>hz/4;
                    if(!draining&&!expired)break;
                    drop("fisheye",i->first,draining?"end_of_stream":"stitch_timeout");i=raw.erase(i);continue;
                }
                auto begin=app::Clock::now();int64_t ts=i->first;
                if(last&&ts<=*last){drop("pair",ts,"non_monotonic");corrected.erase(j);i=raw.erase(i);continue;}
                if(recording)recording->pair(i->second.get(),j->second.get(),ts,exposure_at(ts));
                raw_w=i->second->width;raw_h=i->second->height;out_w=j->second->width;out_h=j->second->height;
                latest_left=i->second;latest_right=j->second;last=ts;++count;
                if(!preview_latest)mailbox.show(latest_left.get(),latest_right.get(),count,ts);
                if(!ui_stop&&!ui.show(latest_left.get(),latest_right.get())){
                    ui_stop=true;app::atomic_text(output+"/stop_requested","preview_closed\n");
                }
                pair_times.push_back(app::Clock::now());while(pair_times.size()>2&&pair_times.back()-pair_times.front()>std::chrono::seconds(2))pair_times.pop_front();
                processing_ms=processing_ms*.9+std::chrono::duration<double,std::milli>(app::Clock::now()-begin).count()*.1;
                corrected.erase(j);i=raw.erase(i);
            }
        };
        auto commands=[&]{
            for(const auto& command:controls.read()){
                const bool start_command=command=="start"||command.rfind("start ",0)==0;
                const bool snapshot_command=command=="snapshot"||command.rfind("snapshot ",0)==0;
                if(start_command&&lenses.busy()){error="前后数据采集中，请等待保存完成";continue;}
                if(start_command&&!recording&&!quit){
                    if(!count||!gop.ready()){error="相机尚在预热，请稍后开始录制";continue;}
                    std::ostringstream name;name<<output<<"/clips/clip_"<<std::setw(3)<<std::setfill('0')<<++clips<<"/video";
                    if(std::filesystem::exists(name.str()))throw std::runtime_error("clip directory already exists");
                    auto frozen=command=="start"?metadata_dir:command.substr(6);
                    recording=std::make_unique<app::Recording>(name.str(),encoder,fps,bitrate,hz,frozen);
                    for(const auto& sample:exposure_cache)recording->exposure(sample.first,sample.second,true);
                    for(const auto& packet:gop.packets())recording->packet(packet,true);
                    error.clear();status(true);
                }else if(command=="stop"){stop_recording(false,"user_stop");}
                else if(snapshot_command){
                    auto frozen=command=="snapshot"?metadata_dir:command.substr(9);
                    if(!snapshots.start(latest_left,latest_right,last.value_or(0),hz,output+"/snapshots/pair_"+std::to_string(++shot_count),frozen,exposure_at(last.value_or(0))))error="请等待画面就绪或上一组照片保存完成";
                }else if(command.rfind("lens_start ",0)==0){
                    const auto fields=command.substr(11);auto tab=fields.find('\t');
                    if(tab==std::string::npos)throw std::runtime_error("missing lens dataset destination");
                    if(recording||!count)error="请停止双路录像并等待画面就绪，再开始前后采集";
                    else if(!lenses.start(fields.substr(tab+1),fields.substr(0,tab),hz))error="前后采集尚未完成";
                    else error.clear();
                    status(true);
                }else if(command=="quit"){quit=true;status(true);}
            }
            try{if(auto done=snapshots.poll()){last_snapshot=*done;error.clear();status(true);}}catch(const std::exception& e){error=e.what();status(true);}
            status();
        };
        Decoder decoder([&](Frame f){
            uint64_t flags=0;
            if(f->opaque_ref&&f->opaque_ref->size==sizeof(flags))std::memcpy(&flags,f->opaque_ref->data,sizeof(flags));
            if(flags&x5::invalid_timestamp){++warmup;drop("fisheye",f->pts,"startup_warmup");return;}
            rawMailbox.show(f.get());
            lenses.accept(f);
            preview_left=f;publish_preview();
            if(!raw.emplace(f->pts,f).second)throw std::runtime_error("duplicate raw timestamp");
            pair();
        });
        status(true);x5::Message m;
        while(true){
            commands();if(!read_message(m))break;
            if(m.kind==1){
                if(interactive)gop.add(m);
                if(recording)recording->packet(m);
            }
            if(m.kind==1)decoder.send(m);
            else if(m.kind==2){
                if(!m.width||!m.height||m.width>8192||m.height>8192||m.width%2||m.height%2||m.data.size()!=uint64_t(m.width)*m.height*4)throw std::runtime_error("invalid corrected RGBA frame");
                auto f=frame();f->format=AV_PIX_FMT_RGBA;f->width=m.width;f->height=m.height;f->pts=m.timestamp;
                auto pixels=std::make_unique<std::vector<uint8_t>>(std::move(m.data));
                f->buf[0]=av_buffer_create(pixels->data(),pixels->size(),[](void* owner,uint8_t*){delete static_cast<std::vector<uint8_t>*>(owner);},pixels.get(),0);
                if(!f->buf[0])throw std::runtime_error("RGBA reference allocation failed");
                pixels.release();f->data[0]=f->buf[0]->data;f->linesize[0]=m.width*4;
                preview_right=f;publish_preview();
                if(!corrected.emplace(m.timestamp,f).second)throw std::runtime_error("duplicate corrected timestamp");
                pair();
            }else if(m.kind==4){
                if(m.data.size()!=16)throw std::runtime_error("invalid exposure telemetry payload");
                double timestamp,value;std::memcpy(&timestamp,m.data.data(),8);std::memcpy(&value,m.data.data()+8,8);
                if(!std::isfinite(timestamp)||!std::isfinite(value))throw std::runtime_error("invalid exposure telemetry value");
                exposure_cache.emplace_back(timestamp,value);while(exposure_cache.size()>128)exposure_cache.pop_front();
                if(recording)recording->exposure(timestamp,value);
            }else if(m.kind==3){eos=true;break;}else throw std::runtime_error("unexpected IPC message kind");
        }
        decoder.finish();draining=true;pair();
        lenses.finish();
        for(const auto& p:raw)drop("fisheye",p.first,"end_of_stream");
        for(const auto& p:corrected)drop("corrected",p.first,"end_of_stream");
        raw.clear();corrected.clear();
        stop_recording(eos,eos?"end_of_stream":"unexpected_eof");
        try{if(auto done=snapshots.finish())last_snapshot=*done;}catch(const std::exception& e){error=e.what();}
        if(!eos&&error.empty())error="相机数据流意外中断，请查看本次采集日志。";
        status(true,eos?"closed":"error");
        std::cerr<<"Processed "<<count<<" matched pairs, "<<unmatched<<" unmatched frames\n";
        if(!interactive&&count==0)throw std::runtime_error("no matched frames; verify camera connection, calibration and timestamp units");
        return eos?0:2;
    }catch(const std::exception& e){
        std::cerr<<"x5_recorder: "<<e.what()<<'\n';
        if(recording){try{recording->finish(false,false,"error");}catch(...){}}
        if(!status_path.empty()){try{app::atomic_text(status_path,"{\"phase\":\"error\",\"error\":"+app::quoted(e.what())+"}\n");}catch(...){}}
        return 1;
    }
}
