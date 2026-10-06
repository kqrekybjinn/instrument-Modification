#include "app_control_service.hpp"
#include <iostream>
#include <cmath>
using workflow::Phase;
namespace mock {int64_t now=1000000;double position=7*32768+19000;int16_t rpm10=0;bool online=true;int writes=0;int queries=0;}
int64_t esp_timer_get_time(){return mock::now;}
extern "C" {
esp_err_t motor_driver_init(){return ESP_OK;}
esp_err_t motor_enable(uint8_t){return ESP_OK;}
esp_err_t motor_set_speed_mode(uint8_t){return ESP_OK;}
esp_err_t motor_drive_speed(uint8_t,motor_drive_cmd_t d){mock::rpm10=d.rpm10;mock::writes++;return ESP_OK;}
esp_err_t motor_query_mileage(uint8_t,motor_mileage_resp_t *r){
 mock::queries++;if(!mock::online)return ESP_ERR_TIMEOUT;
 auto raw=(int64_t)std::floor(mock::position);auto turns=(int32_t)std::floor((double)raw/32768);
 r->mileage_rotations=turns;r->position_raw=(uint16_t)(raw-(int64_t)turns*32768);r->fault_code=0;return ESP_OK;
}
}
struct ControlServiceTest {
 static void tick(ControlService &c){mock::now+=50000;mock::position+=mock::rpm10/10.0/60*32768*0.05;if(c.should_poll_position()){c.refresh_position();c.final_sample_pending_=false;}c.service_motion(mock::now);c.publish();}
 static void command(ControlService &c,control_cmd_id_t id,int value=0){c.handle_command({id,(int16_t)value});c.publish();}
 static void settle(ControlService &c){for(int i=0;i<100&&c.state_.phase==Phase::Settling;i++){tick(c);}assert(c.state_.phase!=Phase::Settling);}
 static void count_cases(){
 for(int target:{1,5})for(int angle:{0,100,19000,32700})for(bool ccw:{false,true}) {
  ControlService c;c.init();mock::online=true;mock::rpm10=0;mock::position=7*32768+angle;
  command(c,CMD_MODE_SWITCH);command(c,CMD_SET_COUNT,target);command(c,CMD_SET_SPEED,50);
  if(ccw)command(c,CMD_FORWARD_DIR_TOGGLE);
  const double start=mock::position;command(c,CMD_STATE_TOGGLE);
  assert(c.state_.phase==Phase::Running&&workflow::displacement(c.state_)==0&&mock::rpm10!=0);
  for(int i=0;i<600&&c.state_.phase!=Phase::Complete;i++)tick(c);
  assert(c.state_.phase==Phase::Complete);
  const double actual=(mock::position-start)*(ccw?-1:1)/32768;
  assert(actual>=target&&actual<target+0.5);
 }
}
 static void run(){
 ControlService c;c.init();c.refresh_position();c.publish();
 command(c,CMD_MODE_SWITCH);command(c,CMD_SET_COUNT,10);assert(c.state_.mode==1);
 auto origin=(int64_t)mock::position;command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Running&&c.state_.origin_valid);assert(c.state_.pos_fixed_start==origin);
 command(c,CMD_SET_COUNT,99);command(c,CMD_MODE_SWITCH);command(c,CMD_FORWARD_DIR_TOGGLE);assert(c.state_.target_count==10&&c.state_.mode==1&&!c.state_.forward_ccw);
 for(int i=0;i<10;i++){tick(c);}command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Settling);settle(c);assert(c.state_.phase==Phase::Paused&&c.state_.origin_valid);
 command(c,CMD_SET_SPEED,200);assert(c.state_.target_rpm==200);command(c,CMD_STATE_TOGGLE);assert(c.state_.phase==Phase::Running);
 command(c,CMD_STOP);assert(c.state_.phase==Phase::Settling);settle(c);assert(c.state_.phase==Phase::Stopped&&!c.state_.reversing&&c.state_.origin_valid);
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
int main(){ControlServiceTest::count_cases();ControlServiceTest::run();}
