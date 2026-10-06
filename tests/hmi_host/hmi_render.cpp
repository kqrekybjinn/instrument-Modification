#include "gui_app.h"
#include "workflow.hpp"
#include <vector>
#include <fstream>
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <set>
static std::vector<lv_color_t> frame(480*320);
static int received=-1,value_=-1;
static lv_point_t pointer_point{};
static bool pointer_down=false;
static void pointer_read(lv_indev_drv_t*,lv_indev_data_t* d){d->point=pointer_point;d->state=pointer_down?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;}
static void pointer_step(){lv_tick_inc(35);lv_timer_handler();}
static lv_obj_t *find(lv_obj_t*,const char*);
static void touch(const char* name,gui_view_t& v){
 auto l=find(lv_scr_act(),name);assert(l);auto b=lv_obj_get_parent(l);
 lv_obj_update_layout(lv_scr_act());lv_area_t a;lv_obj_get_coords(b,&a);
 pointer_point={(lv_coord_t)((a.x1+a.x2)/2),(lv_coord_t)((a.y1+a.y2)/2)};
 received=-1;pointer_down=true;pointer_step();assert(received>=0);
 for(int i=0;i<4;i++){gui_app_render(&v);pointer_step();}
 pointer_down=false;pointer_step();assert(received>=0);
}

static void command(gui_event_t e,int16_t v){received=e;value_=v;}
static void flush(lv_disp_drv_t *d,const lv_area_t *area,lv_color_t *pixels){
 for(int y=area->y1;y<=area->y2;y++)for(int x=area->x1;x<=area->x2;x++)frame[y*480+x]=*pixels++;
 lv_disp_flush_ready(d);
}
static void tick(){lv_obj_update_layout(lv_scr_act());lv_tick_inc(50);lv_refr_now(nullptr);}
static void save(const char *name){tick();std::ofstream f(std::string("artifacts/hmi/")+name+".ppm",std::ios::binary);f<<"P6\n480 320\n255\n";
 for(auto c:frame){lv_color32_t rgb;rgb.full=lv_color_to32(c);char b[3]={(char)rgb.ch.red,(char)rgb.ch.green,(char)rgb.ch.blue};f.write(b,3);}}
