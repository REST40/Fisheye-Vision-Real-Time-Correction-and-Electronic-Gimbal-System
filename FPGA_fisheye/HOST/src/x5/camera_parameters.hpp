#pragma once
#include <cstdint>
#include <camera/camera.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace parameters {
inline std::string quote(const std::string& value){
    std::ostringstream out;out<<'"';
    for(unsigned char c:value){
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
inline int64_t unix_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
inline void atomic_text(const std::filesystem::path& path,const std::string& text){
    auto temp=path;temp+=".tmp";std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<text;out.close();
    if(!out)throw std::runtime_error("cannot save camera parameters: "+path.string());
    std::filesystem::rename(temp,path);
}
inline const char* mode_name(int mode){
    switch(mode){case 0:return "auto";case 1:return "iso_priority";case 2:return "shutter_priority";case 3:return "manual";case 4:return "adaptive";case 5:return "full_auto";default:return "unknown";}
}
// Keep numeric values, not a shallow copy of the SDK's private shared_ptr.
struct Exposure {
    int mode=0,iso=0,ev=0,iso_limit=0;double shutter=0;
    static Exposure read(const std::shared_ptr<ins_camera::ExposureSettings>& s){
        if(!s)throw std::runtime_error("GetExposureSettings returned no settings");
        return {int(s->ExposureMode()),s->Iso(),s->EVBias(),s->VideoISOTopLimit(),s->ShutterSpeed()};
    }
    std::string json()const{
        std::ostringstream out;out<<std::setprecision(17)<<"{\"sdk_mode\":"<<mode<<",\"mode\":"<<quote(mode_name(mode))
            <<",\"iso_setting\":"<<iso<<",\"shutter_seconds_setting\":"<<shutter<<",\"ev_bias_raw\":"<<ev
            <<",\"ev_stops\":"<<double(ev)/20.0<<",\"video_iso_top_limit\":"<<iso_limit
            <<",\"iso_is_per_frame_measurement\":false,\"automatic_values_are_settings_only\":true}";return out.str();
    }
    bool restore(ins_camera::Camera& camera,ins_camera::CameraFunctionMode function)const{
        auto settings=camera.GetExposureSettings(function);if(!settings)return false;
        settings->SetExposureMode(static_cast<ins_camera::PhotographyOptions_ExposureMode>(mode));settings->SetEVBias(ev);
        if(iso>0)settings->SetIso(iso);
        if(shutter>0)settings->SetShutterSpeed(shutter);
        if(iso_limit>0)settings->SetVideoISOTopLimit(iso_limit);
        return camera.SetExposureSettings(function,settings);
    }
};
struct Request {
    std::string mode="keep";int iso=100,ev=0;double shutter=1.0/60;
    void validate()const{
        if(mode!="keep"&&mode!="auto"&&mode!="manual")throw std::runtime_error("invalid exposure mode");
        if(ev<-80||ev>80||ev%10)throw std::runtime_error("EV must be -4 to +4 in 0.5 EV steps (SDK raw = EV * 20)");
        if(iso!=100&&iso!=200&&iso!=400&&iso!=800&&iso!=1600&&iso!=3200)throw std::runtime_error("unsupported ISO selection");
        bool allowed=false;for(int n:{30,50,60,100,120,200,240,500,1000,2000,4000,8000})if(std::abs(shutter-1.0/n)<1e-10)allowed=true;
        if(!std::isfinite(shutter)||!allowed)throw std::runtime_error("unsupported shutter selection");
    }
    std::string json()const{
        std::ostringstream out;out<<std::setprecision(17)<<"{\"mode\":"<<quote(mode)<<",\"iso\":"<<iso<<",\"shutter_seconds\":"<<shutter
            <<",\"ev_stops\":"<<double(ev)/20.0<<",\"ev_bias_raw\":"<<ev<<",\"active_fields\":"
            <<(mode=="keep"?"[]":mode=="auto"?"[\"mode\",\"ev_stops\"]":"[\"mode\",\"iso\",\"shutter_seconds\"]")<<'}';return out.str();
    }
    bool matches(const Exposure& value)const{
        if(mode=="keep")return true;
        if(mode=="auto")return value.mode==0&&value.ev==ev;
        return value.mode==3&&value.iso==iso&&std::abs(value.shutter-shutter)<1e-7;
    }
    Exposure apply(ins_camera::Camera& camera,ins_camera::CameraFunctionMode function)const{
        validate();auto settings=camera.GetExposureSettings(function);
        if(!settings)throw std::runtime_error("camera exposure readback unavailable");
        if(mode!="keep"){
            settings->SetExposureMode(mode=="auto"?ins_camera::AUTO:ins_camera::MANUAL);
            if(mode=="auto")settings->SetEVBias(ev);
            else {settings->SetIso(iso);settings->SetShutterSpeed(shutter);}
            if(!camera.SetExposureSettings(function,settings))throw std::runtime_error("X5 rejected exposure settings");
        }
        auto actual=Exposure::read(camera.GetExposureSettings(function));
        if(!matches(actual))throw std::runtime_error("X5 exposure readback does not match requested settings");
        return actual;
    }
};
inline std::string strings(const std::vector<std::string>& values){
    std::ostringstream out;out<<'[';for(size_t i=0;i<values.size();++i){if(i)out<<',';out<<quote(values[i]);}out<<']';return out.str();
}
inline std::string calibration_json(const ins_camera::PreviewParam& p,const std::vector<std::string>& offsets){
    const auto& c=p.crop_info;std::ostringstream out;
    out<<"{\"schema_version\":1,\"source\":\"X5 Camera SDK GetPreviewParam / GetCameraOffset\",\"representation\":\"opaque_vendor_calibration\","
       <<"\"preview_offsets\":"<<strings(p.offset)<<",\"camera_offsets\":"<<strings(offsets)
       <<",\"crop\":{\"src_width\":"<<c.src_width<<",\"src_height\":"<<c.src_height<<",\"dst_width\":"<<c.dst_width<<",\"dst_height\":"<<c.dst_height
       <<",\"crop_offset_x\":"<<c.crop_offset_x<<",\"crop_offset_y\":"<<c.crop_offset_y<<"},"
       <<"\"opencv_K\":null,\"opencv_D\":null,\"model_documented\":false,\"note\":\"Passed unchanged to official Media SDK; do not interpret as OpenCV K/D.\"}\n";
    return out.str();
}
}
