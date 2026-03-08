#include "gui_app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

LV_FONT_DECLARE(lv_font_modi_16_cjk);

static const char *TAG = "GUI";

static SemaphoreHandle_t s_lvgl_lock = NULL;
static gui_event_cb_t s_evt_cb = NULL;

static lv_obj_t *s_lbl_mode = NULL;
static lv_obj_t *s_lbl_usb = NULL;
static lv_obj_t *s_lbl_tf = NULL;
static lv_obj_t *s_lbl_state = NULL;
static lv_obj_t *s_lbl_run_time = NULL;
static lv_obj_t *s_lbl_rpm = NULL;
static lv_obj_t *s_btn_mode = NULL;
static lv_obj_t *s_btn_mode_label = NULL;
static lv_obj_t *s_btn_dir = NULL;
static lv_obj_t *s_btn_dir_label = NULL;
static lv_obj_t *s_btn_speed = NULL;
static lv_obj_t *s_btn_state = NULL;
static lv_obj_t *s_btn_speed_label = NULL;
static lv_obj_t *s_btn_state_label = NULL;

// Count-mode widgets
static lv_obj_t *s_lbl_count = NULL;
static lv_obj_t *s_btn_count_dec = NULL;
static lv_obj_t *s_btn_count_step = NULL;
static lv_obj_t *s_btn_count_step_label = NULL;
static lv_obj_t *s_btn_count_inc = NULL;
static lv_obj_t *s_count_btn_row = NULL;

static lv_obj_t *s_boot_lbl = NULL;
static lv_obj_t *s_boot_bar = NULL;

static const lv_font_t *pick_text_font(void)
{
    return &lv_font_modi_16_cjk;
}

/* ---- Dark industrial color palette ---- */
static const lv_color_t k_bg      = lv_color_hex(0x0f1923);
static const lv_color_t k_card_bg = lv_color_hex(0x1a2332);
static const lv_color_t k_text    = lv_color_hex(0xe2e8f0);
static const lv_color_t k_muted   = lv_color_hex(0x94a3b8);
static const lv_color_t k_border  = lv_color_hex(0x2a3a4a);
static const lv_color_t k_green   = lv_color_hex(0x22c55e);
static const lv_color_t k_red     = lv_color_hex(0xef4444);
static const lv_color_t k_blue    = lv_color_hex(0x3b82f6);
static const lv_color_t k_amber   = lv_color_hex(0xf59e0b);

static lv_style_transition_dsc_t *btn_transition(void)
{
    static lv_style_transition_dsc_t s_trans;
    static bool s_inited = false;
    static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_ZOOM, LV_STYLE_PROP_INV};

    if (!s_inited) {
        lv_style_transition_dsc_init(&s_trans, props, lv_anim_path_ease_out, 100, 0, NULL);
        s_inited = true;
    }
    return &s_trans;
}

static bool lv_lock(void)
{
    if (!s_lvgl_lock) {
        return true;
    }
    return xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void lv_unlock(void)
{
    if (s_lvgl_lock) {
        xSemaphoreGiveRecursive(s_lvgl_lock);
    }
}

static void send_evt(gui_event_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt);
    }
}

static void button_evt_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    gui_event_t evt = (gui_event_t)(uintptr_t)lv_event_get_user_data(e);
    send_evt(evt);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *txt, lv_color_t color, gui_event_t evt)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_height(btn, 60);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_set_style_transition(btn, btn_transition(), 0);
    lv_obj_set_style_transform_zoom(btn, 256, 0);
    lv_obj_set_style_transform_zoom(btn, 240, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);

    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn, button_evt_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)evt);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_font(label, pick_text_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_center(label);
    return btn;
}

static void reset_main_widgets(void)
{
    s_lbl_mode = NULL;
    s_lbl_usb = NULL;
    s_lbl_tf = NULL;
    s_lbl_state = NULL;
    s_lbl_run_time = NULL;
    s_lbl_rpm = NULL;
    s_btn_mode = NULL;
    s_btn_mode_label = NULL;
    s_btn_dir = NULL;
    s_btn_dir_label = NULL;
    s_btn_speed = NULL;
    s_btn_state = NULL;
    s_btn_speed_label = NULL;
    s_btn_state_label = NULL;
    s_lbl_count = NULL;
    s_btn_count_dec = NULL;
    s_btn_count_step = NULL;
    s_btn_count_step_label = NULL;
    s_btn_count_inc = NULL;
    s_count_btn_row = NULL;
}

static void reset_boot_widgets(void)
{
    s_boot_lbl = NULL;
    s_boot_bar = NULL;
}

