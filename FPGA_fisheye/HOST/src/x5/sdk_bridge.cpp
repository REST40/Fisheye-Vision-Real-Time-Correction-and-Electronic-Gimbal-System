// Integration for the Camera 2.1.1 / Media 3.1.1 public package pair.
// Live X5 H264 preview, millisecond timestamps and RGBA callbacks validated.
#include "wire.hpp"
#include "camera_parameters.hpp"
#include <camera/camera.h>
#include <camera/device_discovery.h>
#include <ins_realtime_stitcher.h>
#include <ins_stitcher.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iostream>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <unistd.h>

static volatile std::sig_atomic_t stop_requested=0;
static void stop_signal(int){stop_requested=1;}

class PipeWriter {
    int fd;std::mutex mutex;std::condition_variable ready;
    std::deque<x5::Message> queue;size_t queued_bytes=0,peak_bytes=0;
    bool closed=false;std::atomic<bool> failed_{false};std::thread thread;
    void run(){
        while(true){
            x5::Message m;
            {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return closed||!queue.empty();});
             if(queue.empty())break;
             m=std::move(queue.front());queue.pop_front();queued_bytes-=m.data.size();}
            // Keep full-resolution pixels in their existing allocation.
            // Copying header+RGBA into another 14/30 MB vector per frame
            // consumed the processing margin at 30 fps.
            auto header=m.header();
            const std::pair<uint8_t*,size_t> parts[]={{header.data(),header.size()},{m.data.data(),m.data.size()}};
            for(const auto& part:parts){
                size_t offset=0;
                while(offset<part.second){
                    auto n=::write(fd,part.first+offset,part.second-offset);
                    if(n<0&&errno==EINTR)continue;
                    if(n<=0){std::cerr<<"IPC write failed: errno="<<errno<<'\n';failed_=true;return;}offset+=size_t(n);
                }
            }
        }
    }
public:
    explicit PipeWriter(int descriptor):fd(descriptor),thread([this]{run();}){}
    bool push(x5::Message m){
        std::lock_guard<std::mutex> lock(mutex);
        if(closed||failed_)return false;
        if(m.data.size()>x5::max_payload){std::cerr<<"IPC payload exceeds protocol limit\n";failed_=true;return false;}
        // Allow the measured first NVENC initialization burst; still bounded.
        if(queued_bytes+m.data.size()>256*1024*1024){std::cerr<<"IPC queue overflow: queued_bytes="<<queued_bytes<<", next_bytes="<<m.data.size()<<'\n';failed_=true;return false;}
        queued_bytes+=m.data.size();peak_bytes=std::max(peak_bytes,queued_bytes);queue.push_back(std::move(m));ready.notify_one();return true;
    }
    bool failed()const{return failed_;}
    size_t pending_bytes(){std::lock_guard<std::mutex> lock(mutex);return queued_bytes;}
    void close(){ {std::lock_guard<std::mutex> lock(mutex);closed=true;ready.notify_all();}if(thread.joinable()){thread.join();std::cerr<<"IPC peak queued bytes: "<<peak_bytes<<'\n';}}
    ~PipeWriter(){close();}
};

