#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace app {
// Independent of the panorama pairer and Recording: decoded camera frames are
// cropped and encoded once, on a bounded background worker, during acquisition.
class LensCapture {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::deque<Frame> queue;
    std::atomic<bool> active{false};
    std::atomic<uint64_t> saved{0};
    std::atomic<int64_t> elapsed_us{0};
    bool closed=false,complete=false;
    std::optional<int64_t> epoch,last;
    std::string path,problem;
    int64_t hz=1000;

    static Frame crop(const Frame& raw,int side){
        auto out=Frame(av_frame_clone(raw.get()),[](AVFrame* f){av_frame_free(&f);});
        if(!out)throw std::bad_alloc();
        out->crop_left=side*raw->height;out->crop_right=(1-side)*raw->height;
        out->crop_top=out->crop_bottom=0;
        check(av_frame_apply_cropping(out.get(),AV_FRAME_CROP_UNALIGNED),"lens crop");
        return out;
    }
    void run(MetadataBundle metadata){
        int width=0,height=0;int64_t first=0,previous_pts=0,actual_duration=0;
        Frame previous;std::unique_ptr<Encoder> encoders[2];bool directory_created=false;
        auto write=[&](const Frame& raw,int64_t pts,int64_t duration){
            for(int i=0;i<2;++i){auto half=crop(raw,i);encoders[i]->write(half.get(),pts,duration);}
            ++saved;actual_duration=pts+duration;
        };
        try{
            if(std::filesystem::exists(path))throw std::runtime_error("dataset directory already exists");
            directory_created=std::filesystem::create_directories(path);
            if(!directory_created)throw std::runtime_error("dataset directory already exists");
            metadata.save(path);
            std::ofstream csv(path+"/frames.csv");
            if(!csv)throw std::runtime_error("dataset is not writable");
            csv<<"frame_id,camera_timestamp,pts_us\n";
            uint64_t received=0;
            for(;;){
                Frame raw;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock,[&]{return closed||!queue.empty();});
                    if(queue.empty())break;
                    raw=queue.front();queue.pop_front();
                }
                if(!previous){
                    width=height=raw->height;first=raw->pts;
                    for(int i=0;i<2;++i){
                        const std::string name=i?"rear":"front";
                        auto half=crop(raw,i);save_png(half.get(),path+"/"+name+".png");
                        encoders[i]=std::make_unique<Encoder>(path+"/"+name+"_10s.mp4",width,height,30,"mjpeg",0,
                                                             static_cast<AVPixelFormat>(half->format));
                    }
                }
                const auto pts=av_rescale_q(raw->pts-first,{1,int(hz)},{1,1000000});
                if(previous)write(previous,previous_pts,pts-previous_pts);
                csv<<received++<<','<<raw->pts<<','<<pts<<'\n';
                if(!csv)throw std::runtime_error("dataset timestamps write failed");
                previous=raw;previous_pts=pts;
            }
            bool full;
            {std::lock_guard<std::mutex> lock(mutex);full=complete&&problem.empty();}
            if(previous)write(previous,previous_pts,full?10000000-previous_pts:std::min<int64_t>(33333,10000000-previous_pts));
            for(auto& encoder:encoders)if(encoder)encoder->finish();
            csv.close();if(!csv)throw std::runtime_error("dataset timestamps flush failed");
        }catch(const std::exception& e){
            std::lock_guard<std::mutex> lock(mutex);problem=e.what();closed=true;complete=false;queue.clear();
        }
        // Destroy encoder instances before advertising the dataset as saved.
        for(auto& encoder:encoders)encoder.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            bool passed=complete&&problem.empty()&&saved>1;
            if(!passed&&problem.empty())problem="采集不足10秒，已保留未完成文件，请重新采集";
            if(directory_created){
                try{
                    std::ostringstream out;
                    out<<"{\"schema_version\":1,\"mode\":\"direct_decoded_lens_capture\",\"passed\":"<<(passed?"true":"false")
                       <<",\"front_side\":\"left\",\"rear_side\":\"right\",\"mapping_evidence\":\"用户遮挡屏幕侧镜头，确认左半变黑\""
                       <<",\"width\":"<<width<<",\"height\":"<<height<<",\"frames_per_lens\":"<<saved
                       <<",\"requested_duration_seconds\":10,\"duration_seconds\":"<<std::setprecision(12)<<actual_duration/1000000.0
                       <<",\"timestamp_hz\":"<<hz<<",\"photo_camera_timestamp\":"<<first<<",\"photo_video_frame_index\":0"
                       <<",\"video_codec\":\"mjpeg\",\"video_profile\":\"Baseline\",\"pixel_format\":\"yuvj420p\",\"jpeg_qscale\":2"
                       <<",\"camera_pts_preserved\":true,\"source_is_bayer_raw\":false,\"geometry\":\"complete unrectified single-lens fisheye\""
                       <<",\"encoding_note\":\"camera H264/HEVC decoded and cropped directly; one lossy MJPEG encoding; no intermediate H264 recording\""
                       <<",\"error\":"<<quoted(problem)<<"}\n";
                    atomic_text(path+"/manifest.json",out.str());
                }catch(const std::exception& e){problem=e.what();}
            }
            active=false;
        }
    }
public:
    bool busy()const{return active;}
    uint64_t frames()const{return saved;}
    double elapsed()const{return elapsed_us/1000000.0;}
    std::string directory()const{std::lock_guard<std::mutex> lock(mutex);return path;}
    std::string error()const{std::lock_guard<std::mutex> lock(mutex);return problem;}
    bool start(const std::string& destination,const std::string& metadata_dir,int64_t timestamp_hz){
        if(active)return false;
        if(worker.joinable())worker.join();
        MetadataBundle metadata(metadata_dir);
        {std::lock_guard<std::mutex> lock(mutex);
            path=destination;problem.clear();hz=timestamp_hz;epoch.reset();last.reset();queue.clear();
            saved=0;elapsed_us=0;closed=false;complete=false;active=true;
        }
        worker=std::thread([this,metadata]{run(metadata);});return true;
    }
    void accept(const Frame& raw){
        if(!active)return;
        std::lock_guard<std::mutex> lock(mutex);if(closed)return;
        if(raw->width!=raw->height*2||raw->height%2){
            problem="相机输入不是两个等大的完整鱼眼图，已停止前后采集";closed=true;
        }else if(last&&raw->pts<=*last){
            problem="相机时间戳不递增，已停止前后采集";closed=true;
        }else{
            if(!epoch)epoch=raw->pts;
            auto pts=av_rescale_q(raw->pts-*epoch,{1,int(hz)},{1,1000000});
            elapsed_us=std::min<int64_t>(pts,10000000);last=raw->pts;
            if(pts>=10000000){complete=true;closed=true;}
            else if(queue.size()>=60){problem="前后采集写入未跟上相机帧率，已保留未完成文件";closed=true;}
            else queue.push_back(raw);
        }
        wake.notify_one();
    }
    void finish(){
        {std::lock_guard<std::mutex> lock(mutex);closed=true;}
        wake.notify_one();if(worker.joinable())worker.join();
    }
    ~LensCapture(){finish();}
};
}
