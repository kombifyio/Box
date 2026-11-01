#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "voice_protocol.h"
#include "voice_control.h"
#include "voice_error.h"

static kbx_voice_text_t message;

static unsigned phases(const char *json)
{
    cJSON *root = cJSON_ParseWithLengthOpts(json, strlen(json) + 1U, NULL, true);
    assert(cJSON_IsObject(root));
    unsigned flags = kbx_voice_phases(root);
    cJSON_Delete(root);
    return flags;
}

static void control_receipts(void)
{
    /* A transcript or a receipt from another voice session must never grant
     * follow-up capture, even when it contains the requested words. */
    kbx_voice_control_t control;
    cJSON *root = cJSON_Parse("{\"type\":\"event\",\"event_type\":\"conversation_mode\","
        "\"conversation\":{\"version\":1,\"mode\":\"conversation\",\"status\":\"accepted\","
        "\"voice_session_id\":\"voice-1\",\"conversation_session_id\":\"history-2\",\"request_id\":\"tool-3\"}}");
    assert(root);
    assert(kbx_voice_control(root, "voice-1", &control));
    assert(control.accepted && control.mode == KBX_VOICE_MODE_CONVERSATION);
    assert(!strcmp(control.conversation_session_id, "history-2"));
    assert(!strcmp(control.request_id, "tool-3"));
    assert(!kbx_voice_control(root, "other-voice", &control));
    cJSON *receipt = cJSON_GetObjectItemCaseSensitive(root, "conversation");
    cJSON_ReplaceItemInObjectCaseSensitive(receipt, "status", cJSON_CreateString("rejected"));
    cJSON_ReplaceItemInObjectCaseSensitive(receipt, "mode", cJSON_CreateString("oneshot"));
    assert(kbx_voice_control(root, "voice-1", &control));
    assert(!control.accepted && control.mode == KBX_VOICE_MODE_ONESHOT);
    cJSON_ReplaceItemInObjectCaseSensitive(receipt, "version", cJSON_CreateNumber(2));
    assert(!kbx_voice_control(root, "voice-1", &control));
    cJSON_ReplaceItemInObjectCaseSensitive(receipt, "version", cJSON_CreateNumber(1));
    cJSON_ReplaceItemInObjectCaseSensitive(receipt, "conversation_session_id", cJSON_CreateString("../other-owner"));
    assert(!kbx_voice_control(root, "voice-1", &control));
    /* The unreleased generic v1 contract replaces the private wire key;
     * accepting an old-key receipt would disagree with the server schema. */
    cJSON_DeleteItemFromObjectCaseSensitive(receipt, "conversation_session_id");
    cJSON_AddStringToObject(receipt, "workbench_session_id", "history-2");
    assert(!kbx_voice_control(root, "voice-1", &control));
    cJSON_Delete(root);
    root = cJSON_Parse("{\"type\":\"input_transcript\",\"done\":true,\"text\":\"Unterhaltung starten\"}");
    assert(!kbx_voice_control(root, "voice-1", &control));
    cJSON_Delete(root);
}

