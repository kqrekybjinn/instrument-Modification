#include "workflow.hpp"
#include <cassert>
#include <iostream>
using namespace workflow;
int main(){
 State s;assert(s.phase==Phase::Ready);assert(parameters_unlocked(s));
 assert(transition(s,Action::Toggle)==Phase::Ready);s.feedback_valid=true;
 assert(transition(s,Action::Toggle)==Phase::Running);
 s.origin_valid=true;s.pos_fixed_start=position_math::oriented_position(5,23000,false);s.pos_fixed_now=s.pos_fixed_start+32768*3+8192;
 set_phase(s,Phase::Running);assert(!parameters_unlocked(s));assert(displacement(s)==106496);
 assert(transition(s,Action::Toggle)==Phase::Paused);assert(transition(s,Action::Stop)==Phase::Stopped); // STOP never requests reverse.
 set_phase(s,Phase::Paused);assert(transition(s,Action::Toggle)==Phase::Running);
 set_phase(s,Phase::Stopped);assert(transition(s,Action::Toggle)==Phase::Stopped);assert(can_return(s));assert(transition(s,Action::Return)==Phase::Returning);
 set_phase(s,Phase::Returning);assert(transition(s,Action::Toggle)==Phase::ReturnPaused);assert(transition(s,Action::Stop)==Phase::ReturnStopped);
 set_phase(s,Phase::ReturnStopped);assert(s.origin_valid);assert(transition(s,Action::Return)==Phase::Returning);
 assert(transition(s,Action::NewRun)==Phase::Ready);prepare_new(s);assert(!s.origin_valid);assert(s.pos_fixed_start==INT64_MIN);assert(!s.running);assert(parameters_unlocked(s));
 s.origin_valid=true;s.pos_fixed_start=10000;s.pos_fixed_now=10000+11*32768;s.target_count=10;s.mode=1;
 set_phase(s,Phase::Complete);assert(transition(s,Action::Toggle)==Phase::Complete);assert(displacement(s)==11*32768);assert(can_return(s));
 set_phase(s,Phase::FeedbackLost);s.feedback_valid=false;assert(transition(s,Action::Toggle)==Phase::FeedbackLost);assert(transition(s,Action::NewRun)==Phase::FeedbackLost);assert(!can_return(s));
 s.feedback_valid=true;assert(transition(s,Action::Toggle)==Phase::Complete);s.pos_fixed_now=s.pos_fixed_start+3*32768;assert(transition(s,Action::Toggle)==Phase::Running);
 s.reversing=true;assert(transition(s,Action::Toggle)==Phase::Returning);assert(transition(s,Action::Stop)==Phase::ReturnStopped);
 s.pos_fixed_now=s.pos_fixed_start;assert(transition(s,Action::Toggle)==Phase::Ready);assert(!can_return(s));
 set_phase(s,Phase::Settling);assert(transition(s,Action::NewRun)==Phase::Settling);assert(transition(s,Action::Toggle)==Phase::Settling);
 assert(valid_speed(50)&&valid_speed(380)&&!valid_speed(0)&&!valid_speed(120));
 // Negative commanded direction gives positive forward progress and decreasing return displacement.
 auto origin=position_math::oriented_position(7,30000,true);auto forward=position_math::oriented_position(-3,30000,true);
 assert(forward-origin==10*32768);assert(position_math::oriented_position(7,30000,true)-origin==0);
 std::cout<<"WORKFLOW_TEST_PASS\n";
}
