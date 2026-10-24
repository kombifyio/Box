/*
 * kombify box - tap and hold actions of the voice control. Moved out of
 * touch.c so boards without touch (RLCD 4.2 buttons) trigger the same
 * behaviour.
 */
#include "esp_log.h"

#include "audio.h"
#include "net_ws.h"
#include "talk_action.h"
#include "ui.h"

static const char *TAG = "kbx_talk";

static bool playback_busy(void)
{
    return audio_is_speaking() || !audio_play_agent_idle();
}

static void stop_playback(void)
{
    net_ws_talk_cancel();
    audio_play_agent_holdoff();
}

void talk_action_tap(void)
{
    /* Conversation cancellation applies even between spoken turns. */
    if (net_ws_conversation_active()) {
        stop_playback();
        ESP_LOGI(TAG, "tap -> conversation stop");
        return;
    }
    /* Tap is talk only. Never open the status/settings card from a tap
     * (that card used to own tap; user-report 2026-08-25). */
    if (playback_busy()) {
        stop_playback();
        ESP_LOGI(TAG, "tap -> stop playback");
        return;
    }
    if (net_ws_talk_active()) {
        (void)net_ws_talk_toggle();
        ESP_LOGI(TAG, "tap -> talk stop");
        return;
    }
    if (net_ws_talk_toggle()) {
        ESP_LOGI(TAG, "tap -> talk start");
        return;
    }
    /* Say why when the owner can act on it: no internet or signed out. */
    kbx_voice_error_kind_t why = net_ws_unready_reason();
    if (why == KBX_VOICE_ERR_OFFLINE) ui_notify_talk_denied(I18N_ERR_OFFLINE);
    else if (why == KBX_VOICE_ERR_SIGNIN) ui_notify_talk_denied(I18N_ERR_SIGNIN);
    ESP_LOGW(TAG, "tap -> talk not ready (reason=%s)", kbx_voice_error_name(why));
}

void talk_action_hold_mute(void)
{
    audio_toggle_local_mute();
    if (audio_local_mute() && net_ws_talk_active()) {
        (void)net_ws_talk_toggle();
    }
    ui_notify_tap();
    ESP_LOGI(TAG, "hold -> local mute=%d", (int)audio_local_mute());
}