static void build_boot_layout(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, k_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 24, 0);
    lv_obj_set_style_pad_gap(scr, 20, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MODI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, k_text, 0);

    s_boot_lbl = lv_label_create(scr);
    lv_label_set_text(s_boot_lbl, "\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE4\xB8\xAD 0%");
    lv_obj_set_style_text_font(s_boot_lbl, pick_text_font(), 0);
    lv_obj_set_style_text_color(s_boot_lbl, k_muted, 0);

    s_boot_bar = lv_bar_create(scr);
    lv_obj_set_width(s_boot_bar, LV_PCT(60));
    lv_obj_set_height(s_boot_bar, 6);
    lv_bar_set_range(s_boot_bar, 0, 100);
    lv_bar_set_value(s_boot_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_boot_bar, k_border, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_boot_bar, LV_OPA_100, LV_PART_MAIN);
    lv_obj_set_style_radius(s_boot_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_boot_bar, k_blue, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_boot_bar, 3, LV_PART_INDICATOR);
}

static void build_layout(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, k_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_pad_gap(scr, 8, 0);

    /* ---- Top status bar ---- */
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_remove_style_all(topbar);
    lv_obj_set_size(topbar, LV_PCT(100), 32);
    lv_obj_set_layout(topbar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_mode = lv_label_create(topbar);
    lv_label_set_text(s_lbl_mode, "MODI");
    lv_obj_set_style_text_color(s_lbl_mode, k_text, 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_20, 0);

    lv_obj_t *top_ctrls = lv_obj_create(topbar);
    lv_obj_remove_style_all(top_ctrls);
    lv_obj_set_layout(top_ctrls, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(top_ctrls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_ctrls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(top_ctrls, 8, 0);
    lv_obj_clear_flag(top_ctrls, LV_OBJ_FLAG_SCROLLABLE);

    /* Mode toggle button */
    s_btn_mode = lv_btn_create(top_ctrls);
    lv_obj_set_size(s_btn_mode, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_bg_color(s_btn_mode, k_border, 0);
    lv_obj_set_style_radius(s_btn_mode, 6, 0);
    lv_obj_set_style_border_width(s_btn_mode, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_mode, 0, 0);
    lv_obj_set_style_pad_hor(s_btn_mode, 10, 0);
    lv_obj_set_style_pad_ver(s_btn_mode, 2, 0);
    lv_obj_set_style_bg_opa(s_btn_mode, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_btn_mode, button_evt_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)GUI_EVENT_MODE_SWITCH);

    s_btn_mode_label = lv_label_create(s_btn_mode);
    lv_label_set_text(s_btn_mode_label, "Timer");
    lv_obj_set_style_text_font(s_btn_mode_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_btn_mode_label, k_text, 0);
    lv_obj_center(s_btn_mode_label);

    /* Forward direction button (CW/CCW) */
    s_btn_dir = lv_btn_create(top_ctrls);
    lv_obj_set_size(s_btn_dir, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_bg_color(s_btn_dir, k_border, 0);
    lv_obj_set_style_radius(s_btn_dir, 6, 0);
    lv_obj_set_style_border_width(s_btn_dir, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_dir, 0, 0);
    lv_obj_set_style_pad_hor(s_btn_dir, 10, 0);
    lv_obj_set_style_pad_ver(s_btn_dir, 2, 0);
    lv_obj_set_style_bg_opa(s_btn_dir, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_btn_dir, button_evt_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)GUI_EVENT_FORWARD_DIR_TOGGLE);

    s_btn_dir_label = lv_label_create(s_btn_dir);
    lv_label_set_text(s_btn_dir_label, "FWD CW");
    lv_obj_set_style_text_font(s_btn_dir_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_btn_dir_label, k_text, 0);
    lv_obj_center(s_btn_dir_label);

    lv_obj_t *indicators = lv_obj_create(topbar);
    lv_obj_remove_style_all(indicators);
    lv_obj_set_layout(indicators, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(indicators, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(indicators, 14, 0);
    lv_obj_clear_flag(indicators, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_usb = lv_label_create(indicators);
    lv_label_set_text(s_lbl_usb, LV_SYMBOL_USB " --");
    lv_obj_set_style_text_color(s_lbl_usb, k_muted, 0);
    lv_obj_set_style_text_font(s_lbl_usb, &lv_font_montserrat_14, 0);

    s_lbl_tf = lv_label_create(indicators);
    lv_label_set_text(s_lbl_tf, LV_SYMBOL_SD_CARD " --");
    lv_obj_set_style_text_color(s_lbl_tf, k_muted, 0);
    lv_obj_set_style_text_font(s_lbl_tf, &lv_font_montserrat_14, 0);

    /* ---- Main content card ---- */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, k_card_bg, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, k_border, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_lbl_state = lv_label_create(card);
    lv_label_set_text(s_lbl_state, "\xE5\x81\x9C\xE6\xAD\xA2");
    lv_obj_set_style_text_font(s_lbl_state, pick_text_font(), 0);
    lv_obj_set_style_text_color(s_lbl_state, k_muted, 0);

    s_lbl_run_time = lv_label_create(card);
    lv_label_set_text(s_lbl_run_time, "00:00.00");
    lv_obj_set_style_text_color(s_lbl_run_time, k_text, 0);
    lv_obj_set_style_text_font(s_lbl_run_time, &lv_font_montserrat_48, 0);

    /* Rotation count label (count mode only, hidden by default) */
    s_lbl_count = lv_label_create(card);
    lv_label_set_text(s_lbl_count, "0/100");
    lv_obj_set_style_text_color(s_lbl_count, k_text, 0);
    lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_48, 0);
    lv_obj_add_flag(s_lbl_count, LV_OBJ_FLAG_HIDDEN);

    s_lbl_rpm = lv_label_create(card);
    lv_label_set_text(s_lbl_rpm, "0 RPM");
    lv_obj_set_style_text_color(s_lbl_rpm, k_muted, 0);
    lv_obj_set_style_text_font(s_lbl_rpm, &lv_font_montserrat_20, 0);

    /* Count adjust buttons row (count mode only, hidden by default) */
    s_count_btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(s_count_btn_row);
    lv_obj_set_width(s_count_btn_row, LV_PCT(60));
    lv_obj_set_height(s_count_btn_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(s_count_btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_count_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_count_btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_count_btn_row, 20, 0);
    lv_obj_clear_flag(s_count_btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_count_btn_row, LV_OBJ_FLAG_HIDDEN);

    s_btn_count_dec  = make_button(s_count_btn_row, LV_SYMBOL_MINUS, k_border, GUI_EVENT_COUNT_DEC);
    s_btn_count_step = make_button(s_count_btn_row, "x10", k_blue, GUI_EVENT_COUNT_STEP_TOGGLE);
    s_btn_count_inc  = make_button(s_count_btn_row, LV_SYMBOL_PLUS, k_border, GUI_EVENT_COUNT_INC);
    lv_obj_set_size(s_btn_count_dec, 64, 40);
    lv_obj_set_size(s_btn_count_step, 64, 40);
    lv_obj_set_size(s_btn_count_inc, 64, 40);
    s_btn_count_step_label = lv_obj_get_child(s_btn_count_step, 0);

    /* ---- Bottom buttons ---- */
    lv_obj_t *btn_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(btn_panel);
    lv_obj_set_width(btn_panel, LV_PCT(100));
    lv_obj_set_layout(btn_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_panel, 10, 0);
    lv_obj_clear_flag(btn_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_speed = make_button(btn_panel, "Speed 60", k_blue, GUI_EVENT_SPEED_STEP);
    s_btn_state = make_button(btn_panel, "Start", k_green, GUI_EVENT_STATE_TOGGLE);
    s_btn_speed_label = lv_obj_get_child(s_btn_speed, 0);
    s_btn_state_label = lv_obj_get_child(s_btn_state, 0);

    lv_obj_set_flex_grow(s_btn_speed, 1);
    lv_obj_set_flex_grow(s_btn_state, 1);
    lv_obj_set_height(s_btn_speed, 64);
    lv_obj_set_height(s_btn_state, 64);
}

esp_err_t gui_app_init(SemaphoreHandle_t lvgl_lock, gui_event_cb_t cb)
{
    s_lvgl_lock = lvgl_lock;
    s_evt_cb = cb;

    if (!lv_lock()) {
        ESP_LOGE(TAG, "LVGL lock failed at init");
        return ESP_FAIL;
    }

    lv_obj_clean(lv_scr_act());
    reset_main_widgets();
    reset_boot_widgets();
    build_boot_layout();
    ESP_LOGI(TAG, "LVGL display res: %dx%d", lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_unlock();
    return ESP_OK;
}

void gui_app_update_boot(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (!lv_lock()) return;
    if (s_boot_bar) {
        lv_bar_set_value(s_boot_bar, percent, LV_ANIM_OFF);
    }
    if (s_boot_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE4\xB8\xAD %u%%", (unsigned)percent);
        lv_label_set_text(s_boot_lbl, buf);
    }
    lv_unlock();
}

void gui_app_enter_main_ui(void)
{
    if (!lv_lock()) return;
    lv_obj_clean(lv_scr_act());
    reset_boot_widgets();
    reset_main_widgets();
    build_layout();

    gui_app_update_usb(false);
    gui_app_update_tf(false);
    lv_unlock();
}

void gui_app_update_motor(bool running, bool reversing, bool paused, int16_t rpm, bool can_reverse,
                          uint8_t mode, bool forward_ccw)
{
    if (!lv_lock()) return;
    if (!s_lbl_rpm || !s_btn_speed || !s_btn_state) {
        lv_unlock();
        return;
    }
    if (s_lbl_state) {
        const char *state = "\xE5\x81\x9C\xE6\xAD\xA2";
        lv_color_t state_color = k_muted;
        if (mode == 1) {
            if (running) {
                if (reversing) {
                    state = "\xE5\x8F\x8D\xE8\xBD\xAC\xE5\x9B\x9E\xE4\xBD\x8D";
                    state_color = k_amber;
                } else {
                    state = "\xE6\xAD\xA3\xE8\xBD\xAC";
                    state_color = k_green;
                }
            } else if (paused) {
                state = "\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C";
                state_color = k_blue;
            }
        } else if (running) {
            if (reversing) {
                state = "\xE5\x8F\x8D\xE8\xBD\xAC\xE5\x9B\x9E\xE4\xBD\x8D";
                state_color = k_amber;
            } else {
                state = "\xE6\xAD\xA3\xE8\xBD\xAC";
                state_color = k_green;
            }
        }
        lv_label_set_text(s_lbl_state, state);
        lv_obj_set_style_text_color(s_lbl_state, state_color, 0);
    }

    char dir_buf[8] = "";
    if (running) {
        const bool is_ccw = reversing ? !forward_ccw : forward_ccw;
        snprintf(dir_buf, sizeof(dir_buf), " %s", is_ccw ? "CCW" : "CW");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d RPM%s", rpm, dir_buf);
    lv_label_set_text(s_lbl_rpm, buf);

    if (s_btn_speed_label) {
        if (mode == 1) {
            lv_label_set_text(s_btn_speed_label, reversing ? "Stop Rev" : "Stop->Rev");
        } else {
            char sbuf[24];
            snprintf(sbuf, sizeof(sbuf), "Speed %d", rpm);
            lv_label_set_text(s_btn_speed_label, sbuf);
        }
    }

    if (s_btn_state_label) {
        const char *txt = "Start";
        lv_color_t btn_color = k_green;
        if (mode == 1) {
            if (running) {
                txt = "Pause";
                btn_color = k_blue;
            } else if (paused) {
                txt = "Resume";
                btn_color = k_green;
            }
        } else {
            if (running) {
                txt = "Stop";
                btn_color = k_red;
            } else if (can_reverse) {
                txt = "Reverse";
                btn_color = k_amber;
            }
        }
        lv_label_set_text(s_btn_state_label, txt);
        lv_obj_set_style_bg_color(s_btn_state, btn_color, 0);
    }

    if (mode == 0 && running) {
        lv_obj_add_state(s_btn_speed, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_btn_speed, LV_STATE_DISABLED);
    }

    lv_unlock();
}

void gui_app_update_actual_rpm(int16_t actual_rpm10)
{
    if (!lv_lock()) return;
    if (s_lbl_rpm) {
        int actual = actual_rpm10 < 0 ? -actual_rpm10 : actual_rpm10;
        actual = (actual + 5) / 10; // round to nearest RPM
        char buf[32];
        snprintf(buf, sizeof(buf), "%d RPM", actual);
        lv_label_set_text(s_lbl_rpm, buf);
    }
    lv_unlock();
}

void gui_app_update_run_time(uint32_t centiseconds)
{
    if (!lv_lock()) return;
    if (!s_lbl_run_time) {
        lv_unlock();
        return;
    }
    uint32_t mm = centiseconds / 6000;
    uint32_t ss = (centiseconds / 100) % 60;
    uint32_t cs = centiseconds % 100;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02u:%02u.%02u", (unsigned)mm, (unsigned)ss, (unsigned)cs);
    lv_label_set_text(s_lbl_run_time, buf);
    lv_unlock();
}

void gui_app_update_usb(bool mounted)
{
    if (!lv_lock()) return;
    if (!s_lbl_usb) {
        lv_unlock();
        return;
    }
    lv_label_set_text(s_lbl_usb, mounted ? LV_SYMBOL_USB " ON" : LV_SYMBOL_USB " OFF");
    lv_obj_set_style_text_color(s_lbl_usb, mounted ? k_green : k_muted, 0);
    lv_unlock();
}

void gui_app_update_tf(bool mounted)
{
    if (!lv_lock()) return;
    if (!s_lbl_tf) {
        lv_unlock();
        return;
    }
    lv_label_set_text(s_lbl_tf, mounted ? LV_SYMBOL_SD_CARD " ON" : LV_SYMBOL_SD_CARD " OFF");
    lv_obj_set_style_text_color(s_lbl_tf, mounted ? k_green : k_muted, 0);
    lv_unlock();
}

void gui_app_sync_button(gui_event_t evt)
{
    if (!lv_lock()) return;
    lv_obj_t *btn = NULL;
    switch (evt) {
    case GUI_EVENT_SPEED_STEP: btn = s_btn_speed; break;
    case GUI_EVENT_STATE_TOGGLE: btn = s_btn_state; break;
    case GUI_EVENT_MODE_SWITCH: btn = s_btn_mode; break;
    case GUI_EVENT_COUNT_INC: btn = s_btn_count_inc; break;
    case GUI_EVENT_COUNT_DEC: btn = s_btn_count_dec; break;
    case GUI_EVENT_COUNT_STEP_TOGGLE: btn = s_btn_count_step; break;
    case GUI_EVENT_FORWARD_DIR_TOGGLE: btn = s_btn_dir; break;
    default: break;
    }
    if (btn) {
        lv_event_send(btn, LV_EVENT_CLICKED, NULL);
    }
    lv_unlock();
}

void gui_app_set_mode(uint8_t mode)
{
    if (!lv_lock()) return;
    // Update mode button label
    if (s_btn_mode_label) {
        lv_label_set_text(s_btn_mode_label, mode == 0 ? "Timer" : "Count");
    }
    // Show/hide timer vs count widgets
    if (s_lbl_run_time) {
        if (mode == 0) {
            lv_obj_clear_flag(s_lbl_run_time, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_run_time, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_lbl_count) {
        // Rotation count visible in both modes; font size differs by mode
        lv_obj_clear_flag(s_lbl_count, LV_OBJ_FLAG_HIDDEN);
        if (mode == 0) {
            // Timer mode: smaller font alongside the timer display
            lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_20, 0);
        } else {
            // Count mode: large font as primary display
            lv_obj_set_style_text_font(s_lbl_count, &lv_font_montserrat_48, 0);
        }
    }
    if (s_count_btn_row) {
        if (mode == 1) {
            lv_obj_clear_flag(s_count_btn_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_count_btn_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_unlock();
}

void gui_app_update_rotation_count(int32_t current, int32_t target)
{
    if (!lv_lock()) return;
    if (s_lbl_count) {
        char buf[32];
        if (target > 0) {
            snprintf(buf, sizeof(buf), "%ld/%ld", (long)current, (long)target);
        } else {
            snprintf(buf, sizeof(buf), "%ld", (long)current);
        }
        lv_label_set_text(s_lbl_count, buf);
    }
    lv_unlock();
}

void gui_app_update_count_buttons(bool enabled)
{
    if (!lv_lock()) return;
    if (s_btn_count_dec) {
        if (enabled) {
            lv_obj_clear_state(s_btn_count_dec, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_count_dec, LV_STATE_DISABLED);
        }
    }
    if (s_btn_count_inc) {
        if (enabled) {
            lv_obj_clear_state(s_btn_count_inc, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_count_inc, LV_STATE_DISABLED);
        }
    }
    if (s_btn_count_step) {
        if (enabled) {
            lv_obj_clear_state(s_btn_count_step, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_count_step, LV_STATE_DISABLED);
        }
    }
    lv_unlock();
}

void gui_app_update_state_button(const char *text, uint32_t color_hex)
{
    if (!lv_lock()) return;
    if (s_btn_state_label) {
        lv_label_set_text(s_btn_state_label, text);
    }
    if (s_btn_state) {
        lv_obj_set_style_bg_color(s_btn_state, lv_color_hex(color_hex), 0);
    }
    lv_unlock();
}

void gui_app_update_forward_direction(bool forward_ccw)
{
    if (!lv_lock()) return;
    if (s_btn_dir_label) {
        lv_label_set_text(s_btn_dir_label, forward_ccw ? "FWD CCW" : "FWD CW");
    }
    if (s_btn_dir) {
        lv_obj_set_style_bg_color(s_btn_dir, forward_ccw ? k_amber : k_border, 0);
    }
    lv_unlock();
}

void gui_app_update_count_step(int step)
{
    if (!lv_lock()) return;
    if (s_btn_count_step_label) {
        lv_label_set_text(s_btn_count_step_label, step == 1 ? "x1" : "x10");
    }
    lv_unlock();
}
