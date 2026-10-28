#include "camera_parameters.hpp"
#include <camera/device_discovery.h>
#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

static volatile std::sig_atomic_t stopping=0;
static void stop_signal(int){stopping=1;}
class Probe:public ins_camera::StreamDelegate {
    std::mutex mutex;std::ofstream video,packets,exposure;
public:
    std::atomic<int> phase{0};std::atomic<bool> failed{false};std::atomic<uint64_t> count{0};
    explicit Probe(const std::filesystem::path& path):video(path/"preview.h264",std::ios::binary),packets(path/"packets.csv"),exposure(path/"exposure.csv"){
        packets<<"packet_id,phase,camera_timestamp,bytes\n";exposure<<"phase,timestamp_raw,exposure_time_raw\n";
    }
    void OnAudioData(const uint8_t*,size_t,int64_t)override{}
    void OnGyroData(const std::vector<ins_camera::GyroData>&)override{}
    void OnExposureData(const ins_camera::ExposureData& e)override{
        std::lock_guard<std::mutex> lock(mutex);exposure<<std::setprecision(17)<<phase<<','<<e.timestamp<<','<<e.exposure_time<<'\n';if(!exposure)failed=true;
    }
    void OnVideoData(const uint8_t* data,size_t size,int64_t timestamp,uint8_t,int index)override{
        std::lock_guard<std::mutex> lock(mutex);
        if(index!=0||count>1500){failed=true;return;}
        video.write(reinterpret_cast<const char*>(data),size);packets<<count++<<','<<phase<<','<<timestamp<<','<<size<<'\n';
        if(!video||!packets)failed=true;
    }
};
int main(int argc,char** argv){
    std::shared_ptr<ins_camera::Camera> camera;bool live=false;std::map<int,parameters::Exposure> originals;
    std::ofstream events;bool restored=true;
    auto cleanup=[&]{
        if(!camera)return;
        if(live){camera->StopLiveStreaming();live=false;}
        for(const auto& item:originals){
            bool okay=item.second.restore(*camera,static_cast<ins_camera::CameraFunctionMode>(item.first));
            restored&=okay;
            if(events)events<<"{\"restore_function\":"<<item.first<<",\"okay\":"<<(okay?"true":"false")<<"}\n"<<std::flush;
        }
        originals.clear();camera->Close();camera.reset();
    };
    try{
        if(argc!=2&&(argc!=3||std::string(argv[2])!="--matrix"))throw std::runtime_error("usage: x5_exposure_probe NEW_DIRECTORY [--matrix]");
        const bool matrix=argc==3;
        std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir.parent_path());
        if(!std::filesystem::create_directory(dir))throw std::runtime_error("output must be a new directory");
        events.open(dir/"events.jsonl");std::signal(SIGINT,stop_signal);std::signal(SIGTERM,stop_signal);
        ins_camera::SetLogLevel(ins_camera::LogLevel::WARNING);ins_camera::DeviceDiscovery discovery;auto devices=discovery.GetAvailableDevices();
        if(devices.size()!=1||devices[0].camera_type!=ins_camera::CameraType::Insta360X5){discovery.FreeDeviceDescriptors(devices);throw std::runtime_error("exactly one X5 required");}
        events<<"{\"camera\":"<<parameters::quote(devices[0].camera_name)<<",\"firmware\":"<<parameters::quote(devices[0].fw_version)<<"}\n";
        camera=std::make_shared<ins_camera::Camera>(devices[0].info);bool opened=camera->Open();discovery.FreeDeviceDescriptors(devices);
        if(!opened)throw std::runtime_error("camera Open failed");
        camera->SetTimeout(2500);camera->SetActiveSensor(ins_camera::SensorDevice::SENSOR_DEVICE_ALL);
        if(!camera->EnableInCameraStitching(false))throw std::runtime_error("cannot disable in-camera stitching");
        for(int f:{7,1}){
            originals.emplace(f,parameters::Exposure::read(camera->GetExposureSettings(static_cast<ins_camera::CameraFunctionMode>(f))));
            events<<"{\"original_function\":"<<f<<",\"exposure\":"<<originals.at(f).json()<<"}\n"<<std::flush;
        }
        auto preview=camera->GetPreviewParam();if(preview.encode_type!=ins_camera::VideoEncodeType::H264)throw std::runtime_error("probe expects H264");
        parameters::atomic_text(dir/"calibration.json",parameters::calibration_json(preview,camera->GetCameraOffset()));
        auto probe=std::make_shared<Probe>(dir);std::shared_ptr<ins_camera::StreamDelegate> delegate=probe;camera->SetStreamDelegate(delegate);
        ins_camera::LiveStreamParam stream{};stream.video_resolution=ins_camera::VideoResolution::RES_1920_960P30;stream.lrv_video_resulution=stream.video_resolution;
        stream.video_bitrate=12*1024*1024;stream.enable_audio=false;stream.using_lrv=false;
        if(!camera->StartLiveStreaming(stream))throw std::runtime_error("preview start failed");
        live=true;
        auto dwell=[&](int milliseconds=2700){auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);while(!stopping&&!probe->failed&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(50));};
        dwell();int phase=0;
        auto sample=[&](const parameters::Request& request,int f,int milliseconds){
            probe->phase=++phase;
            try{
                auto actual=request.apply(*camera,static_cast<ins_camera::CameraFunctionMode>(f));
                events<<"{\"phase\":"<<phase<<",\"function_mode\":"<<f<<",\"requested\":"<<request.json()<<",\"readback\":"<<actual.json()<<",\"matched\":true}\n"<<std::flush;
            }catch(const std::exception& e){events<<"{\"phase\":"<<phase<<",\"function_mode\":"<<f<<",\"requested\":"<<request.json()<<",\"error\":"<<parameters::quote(e.what())<<"}\n"<<std::flush;}
            dwell(milliseconds);
        };
        if(matrix){
            for(int iso:{100,200,400,800,1600,3200}){if(stopping||probe->failed)break;parameters::Request r;r.mode="manual";r.iso=iso;r.shutter=1.0/100;sample(r,7,900);}
            for(int denominator:{30,50,60,100,120,200,240,500,1000,2000,4000,8000}){if(stopping||probe->failed)break;parameters::Request r;r.mode="manual";r.iso=200;r.shutter=1.0/denominator;sample(r,7,600);}
            for(int ev=-80;ev<=80;ev+=10){if(stopping||probe->failed)break;parameters::Request r;r.mode="auto";r.ev=ev;sample(r,7,600);}
        }else for(int f:{7,1}){
            auto function=static_cast<ins_camera::CameraFunctionMode>(f);
            for(int test=0;test<4&&!stopping&&!probe->failed;++test){
                parameters::Request request;request.mode=test<2?"manual":"auto";request.iso=test==1?800:100;request.shutter=test==0?1.0/1000:1.0/60;request.ev=test==3?20:0;
                sample(request,f,2700);
            }
            if(!originals.at(f).restore(*camera,function))throw std::runtime_error("intermediate exposure restore failed");
        }
        cleanup();std::cout<<"preview_packets="<<probe->count<<", originals_restored="<<restored<<'\n';return !restored||probe->failed||!probe->count?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';try{cleanup();}catch(...){}return 1;}
}
