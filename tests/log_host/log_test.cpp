#include "app_usb_log_service.hpp"
#include <cassert>
#include <string>
#include <iostream>
#include <cstring>
static control_state_t state;
static bool mounted=true,fail_write=false;
static std::string contents;
static int writes=0;
control_state_t ControlService::snapshot(){return state;}
int64_t esp_timer_get_time(){return 1000000;}
extern "C" {
bool msc_is_mounted(){return mounted;}
esp_err_t msc_write_file(const char* path,const void* data,size_t len,bool append){
 assert(std::string(path)=="logs/motor.csv");assert(append);++writes;
 if(fail_write)return ESP_FAIL;
 contents.append((const char*)data,len);return ESP_OK;
}
esp_err_t msc_read_file(const char*,void* data,size_t len,size_t* actual){
 *actual=std::min(len,contents.size());if(!*actual)return ESP_FAIL;
 memcpy(data,contents.data(),*actual);return ESP_OK;
}
}
struct UsbLogServiceTest {
 static void run(){
 ControlService control;UsbLogService log(control);
 fail_write=true;log.sample_once();assert(!log.header_written_&&log.last_phase_==-1&&contents.empty());
 fail_write=false;log.sample_once();assert(log.header_written_&&log.last_phase_==0);assert(contents.find("angle_raw")!=std::string::npos);
 auto initial=contents;int attempts=writes;log.sample_once();assert(writes==attempts);
 state.phase=workflow::Phase::Running;state.running=true;fail_write=true;
 log.sample_once();assert(log.last_phase_==0&&contents==initial);
 fail_write=false;log.sample_once();assert(log.last_phase_==(int)workflow::Phase::Running);
 state.running=false;state.phase=workflow::Phase::Stopped;log.sample_once();assert(log.last_phase_==(int)workflow::Phase::Stopped);
 auto saved=contents;mounted=false;log.sample_once();assert(!log.header_written_&&log.last_phase_==-1);
 mounted=true;log.sample_once();assert(contents.rfind(saved,0)==0);
 assert(contents.find("timestamp_ms",1)==std::string::npos);
 std::cout<<"USB_LOG_RETRY_APPEND_PASS\n";
 }
};
int main(){UsbLogServiceTest::run();}