class Delegate:public ins_camera::StreamDelegate {
    std::shared_ptr<ins::RealTimeStitcher> stitcher;PipeWriter& output;uint32_t codec;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    std::once_flag first_video;
    std::mutex timestamps_mutex;std::map<int64_t,int64_t> eligible;
public:
    std::atomic<bool> failed{false};
    std::atomic<int64_t> latest_video_timestamp{0},latest_video_arrival_ns{0},latest_stitched_timestamp{0};
    std::atomic<double> stitch_callback_delay_ms{-1};
    Delegate(std::shared_ptr<ins::RealTimeStitcher> s,PipeWriter& o,uint32_t c):stitcher(std::move(s)),output(o),codec(c){}
    bool valid_timestamp(int64_t ts){
        auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(timestamps_mutex);auto i=eligible.find(ts);
        if(i==eligible.end())return false;
        latest_stitched_timestamp=ts;stitch_callback_delay_ms=double(now-i->second)/1000000;
        return true;
    }
    void OnAudioData(const uint8_t*,size_t,int64_t)override{}
    void OnVideoData(const uint8_t* data,size_t size,int64_t timestamp,uint8_t type,int index)override{
        try{
            // 1920x960 dual fisheye fits one stream. Never silently discard a lens.
            if(index!=0){failed=true;std::cerr<<"Unexpected second encoded stream; select 1920x960 preview\n";return;}
            x5::Message m;m.kind=1;m.timestamp=timestamp;m.codec=codec;m.data.assign(data,data+size);
            std::call_once(first_video,[&]{started=std::chrono::steady_clock::now();});
            // X5 can replay cached frames/zero timestamps on StartLiveStreaming.
            // Preserve every AU, but exclude a measured one-second warmup from pairing.
            if(timestamp<=0||std::chrono::steady_clock::now()-started<std::chrono::seconds(1))m.flags=x5::invalid_timestamp;
            else {
                auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                latest_video_timestamp=timestamp;latest_video_arrival_ns=now;
                std::lock_guard<std::mutex> lock(timestamps_mutex);eligible[timestamp]=now;while(eligible.size()>300)eligible.erase(eligible.begin());
            }
            if(!output.push(std::move(m))){failed=true;return;}
            stitcher->HandleVideoData(data,size,timestamp,type,index);
        }catch(const std::exception& e){failed=true;std::cerr<<"video callback: "<<e.what()<<'\n';}catch(...){failed=true;}
    }
    void OnGyroData(const std::vector<ins_camera::GyroData>& data)override{
        try{
            std::vector<ins::GyroData> converted(data.size());
            for(size_t i=0;i<data.size();++i){
                converted[i].timestamp=data[i].timestamp;
                converted[i].ax=data[i].ax;converted[i].ay=data[i].ay;converted[i].az=data[i].az;
                converted[i].gx=data[i].gx;converted[i].gy=data[i].gy;converted[i].gz=data[i].gz;
            }
            stitcher->HandleGyroData(converted);
        }catch(...){failed=true;}
    }
    void OnExposureData(const ins_camera::ExposureData& data)override{
        try{
            ins::ExposureData e{};e.exposure_time=data.exposure_time;e.timestamp=data.timestamp;stitcher->HandleExposureData(e);
            // Preserve the SDK telemetry without guessing its undocumented unit.
            x5::Message m;m.kind=4;m.data.resize(16);
            static_assert(sizeof(double)==8,"IPC requires IEEE double");
            std::memcpy(m.data.data(),&data.timestamp,8);std::memcpy(m.data.data()+8,&data.exposure_time,8);
            if(!output.push(std::move(m)))failed=true;
        }catch(...){failed=true;}
    }
};

// Media 3.1.1 uses public fields; the later 3.1.5 SDK changed calibration APIs.
template<class Info,class Preview>
auto calibration(Info& info,const Preview& p,int)->decltype(info.SetCalibration(p.offset,p.crop_info.src_width,p.crop_info.src_height,p.crop_info.dst_width,p.crop_info.dst_height,p.crop_info.crop_offset_x,p.crop_info.crop_offset_y),void()){
    const auto& c=p.crop_info;info.SetCalibration(p.offset,c.src_width,c.src_height,c.dst_width,c.dst_height,c.crop_offset_x,c.crop_offset_y);
}
template<class Info,class Preview>
void calibration(Info& info,const Preview& p,long){
    info.offset=p.offset;const auto& c=p.crop_info;
    info.window_crop_info_.src_width=c.src_width;info.window_crop_info_.src_height=c.src_height;
    info.window_crop_info_.dst_width=c.dst_width;info.window_crop_info_.dst_height=c.dst_height;
    info.window_crop_info_.crop_offset_x=c.crop_offset_x;info.window_crop_info_.crop_offset_y=c.crop_offset_y;
}