static void route_switch_receipts(void)
{
    /* Only the server's typed receipt for THIS session switches the route, and
     * the replayed utterance stays valid JSON whatever it contains. */
    kbx_voice_route_switch_t sw;
    const char *good = "{\"type\":\"event\",\"event_type\":\"route_switch\",\"route\":{\"version\":1,"
        "\"to\":\"agent\",\"voice_session_id\":\"voice-1\",\"request_id\":\"route-4\","
        "\"reason\":\"explicit_companion\",\"utterance\":\"frag den Companion nach \\\"Größe\\\"\"}}";
    cJSON *root = cJSON_Parse(good);
    assert(root);
    assert(kbx_voice_route_switch(root, "voice-1", &sw));
    assert(!strcmp(sw.request_id, "route-4") && !strcmp(sw.reason, "explicit_companion"));
    assert(!strcmp(sw.utterance, "frag den Companion nach \"Größe\""));
    char frame[128];
    assert(kbx_voice_text_frame(sw.utterance, frame, sizeof(frame)));
    assert(!strcmp(frame, "{\"type\":\"text\",\"text\":\"frag den Companion nach \\\"Größe\\\"\"}"));
    assert(!kbx_voice_text_frame(sw.utterance, frame, 40));
    /* a receipt of another session fails and never leaves a half-filled result */
    assert(!kbx_voice_route_switch(root, "other-voice", &sw) && !sw.utterance[0]);
    assert(kbx_voice_text_frame("a\n\\", frame, sizeof(frame)));
    assert(!strcmp(frame, "{\"type\":\"text\",\"text\":\"a\\u000a\\\\\"}"));
    cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
    cJSON_ReplaceItemInObjectCaseSensitive(route, "to", cJSON_CreateString("fast"));
    assert(!kbx_voice_route_switch(root, "voice-1", &sw));
    cJSON_ReplaceItemInObjectCaseSensitive(route, "to", cJSON_CreateString("agent"));
    cJSON_ReplaceItemInObjectCaseSensitive(route, "reason", cJSON_CreateString("because"));
    assert(!kbx_voice_route_switch(root, "voice-1", &sw));
    cJSON_ReplaceItemInObjectCaseSensitive(route, "reason", cJSON_CreateString("model_handoff"));
    cJSON_ReplaceItemInObjectCaseSensitive(route, "utterance", cJSON_CreateString(""));
    assert(!kbx_voice_route_switch(root, "voice-1", &sw));
    cJSON_ReplaceItemInObjectCaseSensitive(route, "utterance", cJSON_CreateString("\xC3\x28"));
    assert(!kbx_voice_route_switch(root, "voice-1", &sw));
    char long_text[KBX_VOICE_UTTERANCE_MAX + 2U];
    memset(long_text, 'x', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    cJSON_ReplaceItemInObjectCaseSensitive(route, "utterance", cJSON_CreateString(long_text));
    assert(!kbx_voice_route_switch(root, "voice-1", &sw));
    long_text[KBX_VOICE_UTTERANCE_MAX] = '\0';
    cJSON_ReplaceItemInObjectCaseSensitive(route, "utterance", cJSON_CreateString(long_text));
    assert(kbx_voice_route_switch(root, "voice-1", &sw) && !strcmp(sw.reason, "model_handoff"));
    cJSON_Delete(root);
    root = cJSON_Parse("{\"type\":\"output_transcript\",\"done\":true,\"text\":\"route_switch\"}");
    assert(!kbx_voice_route_switch(root, "voice-1", &sw));
    cJSON_Delete(root);
    /* The account cap is its own class (a busy line), not a denied permission. */
    assert(kbx_voice_error_classify(409, "per_user_limit_exceeded", NULL) == KBX_VOICE_ERR_BUSY);
    assert(kbx_voice_error_classify(0, "per_user_limit_exceeded", NULL) == KBX_VOICE_ERR_BUSY);
    assert(kbx_voice_error_classify(0, "voice_budget_exceeded", NULL) == KBX_VOICE_ERR_DENIED);
    assert(kbx_voice_error_classify(503, "global_limit_exceeded", NULL) == KBX_VOICE_ERR_SERVICE);
}

int main(void)
{
    control_receipts();
    route_switch_receipts();
    /* Regression: a panel request sent just after reply EOF can need longer
     * than the two-second one-shot tail. It must wait for its scoped ACK. */
    assert(kbx_voice_after_reply(false, true, false, INT64_C(3000000)) == KBX_VOICE_REPLY_WAIT);
    assert(kbx_voice_after_reply(false, false, false, INT64_C(3000000)) == KBX_VOICE_REPLY_CLOSE);
    assert(kbx_voice_after_reply(true, false, true, INT64_C(3000000)) == KBX_VOICE_REPLY_WAIT);
    assert(kbx_voice_after_reply(true, false, false, INT64_C(349999)) == KBX_VOICE_REPLY_WAIT);
    assert(kbx_voice_after_reply(true, false, false, INT64_C(350000)) == KBX_VOICE_REPLY_LISTEN);
    /* SpeechKit suppresses standalone events when a transcript carries them. */
    assert(phases("{\"type\":\"input_transcript\",\"text\":\"test phrase\",\"done\":true,"
                  "\"event_type\":\"input_final\",\"event_types\":[\"input_final\"]}") ==
           (KBX_VOICE_INPUT_HEARD | KBX_VOICE_INPUT_FINAL));
    assert(phases("{\"type\":\"output_transcript\",\"text\":\"test reply\",\"done\":true,"
                  "\"event_type\":\"output_text\",\"event_types\":[\"output_text\",\"turn_end\"]}") ==
           (KBX_VOICE_OUTPUT_SEEN | KBX_VOICE_TURN_END));
    assert(phases("{\"type\":\"input_transcript\",\"text\":\"partial\",\"done\":false}") == KBX_VOICE_INPUT_HEARD);
    assert(phases("{\"type\":\"output_transcript\",\"text\":\"reply\",\"done\":true}") == KBX_VOICE_OUTPUT_SEEN);
    assert(phases("{\"type\":\"state\",\"event_types\":[\"input_final\",\"turn_end\"]}") ==
           (KBX_VOICE_INPUT_HEARD | KBX_VOICE_INPUT_FINAL | KBX_VOICE_TURN_END));
    assert(phases("{\"type\":\"interrupted\",\"event_types\":[\"session_end\"]}") ==
           (KBX_VOICE_INTERRUPTED | KBX_VOICE_SESSION_END));

    /* The observed Flux input end precedes the first assistant PCM. It
     * must never arm reply closure, even when old output has been seen. */
    unsigned eager = phases("{\"type\":\"event\",\"event_type\":\"turn_end\","
        "\"event_types\":[\"turn_end\"],\"provider_metadata\":{\"provider_event\":\"EagerEndOfTurn\"}}");
    unsigned input_end = phases("{\"type\":\"event\",\"event_type\":\"turn_end\","
        "\"provider_metadata\":{\"provider_event\":\"EndOfTurn\"}}");
    assert(eager == KBX_VOICE_INPUT_HEARD);
    assert(input_end == (KBX_VOICE_INPUT_HEARD | KBX_VOICE_INPUT_FINAL));
    assert(phases("{\"type\":\"interrupted\",\"provider_metadata\":{"
        "\"provider_event\":\"UserStartedSpeaking\"}}") ==
        (KBX_VOICE_INTERRUPTED | KBX_VOICE_INPUT_HEARD));
    assert(!kbx_voice_turn_ended(eager, true));
    assert(!kbx_voice_turn_ended(input_end, true));
    unsigned output_end = phases("{\"type\":\"event\",\"event_type\":\"turn_end\","
        "\"provider_metadata\":{\"provider_event\":\"AgentAudioDone\"}}");
    assert(kbx_voice_turn_ended(output_end, false));
    unsigned generic_end = phases("{\"type\":\"event\",\"event_types\":[\"turn_end\"],"
        "\"provider_metadata\":{\"provider_event\":\"live.server_message\"}}");
    assert(!kbx_voice_turn_ended(generic_end, false));
    assert(kbx_voice_turn_ended(generic_end, true));
    assert(kbx_voice_turn_ended(phases("{\"type\":\"output_transcript\",\"text\":\"reply\","
        "\"event_types\":[\"output_text\",\"turn_end\"]}"), false));

    /* Control metadata beyond the old 512-byte scratch buffer must survive
     * both SDK chunks and genuine websocket continuation frames. */
    const char prefix[] = "{\"type\":\"output_transcript\",\"text\":\"";
    const char suffix[] = "\",\"done\":true,\"event_types\":[\"output_text\",\"turn_end\"]}";
    char *wire = malloc(KBX_VOICE_TEXT_LIMIT + 1U);
    assert(wire);
    memset(wire, 'x', KBX_VOICE_TEXT_LIMIT);
    memcpy(wire, prefix, sizeof(prefix) - 1U);
    memcpy(wire + KBX_VOICE_TEXT_LIMIT - (sizeof(suffix) - 1U), suffix, sizeof(suffix));
    assert(kbx_voice_text_feed(&message, 1, false, 10000, 0, wire, 4096) == KBX_VOICE_TEXT_PENDING);
    assert(kbx_voice_text_feed(&message, 1, false, 10000, 4096, wire + 4096, 5904) == KBX_VOICE_TEXT_PENDING);
    const size_t rest = KBX_VOICE_TEXT_LIMIT - 10000U;
    size_t offset = 0;
    while (offset < rest) {
        size_t length = rest - offset;
        if (length > 4096U) length = 4096U;
        kbx_voice_text_result_t result = kbx_voice_text_feed(&message, 0, true,
            rest, offset, wire + 10000U + offset, length);
        offset += length;
        assert(result == (offset == rest ? KBX_VOICE_TEXT_COMPLETE : KBX_VOICE_TEXT_PENDING));
    }
    assert(message.used == KBX_VOICE_TEXT_LIMIT);
    assert(phases(message.bytes) == (KBX_VOICE_OUTPUT_SEEN | KBX_VOICE_TURN_END));
    kbx_voice_text_reset(&message);
    assert(kbx_voice_text_feed(&message, 1, true, KBX_VOICE_TEXT_LIMIT + 1U,
        0, wire, 1) == KBX_VOICE_TEXT_INVALID);
    assert(kbx_voice_text_feed(&message, 0, true, 1, 0, wire, 1) == KBX_VOICE_TEXT_INVALID);
    assert(kbx_voice_text_feed(&message, 1, true, 10, 0, wire, 4) == KBX_VOICE_TEXT_PENDING);
    assert(kbx_voice_text_feed(&message, 1, true, 10, 5, wire + 5, 5) == KBX_VOICE_TEXT_INVALID);
    assert(kbx_voice_text_feed(&message, 1, true, 10, 0, wire, 4) == KBX_VOICE_TEXT_PENDING);
    assert(kbx_voice_text_feed(&message, 1, true, 10, 0, wire, 4) == KBX_VOICE_TEXT_INVALID);
    assert(kbx_voice_text_feed(&message, 1, true, 3, 0, "a\0b", 3) == KBX_VOICE_TEXT_INVALID);
    assert(kbx_voice_text_feed(&message, 1, false, 1, 0, "x", 1) == KBX_VOICE_TEXT_PENDING);
    assert(kbx_voice_text_feed(&message, 0, true, KBX_VOICE_TEXT_LIMIT,
        0, wire, 1) == KBX_VOICE_TEXT_INVALID);
    assert(kbx_voice_text_feed(&message, 1, true, 10, 0, wire, 4) == KBX_VOICE_TEXT_PENDING);
    kbx_voice_text_reset(&message);
    assert(!message.active && !message.frame_open && message.used == 0);
    for (size_t i = 0; i < sizeof(message.bytes); ++i) assert(message.bytes[i] == 0);
    free(wire);
    puts("voice protocol phases, authenticated mode receipts, continuation bounds, route-switch receipts and cancellation: ok");
    return 0;
}
