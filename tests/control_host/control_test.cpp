#include "app_control_service.hpp"
#include <iostream>
#include <cmath>
using workflow::Phase;
namespace mock {int64_t now=1000000;double position=7*32768+19000;int16_t rpm10=0;bool online=true;int writes=0;int queries=0;int brakes=0;int mileage_bias=0;}
int64_t esp_timer_get_time(){return mock::now;}
extern "C" {
esp_err_t motor_driver_init(){return ESP_OK;}
esp_err_t motor_enable(uint8_t){return ESP_OK;}
esp_err_t motor_set_speed_mode(uint8_t){return ESP_OK;}
esp_err_t motor_drive_speed(uint8_t,motor_drive_cmd_t d){mock::rpm10=d.rpm10;mock::writes++;return ESP_OK;}
esp_err_t motor_brake(uint8_t){mock::rpm10=0;mock::writes++;mock::brakes++;return ESP_OK;}
esp_err_t motor_query_mileage(uint8_t,motor_mileage_resp_t *r){
 mock::queries++;if(!mock::online)return ESP_ERR_TIMEOUT;
 auto raw=(int64_t)std::floor(mock::position);auto turns=(int32_t)std::floor((double)raw/32768);
 r->mileage_rotations=turns+mock::mileage_bias;r->position_raw=(uint16_t)(raw-(int64_t)turns*32768);r->fault_code=0;return ESP_OK;
}
}
struct ControlServiceTest {
 static void tick(ControlService &c){mock::now+=50000;mock::position+=mock::rpm10/10.0/60*32768*0.05;if(c.should_poll_position()){c.refresh_position();c.final_sample_pending_=false;}c.service_motion(mock::now);c.publish();}
 static void command(ControlService &c,control_cmd_id_t id,int value=0){c.handle_command({id,(int16_t)value});c.publish();}
 static void settle(ControlService &c){for(int i=0;i<100&&c.state_.phase==Phase::Settling;i++){tick(c);}assert(c.state_.phase!=Phase::Settling);}
 static void pause_resume_cases(){
 for(int mode:{0,1})for(bool returning:{false,true}) {
  ControlService c;c.init();mock::online=true;mock::position=8*32768;
  c.state_.mode=mode;c.state_.target_count=20;c.state_.target_rpm=380;
  c.state_.origin_valid=true;c.state_.feedback_valid=true;
  c.state_.pos_fixed_start=7*32768;c.state_.pos_fixed_now=8*32768;c.state_.pos_fixed_peak=9*32768;c.previous_angle_=0;c.previous_mileage_=8;c.previous_sample_us_=mock::now;
  workflow::set_phase(c.state_,returning?Phase::Returning:Phase::Running);
  c.sent_rpm10_=returning?-3800:3800;mock::rpm10=c.sent_rpm10_;
  int queries=mock::queries,brakes=mock::brakes;mock::online=false;
  command(c,CMD_STATE_TOGGLE);
  assert(c.state_.phase==(returning?Phase::ReturnPaused:Phase::Paused));
  assert(mock::brakes==brakes+1&&mock::queries==queries&&mock::rpm10==0);
  assert(c.state_.origin_valid&&c.state_.pos_fixed_start==7*32768);
  mock::online=true;tick(c); // One final sample after pause, no stationary confirmation.
  command(c,CMD_STATE_TOGGLE);
  assert(c.state_.phase==(returning?Phase::Returning:Phase::Running));
  assert(std::abs(mock::rpm10)==SMOOTH_SLOW_RPM*10);
  assert(c.state_.pos_fixed_start==7*32768&&c.state_.target_count==20);
  for(int i=0;i<5;i++){int previous=std::abs(mock::rpm10);tick(c);assert(std::abs(mock::rpm10)<=previous+60);}
  command(c,CMD_STATE_TOGGLE);command(c,CMD_STOP);
  assert(c.state_.phase==(returning?Phase::ReturnStopped:Phase::Stopped)&&mock::rpm10==0);
 }
}
 static void emergency_cases(){
 for(int mode:{0,1})for(Phase phase:{Phase::Running,Phase::Paused,Phase::Returning,Phase::ReturnPaused,Phase::Settling}) {
  ControlService c;c.init();c.state_.mode=mode;c.state_.origin_valid=true;
  c.state_.pos_fixed_start=0;c.state_.pos_fixed_now=32768;
  workflow::set_phase(c.state_,phase);
  if(phase==Phase::Settling)c.state_.reversing=true;
  c.sent_rpm10_=3800;mock::rpm10=3800;mock::online=false;
  int queries=mock::queries,brakes=mock::brakes,writes=mock::writes;
  const bool reverse=c.state_.reversing;command(c,CMD_STOP);
  assert(mock::brakes==brakes+1&&mock::writes==writes+1&&mock::queries==queries);
  assert(mock::rpm10==0&&c.sent_rpm10_==0);
  assert(c.state_.phase==(reverse?Phase::ReturnStopped:Phase::Stopped));
  assert(c.state_.origin_valid&&workflow::displacement(c.state_)==32768);
 }
 mock::online=true;
}
 static void free_angle_cases(){
 for(bool ccw:{false,true}) {
  ControlService c;c.init();mock::online=true;mock::rpm10=0;mock::mileage_bias=0;mock::position=7*32768+29000;
  if(ccw)command(c,CMD_FORWARD_DIR_TOGGLE);
  const double origin=mock::position;command(c,CMD_STATE_TOGGLE);
  for(int i=0;i<100;i++) {
   mock::mileage_bias=i%3==0?1:i%3==1?-1:0;tick(c);
   const double actual=(mock::position-origin)*(ccw?-1:1);
   assert(std::abs(workflow::displacement(c.state_)-actual)<2);
  }
  command(c,CMD_STOP);mock::mileage_bias=0;
 }
}
 static void count_cases(){
 for(int speed:{50,380})for(int target:{1,5})for(int angle:{0,100,19000,32700})for(bool ccw:{false,true}) {
  ControlService c;c.init();mock::online=true;mock::rpm10=0;mock::position=7*32768+angle;
  command(c,CMD_MODE_SWITCH);command(c,CMD_SET_COUNT,target);command(c,CMD_SET_SPEED,speed);
  if(ccw)command(c,CMD_FORWARD_DIR_TOGGLE);
  const double start=mock::position;command(c,CMD_STATE_TOGGLE);
  assert(c.state_.phase==Phase::Running&&workflow::displacement(c.state_)==0&&mock::rpm10!=0);
  for(int i=0;i<600&&c.state_.phase!=Phase::Complete;i++) {
   mock::mileage_bias=i==0?1:i==1?-1:0;tick(c);
  }
  mock::mileage_bias=0;
  assert(c.state_.phase==Phase::Complete);
  const double actual=(mock::position-start)*(ccw?-1:1)/32768;
  assert(actual>=target&&actual<target+0.5);
 }
}
 static void run(){
 ControlService c;c.init();c.refresh_position();c.publish();
 command(c,CMD_MODE_SWITCH);command(c,CMD_SET_COUNT,10);assert(c.state_.mode==1);
 auto origin=(int64_t)mock::position;command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Running&&c.state_.origin_valid);assert(c.state_.pos_fixed_start==0);(void)origin;
 command(c,CMD_SET_COUNT,99);command(c,CMD_MODE_SWITCH);command(c,CMD_FORWARD_DIR_TOGGLE);assert(c.state_.target_count==10&&c.state_.mode==1&&!c.state_.forward_ccw);
 for(int i=0;i<10;i++){tick(c);}command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Paused&&mock::rpm10==0);settle(c);assert(c.state_.phase==Phase::Paused&&c.state_.origin_valid);
 command(c,CMD_SET_SPEED,200);assert(c.state_.target_rpm==200);command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Running);
 command(c,CMD_STOP);assert(c.state_.phase==Phase::Stopped&&mock::rpm10==0);settle(c);assert(c.state_.phase==Phase::Stopped&&!c.state_.reversing&&c.state_.origin_valid);
 int writes=mock::writes;command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Stopped&&mock::writes==writes);
 command(c,CMD_NEW_RUN);assert(c.state_.phase==Phase::Ready&&!c.state_.origin_valid&&mock::rpm10==0);
 command(c,CMD_SET_COUNT,1);command(c,CMD_SET_SPEED,50);command(c,CMD_STATE_TOGGLE);
 for(int i=0;i<200&&c.state_.phase!=Phase::Complete;i++){tick(c);}
 assert(c.state_.phase==Phase::Complete);assert(workflow::displacement(c.state_)>=32768);assert(mock::rpm10==0);
 command(c,CMD_RETURN);assert(c.state_.phase==Phase::Returning&&mock::rpm10<0);
 for(int i=0;i<5;i++){tick(c);}command(c,CMD_STOP);settle(c);assert(c.state_.phase==Phase::ReturnStopped&&c.state_.origin_valid);
 command(c,CMD_RETURN);assert(c.state_.phase==Phase::Returning);
 for(int i=0;i<200&&c.state_.phase!=Phase::Ready;i++){tick(c);}assert(c.state_.phase==Phase::Ready&&!c.state_.origin_valid);
 command(c,CMD_MODE_SWITCH);command(c,CMD_STATE_TOGGLE);for(int i=0;i<10;i++){tick(c);}
 int previous_writes=mock::writes;
 mock::online=false;for(int i=0;i<75;i++){tick(c);}
 assert(c.state_.phase==Phase::Running&&!c.state_.feedback_valid&&mock::rpm10!=0);
 assert(mock::writes==previous_writes); // No query failure -> zero-speed retransmission loop.
 mock::online=true;tick(c);assert(c.state_.phase==Phase::Running&&c.state_.feedback_valid);
 command(c,CMD_STOP);command(c,CMD_STOP);assert(mock::rpm10==0);settle(c);
 command(c,CMD_NEW_RUN);command(c,CMD_FORWARD_DIR_TOGGLE);assert(c.state_.forward_ccw);command(c,CMD_STATE_TOGGLE);for(int i=0;i<10;i++){tick(c);}assert(workflow::displacement(c.state_)>0);
 command(c,CMD_STOP);settle(c);tick(c); // Consume one final sample.
 int idle_queries=mock::queries, idle_writes=mock::writes;
 for(int i=0;i<20;i++){tick(c);}
 assert(mock::queries==idle_queries&&mock::writes==idle_writes);
 mock::online=false;command(c,CMD_NEW_RUN);assert(c.state_.phase==Phase::Ready);
 command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Ready); // Still requires one valid origin.
 mock::online=true;command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Running);
 for(int i=0;i<16;i++){c.push_cmd(CMD_SET_SPEED,100);}c.push_cmd(CMD_STOP);control_cmd_t front{};assert(xQueueReceive(c.control_q_,&front,0)==pdTRUE&&front.id==CMD_STOP);
 std::cout<<"CONTROL_INTEGRATION_PASS\n";
 }
};
int main(){ControlServiceTest::free_angle_cases();ControlServiceTest::pause_resume_cases();ControlServiceTest::emergency_cases();ControlServiceTest::count_cases();ControlServiceTest::run();}