int main(int argc,char** argv){
    std::shared_ptr<ins_camera::Camera> cam;std::shared_ptr<ins::RealTimeStitcher> stitcher;
    std::unique_ptr<PipeWriter> output;
    std::shared_ptr<Delegate> delegate;
    std::atomic<bool> callback_failed{false};
    bool live=false,stitching=false,restore_failed=false;std::optional<parameters::Exposure> restore_exposure;
    auto restore_function=ins_camera::FUNCTION_MODE_NORMAL_VIDEO;
    auto cleanup=[&]{
        if(cam&&live){cam->StopLiveStreaming();live=false;}
        if(stitcher&&stitching){stitcher->CancelStitch();stitching=false;}
        if(cam&&restore_exposure){
            try{if(!restore_exposure->restore(*cam,restore_function))restore_failed=true;}
            catch(...){restore_failed=true;}
            restore_exposure.reset();
            if(restore_failed)std::cerr<<"exposure restore failed\n";
        }
        if(cam)cam->Close();
    };
    try{
        int fd=-1,out_w=1920,out_h=960,exposure_function=7,video_delay_ms=-1;uint32_t codec=0;std::string models,metadata_dir;
        parameters::Request exposure_request;
        bool restore_on_exit=false,flow_state=true;
        for(int i=1;i<argc;++i){std::string a=argv[i];if(i+1>=argc)throw std::runtime_error("missing option value");std::string v=argv[++i];
            if(a=="--ipc-fd")fd=std::stoi(v);else if(a=="--codec")codec=v=="h264"?1:v=="hevc"?2:0;else if(a=="--models")models=v;
            else if(a=="--output-width")out_w=std::stoi(v);else if(a=="--output-height")out_h=std::stoi(v);
            else if(a=="--video-delay-ms"){video_delay_ms=std::stoi(v);if(video_delay_ms< -1||video_delay_ms>5000)throw std::runtime_error("invalid video delay");}
            else if(a=="--flow-state"){if(v!="0"&&v!="1")throw std::runtime_error("invalid flow state");flow_state=v=="1";}
            else if(a=="--metadata-dir")metadata_dir=v;
            else if(a=="--exposure-mode")exposure_request.mode=v;
            else if(a=="--iso")exposure_request.iso=std::stoi(v);
            else if(a=="--shutter-seconds")exposure_request.shutter=std::stod(v);
            else if(a=="--ev-raw")exposure_request.ev=std::stoi(v);
            else if(a=="--exposure-function")exposure_function=std::stoi(v);
            else if(a=="--restore-exposure-on-exit")restore_on_exit=v=="1";
            else throw std::runtime_error("unknown option: "+a);}
        if(fd<0||!codec||models.empty())throw std::runtime_error("usage: x5_sdk_bridge --ipc-fd FD --codec h264|hevc --models PATH");
        if(out_w!=out_h*2||out_w<640||out_w>3840||out_w%2||out_h%2)throw std::runtime_error("invalid 2:1 output size");
        exposure_request.validate();
        if(exposure_function!=7&&exposure_function!=1)throw std::runtime_error("invalid exposure function mode");
        std::signal(SIGINT,stop_signal);std::signal(SIGTERM,stop_signal);std::signal(SIGPIPE,SIG_IGN);
        output=std::make_unique<PipeWriter>(fd);
        ins::InitEnv();ins::SetLogLevel(ins::InsLogLevel::WARNING);
        if(models.back()!='/')models+='/';
        ins::SetModelFileRootDir(models);
        ins_camera::SetLogLevel(ins_camera::LogLevel::WARNING);
        ins_camera::DeviceDiscovery discovery;auto list=discovery.GetAvailableDevices();
        if(list.empty()){discovery.FreeDeviceDescriptors(list);throw std::runtime_error("no camera: select Android USB mode and check USB permissions");}
        if(list.size()!=1){discovery.FreeDeviceDescriptors(list);throw std::runtime_error("connect exactly one X5 to avoid selecting the wrong camera");}
        if(list[0].camera_type!=ins_camera::CameraType::Insta360X5){discovery.FreeDeviceDescriptors(list);throw std::runtime_error("detected camera is not an X5");}
        std::cerr<<"Camera: "<<list[0].camera_name<<", firmware: "<<list[0].fw_version<<'\n';
        const auto name=list[0].camera_name,firmware=list[0].fw_version,serial=list[0].serial_number;
        cam=std::make_shared<ins_camera::Camera>(list[0].info);
        bool opened=cam->Open();discovery.FreeDeviceDescriptors(list);if(!opened)throw std::runtime_error("camera Open failed");
        cam->SetTimeout(2500);
        cam->SetActiveSensor(ins_camera::SensorDevice::SENSOR_DEVICE_ALL);
        if(!cam->EnableInCameraStitching(false))throw std::runtime_error("cannot disable in-camera stitching; raw branch would be invalid");
        auto p=cam->GetPreviewParam();
        const auto function=static_cast<ins_camera::CameraFunctionMode>(exposure_function);
        const auto original=parameters::Exposure::read(cam->GetExposureSettings(function));
        if(restore_on_exit&&exposure_request.mode!="keep"){restore_exposure=original;restore_function=function;}
        try{exposure_request.apply(*cam,function);}catch(...){original.restore(*cam,function);throw;}
        auto save_metadata=[&](uint64_t request_id){
            if(metadata_dir.empty())return;
            auto actual=parameters::Exposure::read(cam->GetExposureSettings(function));
            bool matched=exposure_request.matches(actual);
            std::ostringstream json;json<<"{\"schema_version\":1,\"request_id\":"<<request_id<<",\"sampled_at_unix_ms\":"<<parameters::unix_ms()
                <<",\"camera_media_time_raw\":"<<cam->GetCameraMediaTime()<<",\"camera_media_time_unit\":\"SDK undocumented\",\"camera\":{\"name\":"<<parameters::quote(name)
                <<",\"firmware\":"<<parameters::quote(firmware)<<",\"serial_number\":"<<parameters::quote(serial)<<"},"
                <<"\"camera_sdk_version\":\"2.1.1\",\"media_sdk_version\":\"3.1.1\",\"function_mode\":"<<exposure_function
                <<",\"function_mode_name\":"<<parameters::quote(exposure_function==7?"FUNCTION_MODE_NORMAL_VIDEO":"FUNCTION_MODE_LIVE_STREAM")
                <<",\"requested_exposure\":"<<exposure_request.json()<<",\"exposure\":"<<actual.json()<<",\"original_exposure\":"<<original.json()
                <<",\"readback_matches_request\":"<<(matched?"true":"false")<<",\"valid\":"<<(matched?"true":"false")
                <<",\"in_camera_stitching\":false,\"correction\":{\"provider\":\"official Insta360 Media SDK on PC\",\"projection\":\"equirectangular_360\",\"flow_state\":"<<(flow_state?"true":"false")<<",\"video_delay_ms\":"<<video_delay_ms<<"},"
                <<"\"telemetry\":{\"file\":\"exposure.csv\",\"values\":\"SDK callback raw values\",\"timestamp_unit\":"<<parameters::quote(firmware=="v1.13.21"?"milliseconds":"unverified")
                <<",\"exposure_time_unit\":"<<parameters::quote(firmware=="v1.13.21"?"seconds":"unverified")
                <<",\"unit_evidence\":\"Empirically verified against manual shutters and preview timestamps on X5 v1.13.21, Camera SDK 2.1.1\",\"per_frame_iso_available\":false}}\n";
            parameters::atomic_text(std::filesystem::path(metadata_dir)/"camera_metadata.json",json.str());
        };
        if(!metadata_dir.empty()){
            std::filesystem::create_directories(metadata_dir);
            parameters::atomic_text(std::filesystem::path(metadata_dir)/"calibration.json",parameters::calibration_json(p,cam->GetCameraOffset()));
            save_metadata(0);
        }
        ins::CameraInfo info;info.cameraName=p.camera_name;
        if(p.encode_type==ins_camera::VideoEncodeType::H264){info.decode_type=ins::VideoDecodeType::kH264;if(codec!=1)throw std::runtime_error("preview codec is H264; configuration disagrees");}
        else if(p.encode_type==ins_camera::VideoEncodeType::H265){info.decode_type=ins::VideoDecodeType::kH265;if(codec!=2)throw std::runtime_error("preview codec is HEVC; configuration disagrees");}
        else throw std::runtime_error("unknown preview codec");
        calibration(info,p,0);
        // The matching 3.1.1 example leaves gyro/sweep timestamps at defaults.
        std::cerr<<"SDK preview encode_type="<<static_cast<int>(p.encode_type)<<"; configured codec="<<codec<<'\n';
        stitcher=std::make_shared<ins::RealTimeStitcher>();stitcher->SetCameraInfo(info);
        // This SDK dereferences its stabilization provider in StartStitch even
        // when FlowState is disabled; enabled mode was validated on this X5.
        stitcher->SetStitchType(ins::STITCH_TYPE::DYNAMICSTITCH);stitcher->EnableFlowState(flow_state);stitcher->SetOutputSize(out_w,out_h);
        if(video_delay_ms>=0)stitcher->SetVideoDelayMs(video_delay_ms);
        stitcher->SetStitchStateCallback([&](int code,const char* detail){callback_failed=true;std::cerr<<"Media SDK error "<<code<<": "<<(detail?detail:"")<<'\n';});
        stitcher->SetStitchRealTimeDataCallback([&](uint8_t* data[4],int stride[4],int w,int h,int format,int64_t timestamp){
            try{
                if(!delegate||!delegate->valid_timestamp(timestamp))return;
                // Media 3.1.1's live callback uses its own format code 0 for
                // RGBA (documented sample reads CV_8UC4); not AVPixelFormat.
                if(format!=0){callback_failed=true;std::cerr<<"Unsupported stitched pixel format: "<<format<<'\n';return;}
                if(w<=0||h<=0||w>8192||h>8192||!data[0]||stride[0]<w*4||uint64_t(w)*h*4>x5::max_payload){callback_failed=true;return;}
                x5::Message m;m.kind=2;m.width=w;m.height=h;m.timestamp=timestamp;m.data.resize(size_t(w)*h*4);
                for(int y=0;y<h;++y)std::memcpy(m.data.data()+size_t(y)*w*4,data[0]+y*stride[0],w*4);
                if(!output->push(std::move(m)))callback_failed=true;
            }catch(...){callback_failed=true;}
        });
        delegate=std::make_shared<Delegate>(stitcher,*output,codec);
        std::shared_ptr<ins_camera::StreamDelegate> sdk_delegate=delegate;
        cam->SetStreamDelegate(sdk_delegate);
        ins_camera::LiveStreamParam param;
        param.video_resolution=ins_camera::VideoResolution::RES_1920_960P30;param.lrv_video_resulution=param.video_resolution;
        param.video_bitrate=12*1024*1024;param.enable_audio=false;param.using_lrv=false;
        if(!cam->StartLiveStreaming(param))throw std::runtime_error("StartLiveStreaming failed");
        live=true;
        stitcher->StartStitch();stitching=true;
        uint64_t last_request=0;auto last_latency=std::chrono::steady_clock::now();
        while(!stop_requested&&!delegate->failed&&!callback_failed&&!output->failed()){
            if(!metadata_dir.empty()){
                auto now=std::chrono::steady_clock::now();
                if(now-last_latency>=std::chrono::milliseconds(200)){
                    last_latency=now;
                    std::ostringstream j;j<<"{\"latest_video_timestamp\":"<<delegate->latest_video_timestamp.load()
                        <<",\"latest_video_arrival_ns\":"<<delegate->latest_video_arrival_ns.load()
                        <<",\"latest_stitched_timestamp\":"<<delegate->latest_stitched_timestamp.load()
                        <<",\"encoded_callback_to_stitch_ms\":"<<delegate->stitch_callback_delay_ms.load()
                        <<",\"ipc_pending_bytes\":"<<output->pending_bytes()<<",\"flow_state\":"<<(flow_state?"true":"false")<<",\"video_delay_ms\":"<<video_delay_ms<<"}\n";
                    parameters::atomic_text(std::filesystem::path(metadata_dir)/"sdk_latency.json",j.str());
                }
                std::ifstream input(std::filesystem::path(metadata_dir)/"metadata_request.txt");uint64_t request_id=0;
                if(input>>request_id&&request_id>last_request){
                    try{save_metadata(request_id);}catch(const std::exception& e){
                        parameters::atomic_text(std::filesystem::path(metadata_dir)/"camera_metadata.json","{\"request_id\":"+std::to_string(request_id)+",\"valid\":false,\"error\":"+parameters::quote(e.what())+"}\n");
                    }
                    last_request=request_id;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        cleanup();
        if(restore_failed)throw std::runtime_error("exposure restore failed after test capture");
        if(delegate->failed||callback_failed||output->failed())throw std::runtime_error("capture failed or bounded IPC queue overflowed; inspect SDK log");
        x5::Message eos;eos.kind=3;output->push(std::move(eos));output->close();
        return output->failed()?1:0;
    }catch(const std::exception& e){std::cerr<<"x5_sdk_bridge: "<<e.what()<<'\n';try{cleanup();}catch(...){}return 1;}
}
