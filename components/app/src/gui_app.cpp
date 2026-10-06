#include "gui_app.h"
#include "workflow.hpp"
#include <stdio.h>
#include <string.h>
#include <initializer_list>
LV_FONT_DECLARE(lv_font_modi_16_cjk);
using workflow::Phase;
static SemaphoreHandle_t lock_;
static gui_event_cb_t callback_;
static gui_view_t view_{};
static bool built_;
static lv_obj_t *boot_label_, *boot_bar_, *mode_, *direction_, *feedback_, *usb_, *tf_;
static lv_obj_t *state_label_, *count_label_, *target_label_, *hint_, *progress_, *speed_, *target_;
static lv_obj_t *minus_, *plus_, *step_, *primary_, *secondary_, *third_, *key_hint_;
static lv_obj_t *modal_, *entry_, *error_;
static bool target_modal_;
static const auto bg = lv_color_hex(0x101B27), panel = lv_color_hex(0x1C2A38);
static const auto white = lv_color_hex(0xF1F5F9), muted = lv_color_hex(0xA9BBCB), border = lv_color_hex(0x35495B);
static const auto blue = lv_color_hex(0x2878C8), green = lv_color_hex(0x16866A), red = lv_color_hex(0xB93D45), amber = lv_color_hex(0xE8B459);
static bool take() { return !lock_ || xSemaphoreTakeRecursive(lock_, pdMS_TO_TICKS(50)) == pdTRUE; }
static void give() { if (lock_) xSemaphoreGiveRecursive(lock_); }
static void enable(lv_obj_t *o, bool value) { if(value) lv_obj_clear_state(o,LV_STATE_DISABLED); else lv_obj_add_state(o,LV_STATE_DISABLED); }
static void label(lv_obj_t *o, const char *text) { lv_label_set_text(lv_obj_get_child(o,0),text); }
static void emit(gui_event_t event,int16_t value=0) { if(callback_) callback_(event,value); }
static void place(lv_obj_t *o,int x,int y,int w,int h) { lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE); if(lv_obj_check_type(o,&lv_btn_class)&&lv_obj_get_child_cnt(o)>0){auto l=lv_obj_get_child(o,0);lv_obj_set_width(l,w-8);lv_obj_center(l);} }
static lv_obj_t *text(lv_obj_t *parent,const char *value,int x,int y,int w,const lv_font_t *font=&lv_font_modi_16_cjk,lv_color_t color=white) {
 auto o=lv_label_create(parent); lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);lv_label_set_text(o,value);
 lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,color,0);return o;
}
static void direct(lv_event_t *event) {
 uintptr_t payload=(uintptr_t)lv_event_get_user_data(event);
 emit((gui_event_t)(payload&255),(int16_t)(payload>>8));
}
static lv_obj_t *button(lv_obj_t *parent,const char *value,int x,int y,int w,int h,lv_color_t color,lv_event_cb_t cb,uintptr_t payload=0) {
 auto o=lv_btn_create(parent);place(o,x,y,w,h);
 lv_obj_set_style_radius(o,7,0);lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_shadow_width(o,0,0);
 lv_obj_set_style_bg_color(o,color,0);lv_obj_set_style_bg_opa(o,LV_OPA_100,0);lv_obj_set_style_bg_opa(o,LV_OPA_40,LV_STATE_DISABLED);
 lv_obj_set_style_pad_all(o,4,0);lv_obj_set_style_bg_opa(o,LV_OPA_70,LV_STATE_PRESSED);
 auto l=lv_label_create(o);lv_label_set_text(l,value);lv_obj_set_width(l,w-8);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_set_style_text_font(l,&lv_font_modi_16_cjk,0);lv_obj_set_style_text_color(l,white,0);lv_obj_center(l);
 lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void*)payload);return o;
}
static lv_obj_t *card(lv_obj_t *parent,int x,int y,int w,int h) {
 auto o=lv_obj_create(parent);place(o,x,y,w,h);lv_obj_set_style_pad_all(o,0,0);
 lv_obj_set_style_bg_color(o,panel,0);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,border,0);lv_obj_set_style_radius(o,9,0);return o;
}
static void close_modal() { if(modal_) {lv_obj_del(modal_);modal_=nullptr;entry_=nullptr;error_=nullptr;} }
static void cancel_modal(lv_event_t*) {close_modal();}
static void modal_stop(lv_event_t*) {emit(GUI_EVENT_STOP);close_modal();}
static void pick_speed(lv_event_t *event) { emit(GUI_EVENT_SET_SPEED,(int16_t)(uintptr_t)lv_event_get_user_data(event));close_modal(); }
static void show_speed(lv_event_t*) {
 close_modal();target_modal_=false;
 modal_=lv_obj_create(lv_scr_act());place(modal_,0,0,lv_disp_get_hor_res(nullptr),lv_disp_get_ver_res(nullptr));
 lv_obj_set_style_bg_color(modal_,bg,0);lv_obj_set_style_bg_opa(modal_,LV_OPA_100,0);lv_obj_set_style_pad_all(modal_,0,0);lv_obj_set_style_border_width(modal_,0,0);
 text(modal_,"选择设定转速",18,16,300);
 const int values[]={50,100,200,300,380};
 for(int i=0;i<5;i++) {char b[32];snprintf(b,sizeof(b),"%d RPM%s",values[i],values[i]==view_.rpm?" · 当前":"");
  button(modal_,b,18+(i%3)*148,58+(i/3)*66,136,54,values[i]==view_.rpm?blue:panel,pick_speed,values[i]);}
 text(modal_,"运行及暂停时均可调整，平滑切换",18,210,440,&lv_font_modi_16_cjk,muted);
 button(modal_,"返回",18,258,292,44,border,cancel_modal);
 auto stop=button(modal_,"停止",320,258,142,44,red,modal_stop);
 enable(stop,view_.phase!=(uint8_t)Phase::Ready);
}
static void input_key(lv_event_t *event) {
 char key=(char)(uintptr_t)lv_event_get_user_data(event);
 const char *current=lv_textarea_get_text(entry_);
 if(key=='<') lv_textarea_del_char(entry_);
 else if(key=='C') lv_textarea_set_text(entry_,"");
 else if(strlen(current)<4) {char b[]={key,0};lv_textarea_add_text(entry_,b);}
 if(error_) lv_label_set_text(error_,"");
}
static void apply_count(lv_event_t*) {
 const char *value=lv_textarea_get_text(entry_);int count=0;
 for(const char *p=value;*p;p++) count=count*10+(*p-'0');
 if(count<1||count>9999) {lv_label_set_text(error_,"请输入 1-9999 圈");return;}
 emit(GUI_EVENT_SET_COUNT,count);close_modal();
}
static void show_target(lv_event_t*) {
 if(view_.phase==(uint8_t)Phase::FeedbackLost && view_.feedback_valid && view_.origin_valid && view_.displacement>0){emit(GUI_EVENT_RETURN);return;}
 if(view_.phase!=(uint8_t)Phase::Ready || view_.origin_valid)return;
 close_modal();target_modal_=true;
 modal_=lv_obj_create(lv_scr_act());place(modal_,0,0,480,320);lv_obj_set_style_pad_all(modal_,0,0);
 lv_obj_set_style_bg_color(modal_,bg,0);lv_obj_set_style_border_width(modal_,0,0);
 text(modal_,"目标圈数",16,14,210);text(modal_,"整数 · 1-9999 圈",16,43,220,&lv_font_modi_16_cjk,muted);
 entry_=lv_textarea_create(modal_);place(entry_,16,78,196,54);lv_textarea_set_one_line(entry_,true);
 lv_textarea_set_max_length(entry_,4);lv_textarea_set_accepted_chars(entry_,"0123456789");
 lv_obj_set_style_text_font(entry_,&lv_font_montserrat_20,0);char b[12];snprintf(b,sizeof(b),"%d",view_.target);lv_textarea_set_text(entry_,b);
 error_=text(modal_,"",16,146,214,&lv_font_modi_16_cjk,amber);
 button(modal_,"取消",16,248,92,54,border,cancel_modal);
 button(modal_,"确定",120,248,92,54,green,apply_count);
 const char keys[]="123456789C0<";
 for(int i=0;i<12;i++) {char b[8]={keys[i],0};if(keys[i]=='C')strcpy(b,"清空");if(keys[i]=='<')strcpy(b,"删除");
  button(modal_,b,238+(i%3)*76,14+(i/3)*74,66,64,panel,input_key,keys[i]);}
}
static void primary_click(lv_event_t*) {
 const auto p=(Phase)view_.phase;
 emit(p==Phase::Stopped||p==Phase::Complete||p==Phase::ReturnStopped||p==Phase::OriginLost?GUI_EVENT_NEW_RUN:GUI_EVENT_STATE_TOGGLE);
}
static void secondary_click(lv_event_t*) {
 const auto p=(Phase)view_.phase;
 emit(p==Phase::Stopped||p==Phase::Complete||p==Phase::ReturnStopped?GUI_EVENT_RETURN:GUI_EVENT_STOP);
}
static void third_click(lv_event_t*) {emit(GUI_EVENT_NEW_RUN);}
static void build() {
 lv_obj_t *screen=lv_scr_act();lv_obj_clean(screen);lv_obj_set_style_bg_color(screen,bg,0);lv_obj_set_style_pad_all(screen,0,0);lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 text(screen,"MODI",12,12,80,&lv_font_montserrat_20);
 feedback_=text(screen,"等待反馈",92,13,166,&lv_font_modi_16_cjk,muted);
 mode_=button(screen,"自由模式",272,7,94,32,border,direct,GUI_EVENT_MODE_SWITCH);
 direction_=button(screen,"正转顺时针",374,7,94,32,border,direct,GUI_EVENT_FORWARD_DIR_TOGGLE);
 auto left=card(screen,12,48,252,212);
 state_label_=text(left,"准备运行",14,12,224);
 count_label_=text(left,"0.00",14,48,224,&lv_font_montserrat_48);
 target_label_=text(left,"实际圈数",14,109,224,&lv_font_modi_16_cjk,muted);
 progress_=lv_bar_create(left);place(progress_,14,139,224,5);lv_bar_set_range(progress_,0,1000);
 lv_obj_set_style_bg_color(progress_,border,LV_PART_MAIN);lv_obj_set_style_bg_color(progress_,blue,LV_PART_INDICATOR);
 hint_=text(left,"设置参数后按启动",14,150,224,&lv_font_modi_16_cjk,muted);
 speed_=button(screen,"设定转速\n100 RPM",272,48,196,66,panel,show_speed);
 target_=button(screen,"目标圈数\n100 圈",272,122,196,66,panel,show_target);
 minus_=button(screen,"-",272,196,58,40,border,direct,GUI_EVENT_COUNT_DEC);
 step_=button(screen,"±10",337,196,66,40,border,direct,GUI_EVENT_COUNT_STEP_TOGGLE);
 plus_=button(screen,"+",410,196,58,40,border,direct,GUI_EVENT_COUNT_INC);
 key_hint_=text(screen,"按键1 启动/暂停  ·  按键2 停止",272,240,196,&lv_font_modi_16_cjk,muted);
 lv_obj_set_style_text_font(key_hint_,&lv_font_modi_16_cjk,0);
 primary_=button(screen,"启动",12,268,252,40,green,primary_click);
 secondary_=button(screen,"停止",272,268,196,40,red,secondary_click);
 third_=button(screen,"新运行",368,268,100,40,border,third_click);lv_obj_add_flag(third_,LV_OBJ_FLAG_HIDDEN);
 usb_=text(screen,"USB --",12,311,130,&lv_font_montserrat_14,muted);
 tf_=text(screen,"TF --",140,311,130,&lv_font_montserrat_14,muted);
 // Storage indicators share a compact line in the main card to keep all controls inside 320px.
 lv_obj_set_pos(usb_,26,238);lv_obj_set_pos(tf_,146,238);
 built_=true;
}
esp_err_t gui_app_init(SemaphoreHandle_t lock,gui_event_cb_t cb) {
 lock_=lock;callback_=cb;built_=false;
 if(!take())return ESP_FAIL;
 auto screen=lv_scr_act();lv_obj_clean(screen);lv_obj_set_style_bg_color(screen,bg,0);lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
 text(screen,"MODI",32,70,400,&lv_font_montserrat_48);
 boot_label_=text(screen,"电机初始化中",32,142,400);
 boot_bar_=lv_bar_create(screen);place(boot_bar_,32,188,416,6);lv_bar_set_range(boot_bar_,0,100);give();return ESP_OK;
}
void gui_app_update_boot(uint8_t p) {if(!take())return;if(!built_&&boot_bar_)lv_bar_set_value(boot_bar_,p,LV_ANIM_OFF);give();}
void gui_app_enter_main_ui() {if(!take())return;boot_label_=nullptr;boot_bar_=nullptr;build();give();}
void gui_app_render(const gui_view_t *v) {
 if(!v||!take())return;
 view_=*v;if(!built_){give();return;}
 const auto p=(Phase)v->phase;const bool ready=p==Phase::Ready&&!v->origin_valid;
 const bool result=p==Phase::Stopped||p==Phase::Complete||p==Phase::ReturnStopped;
 const bool lost=p==Phase::FeedbackLost;const bool moving=workflow::motion(p);
 if(modal_&&((target_modal_&&!ready)||p==Phase::Settling||p==Phase::OriginLost))close_modal();
 label(mode_,v->mode?"定圈模式":"自由模式");label(direction_,v->forward_ccw?"正转逆时针":"正转顺时针");
 enable(mode_,ready);enable(direction_,ready);
 lv_label_set_text(feedback_,p==Phase::OriginLost?"起点失效":v->motor_fault?"电机故障":v->feedback_valid?"反馈正常":"反馈未确认");
 lv_obj_set_style_text_color(feedback_,v->feedback_valid?lv_color_hex(0x69D7B5):amber,0);
 const char *status="准备运行",*help="设置参数后按启动";lv_color_t color=white;
 switch(p) {
 case Phase::Running:status="正转运行";help="暂停可继续，停止结束任务";color=lv_color_hex(0x69D7B5);break;
 case Phase::Paused:status="已暂停";help="继续剩余行程，或停止结束";color=amber;break;
 case Phase::Stopped:status="本次已停止";help="保留起点，可新运行或回位";break;
 case Phase::Complete:
  if(v->mode && (v->displacement-(int64_t)v->target*32768>16384 || v->displacement-(int64_t)v->target*32768 < -16384)) {
   status="已停止 · 超出容差";help="误差超过半圈，请检查运行结果";color=amber;
  }else {status="本次已完成";help="保留实际圈数，可新运行或回位";color=lv_color_hex(0x69D7B5);}
  break;
 case Phase::Returning:status="反转回位";help="圈数回到零；可暂停或停止";color=amber;break;
 case Phase::ReturnPaused:status="回位已暂停";help="继续回位，或停止回位";color=amber;break;
 case Phase::ReturnStopped:status="回位未完成";help="继续回位，或从此处新运行";color=amber;break;
 case Phase::FeedbackLost:status=v->feedback_valid?"反馈已恢复":"反馈失效";help=v->feedback_valid?"手动继续、停止或选择新运行":"已请求停止，实际状态未确认";color=amber;break;
 case Phase::Settling:status="正在停止";help="读取最终位置，请稍候";color=amber;break;
 case Phase::OriginLost:status="本次起点失效";help="位置发生跳变，需重新建立任务";color=amber;break;
 default:break;
 }
 lv_label_set_text(state_label_,status);lv_obj_set_style_text_color(state_label_,color,0);lv_label_set_text(hint_,help);
 int64_t h=v->displacement*100/32768,m=h<0?-h:h;char b[96];
 snprintf(b,sizeof(b),"%s%lld.%02lld",h<0?"-":"",(long long)(m/100),(long long)(m%100));lv_label_set_text(count_label_,p==Phase::OriginLost?"--":b);
 snprintf(b,sizeof(b),v->mode?"实际圈数 / 目标 %d 圈":"实际圈数",v->target);lv_label_set_text(target_label_,p==Phase::OriginLost?"原起点不可用于计数或回位":!v->feedback_valid&&v->origin_valid?"上次有效圈数 · 等待确认":v->reversing?"距本次起点 · 圈":b);
 if(v->mode){lv_obj_clear_flag(progress_,LV_OBJ_FLAG_HIDDEN);int64_t value=v->target>0?v->displacement*1000/(v->target*32768LL):0;lv_bar_set_value(progress_,value<0?0:value>1000?1000:value,LV_ANIM_OFF);}else lv_obj_add_flag(progress_,LV_OBJ_FLAG_HIDDEN);
 snprintf(b,sizeof(b),"设定转速\n%d RPM · 点击选择",v->rpm);label(speed_,b);enable(speed_,p!=Phase::Settling);
 snprintf(b,sizeof(b),v->mode?"目标圈数\n%d 圈 · 点击输入":"自由运行\n无目标圈数",v->target);if(v->mode&&!ready){snprintf(b,sizeof(b),"目标圈数\n%d 圈 · 本次已锁定",v->target);}
 label(target_,b);enable(target_,ready&&v->mode);
 if(lost&&v->feedback_valid){snprintf(b,sizeof(b),"回本次起点\n%s · %d RPM",v->forward_ccw?"顺时针":"逆时针",v->rpm);label(target_,b);enable(target_,v->origin_valid&&v->displacement>0);}
 snprintf(b,sizeof(b),"±%d",v->step);label(step_,b);
 for(auto o:{minus_,step_,plus_}) {enable(o,ready&&v->mode);if(v->mode)lv_obj_clear_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);}
 label(primary_,result||p==Phase::OriginLost?"新运行":moving?"暂停":p==Phase::ReturnPaused?"继续回位":p==Phase::Paused||lost?"继续":"启动");
 lv_obj_set_style_bg_color(primary_,moving?blue:green,0);
 enable(primary_,(result&&v->feedback_valid)||(ready&&v->feedback_valid)||moving||((p==Phase::Paused||p==Phase::ReturnPaused||lost)&&v->feedback_valid)||(p==Phase::OriginLost&&v->feedback_valid));
 label(secondary_,"停止");lv_obj_set_style_bg_color(secondary_,red,0);
 if(result) {
  snprintf(b,sizeof(b),"%s\n%s · %d RPM",p==Phase::ReturnStopped?"继续回位":"回本次起点",v->forward_ccw?"顺时针":"逆时针",v->rpm);
  label(secondary_,b);lv_obj_set_style_bg_color(secondary_,blue,0);enable(secondary_,v->origin_valid&&v->feedback_valid&&v->displacement>0);
 }else enable(secondary_,!ready&&p!=Phase::OriginLost);
 lv_obj_add_flag(third_,LV_OBJ_FLAG_HIDDEN);place(primary_,12,268,252,40);place(secondary_,272,268,196,40);
 if(lost&&v->feedback_valid){place(primary_,12,268,144,40);place(secondary_,164,268,144,40);place(third_,316,268,152,40);lv_obj_clear_flag(third_,LV_OBJ_FLAG_HIDDEN);}
 // Hide the physical-key reminder in narrow space; labels stay readable at 16px.
 lv_label_set_text(key_hint_,result?"请选择下一步操作":moving?"键1 暂停  键2 停止":p==Phase::Paused||p==Phase::ReturnPaused||lost?"键1 继续  键2 停止":"键1 启动  键2 停止");
 give();
}
void gui_app_update_usb(bool mounted) {if(!take())return;if(built_){lv_label_set_text(usb_,mounted?"USB OK":"USB --");lv_obj_set_style_text_color(usb_,mounted?lv_color_hex(0x69D7B5):muted,0);}give();}
void gui_app_update_tf(bool mounted) {if(!take())return;if(built_){lv_label_set_text(tf_,mounted?"TF OK":"TF --");lv_obj_set_style_text_color(tf_,mounted?lv_color_hex(0x69D7B5):muted,0);}give();}
