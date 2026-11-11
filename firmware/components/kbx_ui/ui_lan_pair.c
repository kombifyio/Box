/*
 * kombify box - LAN pairing sheet: shows the 6-digit code the homelab
 * speechkit-server asks the operator to approve (lan_pair.h), then the
 * result. Same full-screen sheet primitives as the Cloud pairing sheet, so
 * the round 1.85C and the 1-bit RLCD 4.2 both render it from the display
 * profile. LVGL context only (own timer).
 */
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "lvgl.h"

#include "display_profile.h"
#include "i18n.h"
#include "lan_pair.h"
#include "ui.h"
#include "ui_style.h"
#include "ui_theme.h"

extern const lv_font_t kbx_num_56;

#define LAN_SHOW_RESULT_US (8 * 1000 * 1000)   /* paired: brief confirmation */
#define LAN_SHOW_FAILURE_US (30 * 1000 * 1000) /* failure stays readable    */

static lv_obj_t *s_sheet;
static lv_obj_t *s_title;
static lv_obj_t *s_code;
static lv_obj_t *s_server;
static lv_obj_t *s_hint;
static lv_obj_t *s_detail;
static uint32_t s_seq = UINT32_MAX;
static uint32_t s_locale = UINT32_MAX;
static int64_t s_changed_us;
static bool s_visible;

static i18n_id_t failure_text(const char *error)
{
    if (!strcmp(error, "offline")) return I18N_ERR_OFFLINE;
    if (!strcmp(error, "no_server") || !strcmp(error, "unreachable") || !strcmp(error, "no_pairing") ||
        !strcmp(error, "server_error") || !strcmp(error, "invalid_response")) return I18N_ERR_SERVICE;
    if (!strcmp(error, "denied") || !strcmp(error, "pairing_off") || !strcmp(error, "not_lan") ||
        !strcmp(error, "refused")) return I18N_ERR_REFUSED;
    if (!strcmp(error, "busy")) return I18N_ERR_BUSY;
    if (!strcmp(error, "expired")) return I18N_ERR_RETRY;
    return I18N_ERR_UNKNOWN;
}

static void render(const lan_pair_status_t *st)
{
    i18n_style_label(s_title, I18N_FONT_18);
    i18n_style_label(s_hint, I18N_FONT_16);
    bool code = st->phase == LAN_PAIR_SHOW_CODE && st->code[0];
    bool paired = st->phase == LAN_PAIR_PAIRED;
    lv_label_set_text(s_title, i18n_str(paired ? I18N_CLOUD_CONNECTED : I18N_CLOUD_PAIR));

    if (code) {
        char shown[10];
        snprintf(shown, sizeof(shown), "%.3s %.3s", st->code, st->code + 3);
        kbx_set_text(s_code, shown);
    }
    kbx_set_hidden(s_code, !code);

    char server[112];
    if (st->server[0]) {
        snprintf(server, sizeof(server), "%s", st->instance[0] ? st->instance : st->server);
    } else {
        snprintf(server, sizeof(server), "_speechkit._tcp");
    }
    kbx_set_text(s_server, server);

    i18n_id_t hint = I18N_STATUS_CONNECTING;
    if (paired) hint = I18N_CLOUD_READY;
    else if (st->phase == LAN_PAIR_FAILED) hint = failure_text(st->error);
    kbx_set_hidden(s_hint, code);
    lv_label_set_text(s_hint, i18n_str(hint));

    /* ASCII reason code for the failure (or the server host under a code):
     * the operator can match it against `KBX lan?` and the server log. */
    if (st->phase == LAN_PAIR_FAILED) kbx_set_text(s_detail, st->error);
    else if (code) kbx_set_text(s_detail, st->server);
    else kbx_set_text(s_detail, "");
}

static void lan_timer_cb(lv_timer_t *t)
{
    (void)t;
    lan_pair_status_t st;
    lan_pair_get_status(&st);
    int64_t now = esp_timer_get_time();
    if (st.seq != s_seq) {
        s_seq = st.seq;
        s_changed_us = now;
    }
    bool show = st.phase == LAN_PAIR_BROWSING || st.phase == LAN_PAIR_REQUESTING || st.phase == LAN_PAIR_SHOW_CODE ||
                (st.phase == LAN_PAIR_PAIRED && now - s_changed_us < LAN_SHOW_RESULT_US) ||
                (st.phase == LAN_PAIR_FAILED && now - s_changed_us < LAN_SHOW_FAILURE_US);
    /* The Cloud pairing sheet owns the screen while it is open. */
    if (ui_pairing_is_open()) show = false;
    if (show && (!s_visible || s_changed_us == now || s_locale != i18n_epoch())) {
        if (!s_visible) lv_obj_move_foreground(s_sheet);
        render(&st);
        s_locale = i18n_epoch();
    }
    if (show != s_visible) {
        kbx_set_hidden(s_sheet, !show);
        s_visible = show;
    }
}

bool ui_lan_pair_is_visible(void)
{
    return s_visible;
}

void ui_lan_pair_build(lv_obj_t *scr)
{
    s_sheet = kbx_sheet(scr);

    s_title = kbx_text_block(s_sheet, I18N_FONT_18, KBX_COL_TEXT_HI, kbx_dp(240));
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kbx_dp(70));

    s_code = kbx_label(s_sheet, I18N_FONT_20, KBX_COL_TEXT_HI);
    lv_obj_set_style_text_font(s_code, &kbx_num_56, 0);
    lv_obj_set_style_base_dir(s_code, LV_BASE_DIR_LTR, 0);
    lv_obj_align(s_code, LV_ALIGN_TOP_MID, 0, kbx_dp(132));

    s_hint = kbx_text_block(s_sheet, I18N_FONT_16, KBX_COL_TEXT_MID, kbx_dp(240));
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, kbx_dp(150));

    s_server = kbx_label(s_sheet, I18N_FONT_16, KBX_COL_TEXT_MID);
    lv_obj_set_style_text_font(s_server, &lv_font_montserrat_16, 0);
    lv_obj_set_style_base_dir(s_server, LV_BASE_DIR_LTR, 0);
    lv_obj_set_width(s_server, kbx_dp(240));
    lv_label_set_long_mode(s_server, LV_LABEL_LONG_DOT);
    lv_obj_align(s_server, LV_ALIGN_TOP_MID, 0, kbx_dp(220));

    s_detail = kbx_label(s_sheet, I18N_FONT_14, KBX_COL_TEXT_LO);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_base_dir(s_detail, LV_BASE_DIR_LTR, 0);
    lv_obj_set_width(s_detail, kbx_dp(220));
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, kbx_dp(250));

    kbx_set_hidden(s_sheet, true);
    lv_timer_create(lan_timer_cb, 200, NULL);
}