static lv_obj_t *find(lv_obj_t *root,const char *needle){
 for(unsigned i=0;i<lv_obj_get_child_cnt(root);i++){auto c=lv_obj_get_child(root,i);
 if(lv_obj_check_type(c,&lv_label_class)&&strcmp(lv_label_get_text(c),needle)==0)return c;
 if(auto r=find(c,needle))return r;}return nullptr;
}
static void click(const char *label){auto o=find(lv_scr_act(),label);assert(o);auto b=lv_obj_get_parent(o);assert(!lv_obj_has_state(b,LV_STATE_DISABLED));received=-1;const bool immediate=strcmp(label,"启动")==0||strcmp(label,"暂停")==0||strcmp(label,"停止")==0||strcmp(label,"继续")==0||strcmp(label,"新运行")==0||strcmp(label,"继续回位")==0||strstr(label,"回本次起点")||strstr(label,"继续回位\n");lv_event_send(b,immediate?LV_EVENT_PRESSED:LV_EVENT_CLICKED,nullptr);tick();}
static bool disabled(const char *label){auto o=find(lv_scr_act(),label);assert(o);return lv_obj_has_state(lv_obj_get_parent(o),LV_STATE_DISABLED);}
static void audit(lv_obj_t *o){
 if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
 auto a=o->coords;
 if(a.x1<0||a.y1<0||a.x2>=480||a.y2>=320){std::cerr<<"OUTSIDE "<<a.x1<<","<<a.y1<<" "<<a.x2<<","<<a.y2<<"\n";abort();}
 if(lv_obj_check_type(o,&lv_label_class)) {
  const auto *font=lv_obj_get_style_text_font(o,0);auto str=lv_label_get_text(o);
  uint32_t i=0;while(str[i]){uint32_t cp=_lv_txt_encoded_next(str,&i);if(cp=='\n'||cp=='\r')continue;lv_font_glyph_dsc_t d{};
  if(!lv_font_get_glyph_dsc(font,&d,cp,0)){std::cerr<<"MISSING GLYPH "<<cp<<" "<<str<<"\n";abort();}}
 }
 for(unsigned i=0;i<lv_obj_get_child_cnt(o);i++)audit(lv_obj_get_child(o,i));
}
static void render(gui_view_t &v,const char *name){gui_app_render(&v);tick();audit(lv_scr_act());save(name);}
int main(){
 std::filesystem::create_directories("artifacts/hmi");lv_init();static lv_color_t draw[480*20];static lv_disp_draw_buf_t buf;lv_disp_draw_buf_init(&buf,draw,nullptr,480*20);
 static lv_disp_drv_t driver;lv_disp_drv_init(&driver);driver.hor_res=480;driver.ver_res=320;driver.draw_buf=&buf;driver.flush_cb=flush;lv_disp_drv_register(&driver);
 static lv_indev_drv_t input;lv_indev_drv_init(&input);input.type=LV_INDEV_TYPE_POINTER;input.read_cb=pointer_read;lv_indev_drv_register(&input);
 gui_app_init(nullptr,command);save("boot");gui_app_enter_main_ui();gui_app_update_usb(true);gui_app_update_tf(false);
 gui_view_t v{(uint8_t)workflow::Phase::Ready,1,true,false,false,false,100,10,10,0,0,0,0};
 v.motor_turns=7;v.motor_angle=16384;render(v,"ready");assert(find(lv_scr_act(),"7.50"));assert(find(lv_scr_act(),"7 rev / 180.00 deg"));touch("启动",v);assert(received==GUI_EVENT_STATE_TOGGLE);click("启动");assert(received==GUI_EVENT_STATE_TOGGLE);assert(disabled("停止"));
 click("设定转速\n100 RPM · 点击选择");audit(lv_scr_act());save("speed");click("300 RPM");assert(received==GUI_EVENT_SET_SPEED&&value_==300);
 click("目标圈数\n10 圈 · 点击输入");audit(lv_scr_act());save("target");click("清空");click("1");click("2");click("确定");assert(received==GUI_EVENT_SET_COUNT&&value_==12);
 v.phase=(uint8_t)workflow::Phase::Running;v.origin_valid=true;v.displacement=106496;render(v,"running");touch("暂停",v);assert(received==GUI_EVENT_STATE_TOGGLE);touch("停止",v);assert(received==GUI_EVENT_STOP);click("暂停");assert(received==GUI_EVENT_STATE_TOGGLE);click("停止");assert(received==GUI_EVENT_STOP);click("设定转速\n100 RPM · 点击选择");click("停止");assert(received==GUI_EVENT_STOP);assert(disabled("定圈模式"));assert(disabled("正转顺时针"));
 // Parameter modal is closed if a physical key starts motion.
 v.phase=(uint8_t)workflow::Phase::Ready;v.origin_valid=false;render(v,"ready_again");click("目标圈数\n10 圈 · 点击输入");v.phase=(uint8_t)workflow::Phase::Running;v.origin_valid=true;gui_app_render(&v);assert(!find(lv_scr_act(),"确定"));
 v.phase=(uint8_t)workflow::Phase::Paused;render(v,"paused");click("继续");assert(received==GUI_EVENT_STATE_TOGGLE);
 v.phase=(uint8_t)workflow::Phase::Complete;v.displacement=333578;render(v,"complete");click("新运行");assert(received==GUI_EVENT_NEW_RUN);click("回本次起点\n逆时针 · 100 RPM");assert(received==GUI_EVENT_RETURN);
 v.phase=(uint8_t)workflow::Phase::Returning;v.reversing=true;v.displacement=122880;render(v,"returning");click("停止");assert(received==GUI_EVENT_STOP);
 v.phase=(uint8_t)workflow::Phase::ReturnStopped;render(v,"return_stopped");click("继续回位\n逆时针 · 100 RPM");assert(received==GUI_EVENT_RETURN);
 v.phase=(uint8_t)workflow::Phase::Stopped;v.feedback_valid=false;render(v,"stopped_without_confirmation");click("新运行");assert(received==GUI_EVENT_NEW_RUN);click("回本次起点\n逆时针 · 100 RPM");assert(received==GUI_EVENT_RETURN);
 v.phase=(uint8_t)workflow::Phase::Ready;v.origin_valid=false;v.mode=0;v.displacement=0;render(v,"free_ready");
 v.phase=(uint8_t)workflow::Phase::Running;v.origin_valid=true;v.displacement=123*32768;render(v,"free_running");click("暂停");assert(received==GUI_EVENT_STATE_TOGGLE);
 v.mode=1;v.target=9999;v.displacement=9999LL*32768+16384;render(v,"maximum_count");
 v.phase=(uint8_t)workflow::Phase::Complete;v.target=10;v.displacement=11*32768;render(v,"outside_tolerance");assert(find(lv_scr_act(),"已停止 · 超出容差"));

 std::cout<<"HMI_RENDER_AND_INTERACTION_PASS\n";
}
