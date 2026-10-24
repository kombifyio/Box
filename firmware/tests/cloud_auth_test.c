/* Real cJSON boundary tests: ambiguous/truncated OAuth credentials must never
 * be provisioned, and refresh characters must not inject form parameters.
 * Compile this file with the ESP-IDF cJSON.c and its include directory, and
 * with -I components/kbx_core/include. */
#define CLOUD_AUTH_NATIVE_TEST
#include "../components/kbx_core/cloud_auth.c"
#include <assert.h>

int main(void)
{
    char token[8] = "";
    int seconds = 0;
    assert(json_field("{\"access_token\":\"abc_-.\",\"expires_in\":900}", "access_token", token, sizeof(token), NULL));
    assert(!strcmp(token, "abc_-.") );
    assert(json_field("{\"expires_in\":900}", "expires_in", NULL, 0, &seconds));
    assert(seconds == 900);
    assert(!json_field("{\"access_token\":\"12345678\"}", "access_token", token, sizeof(token), NULL));
    assert(!json_field("{\"access_token\":\"one\",\"access_token\":\"two\"}", "access_token", token, sizeof(token), NULL));
    assert(!json_field("{\"access_token\":\"a\\u0000b\"}", "access_token", token, sizeof(token), NULL));
    assert(!json_field("{\"access_token\":\"abc\"}garbage", "access_token", token, sizeof(token), NULL));
    assert(!json_field("{\"access_token\":42}", "access_token", token, sizeof(token), NULL));
    assert(!json_field("{\"expires_in\":900.5}", "expires_in", NULL, 0, &seconds));
    assert(!json_field("{\"expires_in\":-1}", "expires_in", NULL, 0, &seconds));
    assert(!json_field("{\"expires_in\":1e20}", "expires_in", NULL, 0, &seconds));
    assert(!json_has_field("{\"access_token\":\"abc\"}", "refresh_token"));
    assert(json_has_field("{\"refresh_token\":null}", "refresh_token"));
    assert(!json_field("{\"refresh_token\":null}", "refresh_token", token, sizeof(token), NULL));
    char encoded[64] = "";
    assert(form_encode("a+b&grant=x/%?", encoded, sizeof(encoded)));
    assert(!strcmp(encoded, "a%2Bb%26grant%3Dx%2F%25%3F"));
    assert(!form_encode("a+b", encoded, 5));
    assert(!strcmp(safe_oauth_error("invalid_grant"), "invalid_grant"));
    assert(!strcmp(safe_oauth_error("unauthorized_client"), "unauthorized_client"));
    assert(!strcmp(safe_oauth_error("access_denied"), "access_denied"));
    assert(!strcmp(safe_oauth_error("secret-like-value\naccess_token=private"), "unknown_oauth_error"));
    assert(poll_http_retryable(429));
    assert(poll_http_retryable(500));
    assert(poll_http_retryable(503));
    assert(poll_http_retryable(599));
    assert(!poll_http_retryable(403));
    assert(!poll_http_retryable(400));
    assert(!poll_http_retryable(600));
    assert(poll_backoff_interval(5) == 10);
    assert(poll_backoff_interval(1800) == 3600);
    size_t access_bytes = 0;
    assert(access_response_error("{\"access_token\":\"abc\",\"expires_in\":2592000}", token, sizeof(token), &seconds, &access_bytes) == NULL);
    assert(seconds == 2592000 && access_bytes == 3);
    assert(!strcmp(access_response_error("{\"expires_in\":900}", token, sizeof(token), &seconds, &access_bytes), "missing_access_token"));
    assert(!strcmp(access_response_error("{\"access_token\":123,\"expires_in\":900}", token, sizeof(token), &seconds, &access_bytes), "invalid_access_token_type"));
    assert(!strcmp(access_response_error("{\"access_token\":\"12345678\",\"expires_in\":900}", token, sizeof(token), &seconds, &access_bytes), "access_token_too_large"));
    assert(!strcmp(access_response_error("{\"access_token\":\"abc\",\"expires_in\":-1}", token, sizeof(token), &seconds, &access_bytes), "invalid_token_expiry"));
    /* Live Auth0 regression: the approved account's JWT was 4758 bytes.
     * Keep the transport's expanded bound explicit without stack-sized buffers. */
    char *large_access = calloc(8192, 1);
    char *large_response = calloc(9000, 1);
    assert(large_access && large_response);
    memset(large_response, 'a', 4758);
    large_response[4758] = 0;
    char *access_fixture = calloc(8193, 1);
    assert(access_fixture);
    memcpy(access_fixture, large_response, 4759);
    snprintf(large_response, 9000, "{\"access_token\":\"%s\",\"expires_in\":900}", access_fixture);
    assert(access_response_error(large_response, large_access, 8192, &seconds, &access_bytes) == NULL);
    assert(access_bytes == 4758 && strlen(large_access) == 4758);
    memset(access_fixture, 'b', 8192);
    access_fixture[8192] = 0;
    snprintf(large_response, 9000, "{\"access_token\":\"%s\",\"expires_in\":900}", access_fixture);
    assert(!strcmp(access_response_error(large_response, large_access, 8192, &seconds, &access_bytes), "access_token_too_large"));
    free(access_fixture); free(large_response); free(large_access);
    char payload[512];
    const char *valid_jwt = "e30.eyJhdWQiOiJodHRwczovL2FwaS5rb21iaWZ5LmlvIiwiaXNzIjoiaHR0cHM6Ly9sb2dpbi5rb21iaWZ5LmlvLyIsImV4cCI6MTgwMDAwMDkwMH0.sig";
    assert(jwt_claims_valid(valid_jwt, 1800000000, payload, sizeof(payload), &seconds));
    assert(seconds == 900);
    assert(jwt_claims_valid("e30.eyJhdWQiOiJodHRwczovL2FwaS5rb21iaWZ5LmlvIiwiaXNzIjoiaHR0cHM6Ly9sb2dpbi5rb21iaWZ5LmlvLyIsImV4cCI6MTgwMjU5MjAwMH0.sig", 1800000000, payload, sizeof(payload), &seconds));
    assert(seconds == 86400); /* Accept long issuer lifetime; cache at most a day. */
    assert(jwt_claims_valid("e30.eyJhdWQiOlsib3RoZXIiLCJodHRwczovL2FwaS5rb21iaWZ5LmlvIl0sImlzcyI6Imh0dHBzOi8vbG9naW4ua29tYmlmeS5pby8iLCJleHAiOjE4MDAwMDA5MDB9.sig", 1800000000, payload, sizeof(payload), &seconds));
    assert(!jwt_claims_valid("e30.eyJhdWQiOiJodHRwczovL3dyb25nLmV4YW1wbGUiLCJpc3MiOiJodHRwczovL2xvZ2luLmtvbWJpZnkuaW8vIiwiZXhwIjoxODAwMDAwOTAwfQ.sig", 1800000000, payload, sizeof(payload), &seconds));
    assert(!jwt_claims_valid("e30.eyJhdWQiOiJodHRwczovL2FwaS5rb21iaWZ5LmlvIiwiaXNzIjoiaHR0cHM6Ly93cm9uZy5leGFtcGxlLyIsImV4cCI6MTgwMDAwMDkwMH0.sig", 1800000000, payload, sizeof(payload), &seconds));
    assert(!jwt_claims_valid(valid_jwt, 1800000900, payload, sizeof(payload), &seconds));
    assert(!jwt_claims_valid(valid_jwt, 0, payload, sizeof(payload), &seconds));
    assert(!jwt_claims_valid(valid_jwt, 1800000000, payload, 8, &seconds));
    assert(!jwt_claims_valid("opaque_token", 1800000000, payload, sizeof(payload), &seconds));
    assert(!base64url_payload("Zg_", 3, payload, sizeof(payload))); /* Nonzero padding bits. */
    puts("cloud auth credential boundary tests passed");
    return 0;
}
