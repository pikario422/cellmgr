#include "profile.h"
#include "json_util.h"

static char *find_capability_block(const char *json, const char *capability)
{
    char needle[192];
    snprintf(needle, sizeof(needle), "\"%s\"", capability);
    const char *p = strstr(json, needle);
    if (!p) {
        return NULL;
    }
    const char *brace = strchr(p, '{');
    if (!brace) {
        return NULL;
    }
    int depth = 0;
    const char *end = brace;
    int in_string = 0;
    int escaped = 0;
    for (; *end; end++) {
        char c = *end;
        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                end++;
                break;
            }
        }
    }
    if (depth != 0) {
        return NULL;
    }
    size_t len = (size_t)(end - brace);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, brace, len);
    out[len] = '\0';
    return out;
}

int profile_import_file(app_db *db, const char *path, const char *fallback_id)
{
    size_t len = 0;
    char *json = read_text_file(path, 1024 * 1024, &len);
    if (!json) {
        log_warn("profile file not found: %s", path);
        return -1;
    }
    char *id = json_get_string(json, "id");
    char *name = json_get_string(json, "name");
    if (!id) {
        id = xstrdup(fallback_id);
    }
    if (!name) {
        name = xstrdup(id ? id : "unknown");
    }
    int ok = (id && name) ? db_upsert_profile(db, id, name, json) : -1;
    if (ok == 0) {
        log_info("imported profile %s from %s (%zu bytes)", id, path, len);
    }
    free(id);
    free(name);
    free(json);
    return ok;
}

int profile_import_json(app_db *db, const char *json, char **out_id, char **out_name)
{
    if (out_id) {
        *out_id = NULL;
    }
    if (out_name) {
        *out_name = NULL;
    }
    if (!json || strlen(json) < 2) {
        return -1;
    }
    char *id = json_get_string(json, "id");
    char *name = json_get_string(json, "name");
    if (!id || !name || id[0] == '\0' || name[0] == '\0') {
        free(id);
        free(name);
        return -1;
    }
    int ok = db_upsert_profile(db, id, name, json);
    if (ok == 0) {
        if (out_id) {
            *out_id = xstrdup(id);
        }
        if (out_name) {
            *out_name = xstrdup(name);
        }
    }
    free(id);
    free(name);
    return ok;
}

char *profile_get_active_json(app_db *db, const app_config *cfg)
{
    return db_get_profile_json(db, cfg->active_profile);
}

char *profile_get_capability_command(const char *profile_json, const char *capability,
                                     const char *field)
{
    char *block = find_capability_block(profile_json, capability);
    if (!block) {
        return NULL;
    }
    char *value = json_get_string(block, field);
    if (!value && strcmp(field, "discovery.command") == 0) {
        char *discovery = find_capability_block(block, "discovery");
        if (discovery) {
            value = json_get_string(discovery, "command");
            free(discovery);
        }
    }
    free(block);
    return value;
}

int profile_get_capability_ttl(const char *profile_json, const char *capability, int fallback)
{
    char *block = find_capability_block(profile_json, capability);
    if (!block) {
        return fallback;
    }
    char *discovery = find_capability_block(block, "discovery");
    int ttl = fallback;
    if (discovery) {
        ttl = json_get_int(discovery, "cache_ttl_sec", fallback);
        free(discovery);
    }
    free(block);
    return ttl;
}
