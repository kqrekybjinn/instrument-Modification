#include "phase_position.hpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <cmath>
static std::vector<std::string> fields(const std::string& line){
 std::vector<std::string> out;std::stringstream s(line);std::string v;while(std::getline(s,v,','))out.push_back(v);return out;
}
int main(){
 position_math::PhasePosition tracker;std::ifstream csv("artifacts/diagnostics/20261018-MOTOR.csv");assert(csv);
 std::string line;bool started=false;int previous=0,samples=0,phantoms=0;int64_t expected=0;int64_t origin=0;
 while(std::getline(csv,line)){
  auto f=fields(line);if(f.size()!=15||f[0]=="timestamp_ms")continue;
  int phase=std::stoi(f[10]);if((phase!=1&&phase!=4&&phase!=5)||f[9]!="1")continue;
  int m=std::stoi(f[6]),a=std::stoi(f[11]);int64_t t=std::stoll(f[0])*1000;
  if(!started){tracker.reset(m,a,t);origin=tracker.position();previous=a;started=true;}
  else {int step=a-previous;if(step>16384)step-=32768;if(step<-16384)step+=32768;expected+=step;previous=a;assert(tracker.update(m,a,t,120));}
  assert(tracker.position()-origin==expected);++samples;
  if(phase==1||phase==4){
   auto old=std::stoll(f[8]);if(old-expected==32768)++phantoms;
   if(t==27607000){assert(expected==324274);assert(expected<10*32768);}
  }
 }
 assert(samples>80&&phantoms==6&&tracker.calibrated());
 // Learned mileage phase permits a multi-turn gap; unlike pure angle integration.
 tracker.reset(0,1000,0);
 for(int raw=2000;raw<70000;raw+=1000){int a=raw%32768;int m=raw/32768+(a>=29000);assert(tracker.update(m,a,(raw-1000)*100LL,120));}
 assert(tracker.calibrated());
 assert(tracker.update(10,5000,12000000,120));assert(tracker.position()==10*32768+5000);
 // At the not-yet-localized counter boundary a long gap must not invent a turn.
 auto before=tracker.position();assert(!tracker.update(12,28500,14000000,120));assert(tracker.position()==before);
 assert(tracker.update(12,1000,14500000,120));assert(tracker.position()==12*32768+1000);
 std::cout<<"PHASE_REAL_LOG_REPLAY_PASS "<<samples<<" samples, "<<phantoms<<" phantom turns removed\n";
}
