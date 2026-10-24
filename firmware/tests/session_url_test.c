/* Protect the account boundary: cloud requests stay at Gateway and an
 * untrusted session identifier cannot change the authenticated request path. */
#include "../components/kbx_voice/session_url.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    char url[256];
    assert(kbx_session_url(KBX_CLOUD_SERVER, NULL, false, url, sizeof(url)));
    assert(!strcmp(url, "https://api.kombify.io/v1/speechkit/voiceagent/sessions"));
    assert(kbx_session_url(KBX_CLOUD_SERVER, "turn-1", true, url, sizeof(url)));
    assert(!strcmp(url, "wss://api.kombify.io/v1/speechkit/voiceagent/sessions/turn-1/ws"));
    assert(kbx_session_url("https://selfhost.example", "turn-1", false, url, sizeof(url)));
    assert(!strcmp(url, "https://selfhost.example/v1/voiceagent/sessions/turn-1"));
    assert(!kbx_cloud_server("https://api.kombify.io.attacker.example/v1/speechkit"));
    assert(!kbx_session_url(KBX_CLOUD_SERVER, "../admin", true, url, sizeof(url)));
    assert(!kbx_session_url("https://host?redirect=", "turn", false, url, sizeof(url)));
    assert(!kbx_session_url(KBX_CLOUD_SERVER, "turn", true, url, 12));
    return 0;
}
