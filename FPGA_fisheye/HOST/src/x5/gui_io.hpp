#pragma once
#include <array>
#include <chrono>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <future>
#include <iomanip>
#include <sstream>
#include <sys/mman.h>
#include <unistd.h>

namespace app {
using Clock=std::chrono::steady_clock;
inline std::string quoted(const std::string& text){
    std::ostringstream out;out<<'"';
    for(unsigned char c:text){
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
inline void atomic_text(const std::string& path,const std::string& text){
    std::ofstream file(path+".tmp",std::ios::binary|std::ios::trunc);file<<text;file.close();
    if(!file)throw std::runtime_error("cannot save status: "+path);
    std::filesystem::rename(path+".tmp",path);
}

class Controls {
    int fd;std::string pending;
public:
    explicit Controls(int descriptor):fd(descriptor){if(fd>=0){int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)throw std::runtime_error("control pipe unavailable");}}
    std::vector<std::string> read(){
        std::vector<std::string> commands;if(fd<0)return commands;char buffer[512];
        while(true){ssize_t n=::read(fd,buffer,sizeof buffer);if(n>0)pending.append(buffer,n);else if(n<0&&errno==EINTR)continue;else break;}
        if(pending.size()>4096)throw std::runtime_error("control command too long");
        size_t pos;while((pos=pending.find('\n'))!=std::string::npos){commands.push_back(pending.substr(0,pos));pending.erase(0,pos+1);}
        return commands;
    }
};

// A latest-frame mailbox: the UI never queues or delays recording frames.
// Header v2: magic/version, seqlock, publication ID, corrected camera timestamp,
// source/output sizes, monotonic write time, raw camera timestamp.
// RGB24 payload: two 640x320 views side by side.
class PreviewBuffer {
    static constexpr size_t payload=1280*320*3,bytes=64+payload;
    uint8_t* memory=nullptr;int fd=-1;uint64_t sequence=0;SwsContext* sws[2]={};
public:
    explicit PreviewBuffer(const std::string& path){
        if(path.empty())return;
        fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0)throw std::runtime_error("preview mailbox exists or cannot be opened");
        if(ftruncate(fd,bytes)){::close(fd);fd=-1;throw std::runtime_error("preview allocation failed");}
        auto map=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(map==MAP_FAILED){::close(fd);fd=-1;throw std::runtime_error("preview mapping failed");}
        memory=static_cast<uint8_t*>(map);std::memset(memory,0,bytes);
        x5::put(memory,0x50563558,4);x5::put(memory+4,2,4);
    }
    void show(const AVFrame* left,const AVFrame* right,uint64_t id,int64_t timestamp){
        if(!memory)return;
        auto seq=reinterpret_cast<uint64_t*>(memory+8);
        __atomic_store_n(seq,++sequence,__ATOMIC_SEQ_CST);
        x5::put(memory+16,id,8);x5::put(memory+24,uint64_t(timestamp),8);
        x5::put(memory+32,left->width,4);x5::put(memory+36,left->height,4);
        x5::put(memory+40,right->width,4);x5::put(memory+44,right->height,4);
        auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        x5::put(memory+48,uint64_t(now),8);
        // v2: each preview branch may show its newest frame independently.
        // Recording/snapshots still use matched frames from the pairer.
        x5::put(memory+56,uint64_t(left->pts),8);
        const AVFrame* frames[]={left,right};
        for(int i=0;i<2;++i){auto f=frames[i];
            sws[i]=sws_getCachedContext(sws[i],f->width,f->height,static_cast<AVPixelFormat>(f->format),640,320,AV_PIX_FMT_RGB24,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
            if(!sws[i])throw std::runtime_error("preview scaler unavailable");
            uint8_t* dst[]={memory+64+i*640*3};int stride[]={1280*3};
            if(sws_scale(sws[i],f->data,f->linesize,0,f->height,dst,stride)!=320)throw std::runtime_error("preview scaling failed");
        }
        __atomic_store_n(seq,++sequence,__ATOMIC_SEQ_CST);
    }
    ~PreviewBuffer(){for(auto c:sws)sws_freeContext(c);if(memory)munmap(memory,bytes);if(fd>=0)::close(fd);}
};

// Full unrectified dual-fisheye mailbox, independent of SDK panorama timing.
class RawBuffer {
    static constexpr size_t capacity=2656u*1328u*3u,bytes=64+capacity;
    uint8_t* memory=nullptr;int fd=-1;uint64_t sequence=0,id=0;SwsContext* sws=nullptr;
public:
    explicit RawBuffer(const std::string& path){
        if(path.empty())return;
        fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL,0600);
        if(fd<0)throw std::runtime_error("cannot create full raw mailbox");
        if(ftruncate(fd,bytes)){::close(fd);fd=-1;throw std::runtime_error("raw mailbox allocation failed");}
        auto map=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(map==MAP_FAILED){::close(fd);fd=-1;throw std::runtime_error("raw mailbox mapping failed");}
        memory=static_cast<uint8_t*>(map);std::memset(memory,0,bytes);
        x5::put(memory,0x52573558,4);x5::put(memory+4,1,4);
    }
    void show(const AVFrame* raw){
        if(!memory)return;
        const size_t size=size_t(raw->width)*raw->height*3;
        if(raw->width<=0||raw->height<=0||size>capacity)throw std::runtime_error("raw frame exceeds mailbox");
        auto seq=reinterpret_cast<uint64_t*>(memory+8);
        __atomic_store_n(seq,++sequence,__ATOMIC_SEQ_CST);
        x5::put(memory+16,++id,8);x5::put(memory+24,uint64_t(raw->pts),8);
        x5::put(memory+32,raw->width,4);x5::put(memory+36,raw->height,4);
        sws=sws_getCachedContext(sws,raw->width,raw->height,static_cast<AVPixelFormat>(raw->format),
                                raw->width,raw->height,AV_PIX_FMT_RGB24,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
        if(!sws)throw std::runtime_error("raw RGB conversion failed");
        uint8_t* dst[]={memory+64};int stride[]={raw->width*3};
        if(sws_scale(sws,raw->data,raw->linesize,0,raw->height,dst,stride)!=raw->height)
            throw std::runtime_error("raw RGB conversion incomplete");
        __atomic_store_n(seq,++sequence,__ATOMIC_SEQ_CST);
    }
    ~RawBuffer(){sws_freeContext(sws);if(memory)munmap(memory,bytes);if(fd>=0)::close(fd);}
};

inline bool keyframe(const x5::Message& m){
    for(size_t i=0;i+4<m.data.size();++i){
        size_t start=0;
        if(m.data[i]==0&&m.data[i+1]==0&&m.data[i+2]==1)start=i+3;
        else if(m.data[i]==0&&m.data[i+1]==0&&m.data[i+2]==0&&m.data[i+3]==1)start=i+4;
        if(!start||start>=m.data.size())continue;
        int type=m.codec==1?(m.data[start]&31):((m.data[start]>>1)&63);
        if((m.codec==1&&type==5)||(m.codec==2&&type>=16&&type<=21))return true;
    }
    return false;
}
class GopCache {
    std::deque<x5::Message> packets_;size_t bytes=0;bool valid=false;
public:
    void add(const x5::Message& m){
        if(keyframe(m)){packets_.clear();bytes=0;valid=true;}
        if(!valid)return;
        if(bytes+m.data.size()>32*1024*1024||packets_.size()>=300){packets_.clear();bytes=0;valid=false;return;}
        packets_.push_back(m);bytes+=m.data.size();
    }
    bool ready()const{return valid;}
    const auto& packets()const{return packets_;}
};

// Load once at the command boundary. A later SDK readback must never change
// the metadata belonging to a completed clip or an asynchronous PNG job.
class MetadataBundle {
    std::map<std::string,std::string> files;
public:
    explicit MetadataBundle(const std::string& directory){
        if(directory.empty())return;
        for(const std::string name:{"capture_settings.json","camera_metadata.json","calibration.json"}){
            auto path=std::filesystem::path(directory)/name;
            if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("camera parameter file unavailable: "+name);
            std::ifstream input(path,std::ios::binary);std::ostringstream text;text<<input.rdbuf();
            if(input.bad()||text.str().empty())throw std::runtime_error("camera parameter file read failed: "+name);
            files.emplace(name,text.str());
        }
    }
    void save(const std::string& directory)const{for(const auto& item:files)atomic_text(directory+"/"+item.first,item.second);}
    std::string json()const{
        if(files.empty())return "{\"available\":false}";
        return "{\"available\":true,\"capture_settings\":"+files.at("capture_settings.json")+",\"camera_readback\":"+files.at("camera_metadata.json")+",\"calibration\":"+files.at("calibration.json")+"}";
    }
};

class Recording {
    std::string path,encoder;int fps,bitrate;
    std::ofstream csv,drops,packet_csv,source,exposure_csv;
    std::unique_ptr<Encoder> left_encoder,right_encoder;
    std::optional<int64_t> epoch;
    uint64_t count_=0,dropped=0,packet_count=0,byte_offset=0,pre_roll=0;bool done=false;
    int64_t timestamp_hz;
    MetadataBundle metadata;int raw_w=0,raw_h=0,out_w=0,out_h=0;int64_t last_timestamp=0;uint64_t exposure_count=0,exposure_matched=0;
    void save_parameters(bool final){
        std::ostringstream out;out<<"{\"schema_version\":1,\"metadata\":"<<metadata.json()
            <<",\"observed\":{\"fisheye_width\":"<<raw_w<<",\"fisheye_height\":"<<raw_h<<",\"corrected_width\":"<<out_w<<",\"corrected_height\":"<<out_h
            <<",\"nominal_fps\":"<<fps<<",\"timestamp_hz\":"<<timestamp_hz<<",\"first_camera_timestamp\":"<<epoch.value_or(0)<<",\"last_camera_timestamp\":"<<last_timestamp
            <<",\"paired_frames\":"<<count_<<",\"encoder\":"<<quoted(encoder)<<",\"target_bitrate_bps\":"<<bitrate<<",\"exposure_samples\":"<<exposure_count
            <<",\"frames_with_exposure_sample\":"<<exposure_matched<<",\"frames_without_exposure_sample\":"<<count_-exposure_matched
            <<"},\"finalized\":"<<(final?"true":"false")<<",\"telemetry_note\":\"exposure.csv preserves raw SDK callback values and bounded pre-roll; frames.csv joins exact camera timestamps, leaving missing values blank. No measured ISO is provided by this SDK.\"}\n";
        atomic_text(path+"/recording_parameters.json",out.str());
    }
public:
    Recording(std::string directory,const std::string& codec,int fps_,int bitrate_,int64_t hz,const std::string& metadata_dir=""):path(std::move(directory)),encoder(codec),fps(fps_),bitrate(bitrate_),timestamp_hz(hz),metadata(metadata_dir){
        std::filesystem::create_directories(path);
        csv.open(path+"/frames.csv");drops.open(path+"/unmatched.csv");packet_csv.open(path+"/preview_packets.csv");exposure_csv.open(path+"/exposure.csv");
        if(!csv||!drops||!packet_csv||!exposure_csv)throw std::runtime_error("recording directory is not writable");
        csv<<"frame_id,camera_timestamp,pts_us,exposure_time_raw\n";drops<<"branch,camera_timestamp,reason\n";
        packet_csv<<"packet_id,camera_timestamp,byte_offset,byte_size,codec,flags\n";
        exposure_csv<<"sample_id,timestamp_raw,exposure_time_raw,host_received_monotonic_ns,pre_roll\n";
        metadata.save(path);save_parameters(false);
    }
    const std::string& directory()const{return path;}
    uint64_t count()const{return count_;}
    int64_t epoch_timestamp()const{return epoch.value_or(0);}
    void exposure(double timestamp,double value,bool context=false){
        auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        exposure_csv<<std::setprecision(17)<<exposure_count++<<','<<timestamp<<','<<value<<','<<now<<','<<(context?1:0)<<'\n';
        if(!exposure_csv)throw std::runtime_error("exposure telemetry write failed");
    }
    void packet(const x5::Message& m,bool context=false){
        if(!source.is_open())source.open(path+(m.codec==1?"/preview.h264":"/preview.h265"),std::ios::binary);
        source.write(reinterpret_cast<const char*>(m.data.data()),m.data.size());
        if(!source)throw std::runtime_error("encoded preview archive write failed");
        packet_csv<<packet_count++<<','<<m.timestamp<<','<<byte_offset<<','<<m.data.size()<<','<<m.codec<<','<<(m.flags|(context?2:0))<<'\n';
        byte_offset+=m.data.size();if(context)++pre_roll;
    }
    void drop(const char* branch,int64_t ts,const char* reason){drops<<branch<<','<<ts<<','<<reason<<'\n';++dropped;}
    void pair(const AVFrame* left,const AVFrame* right,int64_t ts,std::optional<double> exposure_value={}){
        if(!epoch)epoch=ts;
        last_timestamp=ts;raw_w=left->width;raw_h=left->height;out_w=right->width;out_h=right->height;
        auto pts=av_rescale_q(ts-*epoch,AVRational{1,static_cast<int>(timestamp_hz)},AVRational{1,1000000});
        if(!left_encoder){
            left_encoder=std::make_unique<Encoder>(path+"/fisheye.mp4",left->width,left->height,fps,encoder,bitrate,static_cast<AVPixelFormat>(left->format));
            right_encoder=std::make_unique<Encoder>(path+"/corrected.mp4",right->width,right->height,fps,encoder,bitrate,static_cast<AVPixelFormat>(right->format));
        }
        left_encoder->write(left,pts);right_encoder->write(right,pts);csv<<count_++<<','<<ts<<','<<pts<<',';
        if(exposure_value){csv<<std::setprecision(17)<<*exposure_value;++exposure_matched;}
        csv<<'\n';
        if(count_==1)save_parameters(false);
        if(!csv)throw std::runtime_error("frame manifest write failed");
    }
    void finish(bool eos,bool clean_stop,const std::string& reason){
        if(done)return;
        if(left_encoder)left_encoder->finish();
        if(right_encoder)right_encoder->finish();
        csv.flush();drops.flush();packet_csv.flush();exposure_csv.flush();if(source.is_open())source.flush();
        if(!csv||!drops||!packet_csv||!exposure_csv||(source.is_open()&&!source))throw std::runtime_error("recording manifest flush failed");
        save_parameters(true);
        std::ostringstream result;result<<"{\"paired_frames\":"<<count_<<",\"unmatched_frames\":"<<dropped<<",\"encoded_packets\":"<<packet_count
            <<",\"timestamp_hz\":"<<timestamp_hz<<",\"clean_eos\":"<<(eos?"true":"false")<<",\"clean_stop\":"<<(clean_stop?"true":"false")
            <<",\"pre_roll_packets\":"<<pre_roll<<",\"encoder\":"<<quoted(encoder)<<",\"stop_reason\":"<<quoted(reason)<<"}\n";
        atomic_text(path+"/summary.json",result.str());done=true;
    }
    ~Recording(){if(!done){try{finish(false,false,"interrupted");}catch(...){}}}
};

inline void save_png(const AVFrame* f,const std::string& path){
    const AVCodec* codec=avcodec_find_encoder(AV_CODEC_ID_PNG);if(!codec)throw std::runtime_error("PNG encoder unavailable");
    AVCodecContext* context=avcodec_alloc_context3(codec);SwsContext* scale=nullptr;AVPacket* packet=av_packet_alloc();
    auto release=[&]{sws_freeContext(scale);avcodec_free_context(&context);av_packet_free(&packet);};
    try{
        context->width=f->width;context->height=f->height;context->pix_fmt=AV_PIX_FMT_RGB24;context->time_base={1,30};context->thread_count=1;
        check(avcodec_open2(context,codec,nullptr),"PNG encoder open");auto rgb=frame();rgb->width=f->width;rgb->height=f->height;rgb->format=AV_PIX_FMT_RGB24;
        check(av_frame_get_buffer(rgb.get(),32),"snapshot allocation");
        scale=sws_getContext(f->width,f->height,static_cast<AVPixelFormat>(f->format),f->width,f->height,AV_PIX_FMT_RGB24,SWS_BILINEAR,nullptr,nullptr,nullptr);
        if(!scale)throw std::runtime_error("snapshot converter unavailable");
        sws_scale(scale,f->data,f->linesize,0,f->height,rgb->data,rgb->linesize);
        check(avcodec_send_frame(context,rgb.get()),"PNG send");check(avcodec_receive_packet(context,packet),"PNG receive");
        std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(packet->data),packet->size);output.close();
        if(!output)throw std::runtime_error("snapshot save failed");
    }catch(...){release();throw;}release();
}
class Snapshots {
    std::future<std::string> job;
public:
    bool busy()const{return job.valid();}
    bool start(Frame left,Frame right,int64_t ts,int64_t hz,const std::string& directory,const std::string& metadata_dir="",std::optional<double> exposure_value={}){
        if(busy()||!left||!right)return false;
        MetadataBundle metadata(metadata_dir);
        job=std::async(std::launch::async,[left,right,ts,hz,directory,metadata,exposure_value]{
            std::filesystem::create_directories(directory);
            metadata.save(directory);
            save_png(left.get(),directory+"/fisheye.png");save_png(right.get(),directory+"/corrected.png");
            std::ostringstream parameters;parameters<<"{\"schema_version\":1,\"metadata\":"<<metadata.json()<<",\"camera_timestamp\":"<<ts<<",\"timestamp_hz\":"<<hz
                <<",\"observed\":{\"fisheye_width\":"<<left->width<<",\"fisheye_height\":"<<left->height<<",\"corrected_width\":"<<right->width<<",\"corrected_height\":"<<right->height
                <<"},\"exposure_sample\":{\"available\":"<<(exposure_value?"true":"false");
            if(exposure_value)parameters<<",\"timestamp_raw\":"<<ts<<",\"exposure_time_raw\":"<<std::setprecision(17)<<*exposure_value<<",\"match\":\"exact camera timestamp\"";
            parameters<<"},\"finalized\":true}\n";
            atomic_text(directory+"/recording_parameters.json",parameters.str());
            atomic_text(directory+"/pair.json","{\"camera_timestamp\":"+std::to_string(ts)+",\"timestamp_hz\":"+std::to_string(hz)+"}\n");
            return directory;
        });return true;
    }
    std::optional<std::string> poll(){if(job.valid()&&job.wait_for(std::chrono::seconds(0))==std::future_status::ready)return job.get();return {};}
    std::optional<std::string> finish(){if(job.valid())return job.get();return {};}
};
}
