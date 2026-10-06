#pragma once
struct gui_view_t{uint8_t phase,mode;bool feedback_valid,origin_valid,reversing,forward_ccw;int16_t rpm,target,step;int64_t displacement;uint8_t motor_fault; int32_t motor_turns; uint16_t motor_angle;};
inline void gui_app_render(const gui_view_t*){}
inline void gui_app_update_boot(int){}
inline void gui_app_enter_main_ui(){}
