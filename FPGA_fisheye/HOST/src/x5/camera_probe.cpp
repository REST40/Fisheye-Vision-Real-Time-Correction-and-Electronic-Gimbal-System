#include <cstdint>
#include <camera/camera.h>
#include <camera/device_discovery.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

class Probe : public ins_camera::StreamDelegate {
    std::mutex mutex;
    std::filesystem::path directory;
    std::ofstream csv;
    std::map<int,std::ofstream> streams;
    std::string extension;
    uint64_t total_bytes=0;
public:
    std::atomic<uint64_t> packets{0};
    std::atomic<bool> failed{false};
    Probe(std::filesystem::path dir,std::string ext):directory(dir),csv(dir/"packets.csv"),extension(ext){
        csv<<"packet_id,timestamp,stream_type,stream_index,bytes,host_monotonic_ns\n";
    }
    void OnAudioData(const uint8_t*,size_t,int64_t)override{}
    void OnGyroData(const std::vector<ins_camera::GyroData>&)override{}
    void OnExposureData(const ins_camera::ExposureData&)override{}
    void OnVideoData(const uint8_t* data,size_t size,int64_t timestamp,uint8_t type,int index)override{
        try{
            std::lock_guard<std::mutex> lock(mutex);
            if(index<0||index>3||total_bytes+size>512*1024*1024){failed=true;return;}
            auto& file=streams[index];
            if(!file.is_open())file.open(directory/("stream"+std::to_string(index)+extension),std::ios::binary);
            file.write(reinterpret_cast<const char*>(data),size);
            auto now=std::chrono::steady_clock::now().time_since_epoch();
            csv<<packets++<<','<<timestamp<<','<<int(type)<<','<<index<<','<<size<<','
               <<std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()<<'\n';
            total_bytes+=size;
            if(!file||!csv)failed=true;
        }catch(...){failed=true;}
    }
};

int main(int argc,char** argv){
    std::shared_ptr<ins_camera::Camera> camera;bool live=false;
    auto cleanup=[&]{if(camera){if(live){camera->StopLiveStreaming();live=false;}camera->Close();}};
    try{
        if(argc!=2)throw std::runtime_error("usage: x5_camera_probe NEW_OUTPUT_DIRECTORY");
        std::filesystem::path directory=argv[1];
        if(!directory.parent_path().empty())std::filesystem::create_directories(directory.parent_path());
        if(!std::filesystem::create_directory(directory))throw std::runtime_error("output directory must be new");
        ins_camera::SetLogLevel(ins_camera::LogLevel::WARNING);
        ins_camera::DeviceDiscovery discovery;auto devices=discovery.GetAvailableDevices();
        if(devices.size()!=1){discovery.FreeDeviceDescriptors(devices);throw std::runtime_error("expected one camera");}
        if(devices[0].camera_type!=ins_camera::CameraType::Insta360X5){discovery.FreeDeviceDescriptors(devices);throw std::runtime_error("expected X5");}
        std::string firmware=devices[0].fw_version;
        camera=std::make_shared<ins_camera::Camera>(devices[0].info);
        bool opened=camera->Open();discovery.FreeDeviceDescriptors(devices);
        if(!opened)throw std::runtime_error("camera Open failed");
        camera->SetActiveSensor(ins_camera::SensorDevice::SENSOR_DEVICE_ALL);
        if(!camera->EnableInCameraStitching(false))throw std::runtime_error("cannot disable in-camera stitching");
        auto preview=camera->GetPreviewParam();const auto& c=preview.crop_info;
        std::string codec=preview.encode_type==ins_camera::VideoEncodeType::H264?"h264":"hevc";
        std::ofstream meta(directory/"metadata.json");
        meta<<"{\n\"camera\":"<<std::quoted(preview.camera_name)<<",\n\"firmware\":"<<std::quoted(firmware)
            <<",\n\"codec\":"<<std::quoted(codec)<<",\n\"encode_type\":"<<int(preview.encode_type)
            <<",\n\"calibration_count\":"<<preview.offset.size()<<",\n\"crop\":["<<c.src_width<<','<<c.src_height<<','<<c.dst_width<<','<<c.dst_height<<','<<c.crop_offset_x<<','<<c.crop_offset_y<<"]\n}\n";meta.close();
        auto probe=std::make_shared<Probe>(directory,codec=="h264"?".h264":".h265");
        std::shared_ptr<ins_camera::StreamDelegate> delegate=probe;camera->SetStreamDelegate(delegate);
        ins_camera::LiveStreamParam param{};param.video_resolution=ins_camera::VideoResolution::RES_1920_960P30;
        param.lrv_video_resulution=param.video_resolution;param.video_bitrate=12*1024*1024;param.enable_audio=false;param.using_lrv=false;
        if(!camera->StartLiveStreaming(param))throw std::runtime_error("StartLiveStreaming failed");
        live=true;
        auto start=std::chrono::steady_clock::now();
        while(!probe->failed&&std::chrono::steady_clock::now()-start<std::chrono::seconds(8))std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cleanup();
        std::cout<<"packets="<<probe->packets<<", codec="<<codec<<'\n';
        return probe->failed||probe->packets==0?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';try{cleanup();}catch(...){}return 1;}
}
