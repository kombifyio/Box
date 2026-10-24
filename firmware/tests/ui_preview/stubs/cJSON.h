/* Host preview stub: declarations only (voice_error.h inlines are unused). */
#pragma once
typedef struct cJSON { char *valuestring; int type; } cJSON;
cJSON *cJSON_Parse(const char *s);
void cJSON_Delete(cJSON *c);
int cJSON_IsObject(const cJSON *c);
int cJSON_IsString(const cJSON *c);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *c, const char *k);
