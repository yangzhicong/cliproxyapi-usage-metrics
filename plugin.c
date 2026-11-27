#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ui_assets.h"

#if defined(_WIN32)
#define CLIPROXY_EXPORT __declspec(dllexport)
#else
#define CLIPROXY_EXPORT __attribute__((visibility("default")))
#endif

#define ABI_VERSION 1
#define PLUGIN_VERSION "1.0.10"
#define MAX_RECORDS 4000
#define MAX_AGG_BUCKETS 512
#define DEFAULT_STATE_PATH "plugins/usage-metrics.state.json"
#define SAVE_MIN_INTERVAL_SEC 5
#define SAVE_DIRTY_THRESHOLD 10

typedef struct {
    void* ptr;
    size_t len;
} cliproxy_buffer;

typedef int (*cliproxy_host_call_fn)(void*, const char*, const uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_host_free_fn)(void*, size_t);

typedef struct {
    uint32_t abi_version;
    void* host_ctx;
    cliproxy_host_call_fn call;
    cliproxy_host_free_fn free_buffer;
} cliproxy_host_api;

typedef int (*cliproxy_plugin_call_fn)(const char*, const uint8_t*, size_t, cliproxy_buffer*);
typedef void (*cliproxy_plugin_free_fn)(void*, size_t);
typedef void (*cliproxy_plugin_shutdown_fn)(void);

typedef struct {
    uint32_t abi_version;
    cliproxy_plugin_call_fn call;
    cliproxy_plugin_free_fn free_buffer;
    cliproxy_plugin_shutdown_fn shutdown;
} cliproxy_plugin_api;

/* ---------- String Builder ---------- */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} sb_t;

static void sb_init(sb_t* sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static bool sb_reserve(sb_t* sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return true;
    size_t ncap = sb->cap ? sb->cap * 2 : 256;
    while (ncap < sb->len + extra + 1) ncap *= 2;
    char* p = (char*)realloc(sb->data, ncap);
    if (!p) return false;
    sb->data = p;
    sb->cap = ncap;
    return true;
}

static void sb_append_mem(sb_t* sb, const char* s, size_t n) {
    if (!s || n == 0) return;
    if (!sb_reserve(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append_str(sb_t* sb, const char* s) {
    if (!s) return;
    sb_append_mem(sb, s, strlen(s));
}

static void sb_append_char(sb_t* sb, char c) {
    if (!sb_reserve(sb, 1)) return;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(sb_t* sb, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need <= 0) return;
    if (!sb_reserve(sb, (size_t)need)) return;
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)need;
}

static void sb_append_json_escaped(sb_t* sb, const char* s) {
    sb_append_char(sb, '"');
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            unsigned char c = *p;
            switch (c) {
                case '"':  sb_append_str(sb, "\\\""); break;
                case '\\': sb_append_str(sb, "\\\\"); break;
                case '\b': sb_append_str(sb, "\\b"); break;
                case '\f': sb_append_str(sb, "\\f"); break;
                case '\n': sb_append_str(sb, "\\n"); break;
                case '\r': sb_append_str(sb, "\\r"); break;
                case '\t': sb_append_str(sb, "\\t"); break;
                default:
                    if (c < 0x20) {
                        sb_appendf(sb, "\\u%04x", c);
                    } else {
                        sb_append_char(sb, (char)c);
                    }
                    break;
            }
        }
    }
    sb_append_char(sb, '"');
}

/* ---------- Base64 Encoder ---------- */

static char* base64_encode(const uint8_t* data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = tbl[(triple >> 6) & 0x3F];
        out[j++] = tbl[triple & 0x3F];
    }
    size_t mod = len % 3;
    if (mod == 1) {
        out[out_len - 1] = '=';
        out[out_len - 2] = '=';
    } else if (mod == 2) {
        out[out_len - 1] = '=';
    }
    out[out_len] = '\0';
    return out;
}

/* ---------- Minimal JSON Parser ---------- */

typedef enum {
    J_NULL = 0,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} jtype_t;

typedef struct jval jval_t;

typedef struct {
    char* key;
    jval_t* val;
} jpair_t;

struct jval {
    jtype_t type;
    bool bval;
    double dval;
    int64_t ival;
    char* sval;
    jval_t** items;
    size_t count;
    size_t cap;
    jpair_t* pairs;
};

static void j_free(jval_t* v) {
    if (!v) return;
    if (v->type == J_STR) {
        free(v->sval);
    } else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->count; i++) j_free(v->items[i]);
        free(v->items);
    } else if (v->type == J_OBJ) {
        for (size_t i = 0; i < v->count; i++) {
            free(v->pairs[i].key);
            j_free(v->pairs[i].val);
        }
        free(v->pairs);
    }
    free(v);
}

typedef struct {
    const char* s;
    size_t len;
    size_t pos;
    int depth;
} jparser_t;

static void jp_skip_ws(jparser_t* p) {
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
        else break;
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void sb_append_utf8(sb_t* sb, uint32_t cp) {
    if (cp <= 0x7F) {
        sb_append_char(sb, (char)cp);
    } else if (cp <= 0x7FF) {
        sb_append_char(sb, (char)(0xC0 | (cp >> 6)));
        sb_append_char(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        sb_append_char(sb, (char)(0xE0 | (cp >> 12)));
        sb_append_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_append_char(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        sb_append_char(sb, (char)(0xF0 | (cp >> 18)));
        sb_append_char(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_append_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_append_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static char* jp_parse_string_raw(jparser_t* p) {
    if (p->pos >= p->len || p->s[p->pos] != '"') return NULL;
    p->pos++;
    sb_t sb;
    sb_init(&sb);
    sb_reserve(&sb, 32);
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (c == '"') {
            return sb.data ? sb.data : strdup("");
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                free(sb.data);
                return NULL;
            }
            char esc = p->s[p->pos++];
            switch (esc) {
                case '"':  sb_append_char(&sb, '"'); break;
                case '\\': sb_append_char(&sb, '\\'); break;
                case '/':  sb_append_char(&sb, '/'); break;
                case 'b':  sb_append_char(&sb, '\b'); break;
                case 'f':  sb_append_char(&sb, '\f'); break;
                case 'n':  sb_append_char(&sb, '\n'); break;
                case 'r':  sb_append_char(&sb, '\r'); break;
                case 't':  sb_append_char(&sb, '\t'); break;
                case 'u': {
                    if (p->pos + 4 > p->len) { free(sb.data); return NULL; }
                    int h0 = hex_val(p->s[p->pos]);
                    int h1 = hex_val(p->s[p->pos + 1]);
                    int h2 = hex_val(p->s[p->pos + 2]);
                    int h3 = hex_val(p->s[p->pos + 3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { free(sb.data); return NULL; }
                    uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    p->pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len &&
                        p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                        int l0 = hex_val(p->s[p->pos + 2]);
                        int l1 = hex_val(p->s[p->pos + 3]);
                        int l2 = hex_val(p->s[p->pos + 4]);
                        int l3 = hex_val(p->s[p->pos + 5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t low = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                p->pos += 6;
                            }
                        }
                    }
                    sb_append_utf8(&sb, cp);
                    break;
                }
                default:
                    sb_append_char(&sb, esc);
                    break;
            }
        } else {
            sb_append_char(&sb, c);
        }
    }
    free(sb.data);
    return NULL;
}

static jval_t* jp_parse_value(jparser_t* p);

static jval_t* jp_parse_array(jparser_t* p) {
    p->pos++; /* '[' */
    jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
    if (!v) return NULL;
    v->type = J_ARR;
    jp_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        return v;
    }
    while (p->pos < p->len) {
        jp_skip_ws(p);
        jval_t* elem = jp_parse_value(p);
        if (!elem) { j_free(v); return NULL; }
        if (v->count == v->cap) {
            size_t ncap = v->cap ? v->cap * 2 : 8;
            jval_t** ni = (jval_t**)realloc(v->items, ncap * sizeof(jval_t*));
            if (!ni) { j_free(elem); j_free(v); return NULL; }
            v->items = ni;
            v->cap = ncap;
        }
        v->items[v->count++] = elem;
        jp_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
        } else if (p->pos < p->len && p->s[p->pos] == ']') {
            p->pos++;
            return v;
        } else {
            j_free(v);
            return NULL;
        }
    }
    j_free(v);
    return NULL;
}

static jval_t* jp_parse_object(jparser_t* p) {
    p->pos++; /* '{' */
    jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
    if (!v) return NULL;
    v->type = J_OBJ;
    jp_skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        return v;
    }
    while (p->pos < p->len) {
        jp_skip_ws(p);
        char* key = jp_parse_string_raw(p);
        if (!key) { j_free(v); return NULL; }
        jp_skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { free(key); j_free(v); return NULL; }
        p->pos++;
        jp_skip_ws(p);
        jval_t* val = jp_parse_value(p);
        if (!val) { free(key); j_free(v); return NULL; }
        if (v->count == v->cap) {
            size_t ncap = v->cap ? v->cap * 2 : 8;
            jpair_t* np = (jpair_t*)realloc(v->pairs, ncap * sizeof(jpair_t));
            if (!np) { free(key); j_free(val); j_free(v); return NULL; }
            v->pairs = np;
            v->cap = ncap;
        }
        v->pairs[v->count].key = key;
        v->pairs[v->count].val = val;
        v->count++;
        jp_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
        } else if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            return v;
        } else {
            j_free(v);
            return NULL;
        }
    }
    j_free(v);
    return NULL;
}

static jval_t* jp_parse_number(jparser_t* p) {
    size_t start = p->pos;
    if (p->s[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    bool is_float = false;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        is_float = true;
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_float = true;
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    size_t n = p->pos - start;
    if (n == 0 || n >= 64) return NULL;
    char buf[64];
    memcpy(buf, p->s + start, n);
    buf[n] = '\0';
    jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
    if (!v) return NULL;
    v->type = J_NUM;
    v->dval = strtod(buf, NULL);
    if (is_float) {
        v->ival = (int64_t)v->dval;
    } else {
        v->ival = strtoll(buf, NULL, 10);
    }
    return v;
}

static jval_t* jp_parse_value(jparser_t* p) {
    if (p->depth > 64) return NULL;
    jp_skip_ws(p);
    if (p->pos >= p->len) return NULL;
    char c = p->s[p->pos];
    if (c == '{') {
        p->depth++;
        jval_t* r = jp_parse_object(p);
        p->depth--;
        return r;
    }
    if (c == '[') {
        p->depth++;
        jval_t* r = jp_parse_array(p);
        p->depth--;
        return r;
    }
    if (c == '"') {
        char* str = jp_parse_string_raw(p);
        if (!str) return NULL;
        jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
        if (!v) { free(str); return NULL; }
        v->type = J_STR;
        v->sval = str;
        return v;
    }
    if (c == 't' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) {
        p->pos += 4;
        jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
        if (!v) return NULL;
        v->type = J_BOOL;
        v->bval = true;
        return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) {
        p->pos += 5;
        jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
        if (!v) return NULL;
        v->type = J_BOOL;
        v->bval = false;
        return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0) {
        p->pos += 4;
        jval_t* v = (jval_t*)calloc(1, sizeof(jval_t));
        if (!v) return NULL;
        v->type = J_NULL;
        return v;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        return jp_parse_number(p);
    }
    return NULL;
}

static jval_t* j_parse(const char* s, size_t len) {
    if (!s || len == 0) return NULL;
    jparser_t p = { s, len, 0, 0 };
    return jp_parse_value(&p);
}

static const jval_t* j_obj_get(const jval_t* obj, const char* key) {
    if (!obj || obj->type != J_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->pairs[i].key, key) == 0) return obj->pairs[i].val;
    }
    return NULL;
}

static const jval_t* j_obj_get_ci(const jval_t* obj, const char* key) {
    if (!obj || obj->type != J_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcasecmp(obj->pairs[i].key, key) == 0) return obj->pairs[i].val;
    }
    return NULL;
}

static const char* j_obj_get_str(const jval_t* obj, const char* key) {
    const jval_t* v = j_obj_get(obj, key);
    if (!v) v = j_obj_get_ci(obj, key);
    return (v && v->type == J_STR) ? v->sval : NULL;
}

static int64_t j_obj_get_i64(const jval_t* obj, const char* key, int64_t defv) {
    const jval_t* v = j_obj_get(obj, key);
    if (!v) v = j_obj_get_ci(obj, key);
    if (!v || v->type != J_NUM) return defv;
    return v->ival;
}

static bool j_obj_get_bool(const jval_t* obj, const char* key, bool defv) {
    const jval_t* v = j_obj_get(obj, key);
    if (!v) v = j_obj_get_ci(obj, key);
    if (!v || v->type != J_BOOL) return defv;
    return v->bval;
}

static const char* j_query_get(const jval_t* query, const char* key) {
    const jval_t* v = j_obj_get(query, key);
    if (!v) v = j_obj_get_ci(query, key);
    if (!v) return NULL;
    if (v->type == J_STR) return v->sval;
    if (v->type == J_ARR && v->count > 0 && v->items[0]->type == J_STR) {
        return v->items[0]->sval;
    }
    return NULL;
}

/* ---------- Data Structures ---------- */

typedef struct {
    int64_t ts;
    char model[96];
    char provider[48];
    char auth_id[96];
    uint8_t stream;
    uint8_t failed;
    int32_t status_code;
    int64_t input_tokens;         /* normalized effective total input tokens */
    int64_t output_tokens;
    int64_t reasoning_tokens;
    int64_t cache_read_tokens;
    int64_t cache_creation_tokens;
    int64_t total_tokens;
    int64_t ttft_ms;              /* >0 when reported */
    int64_t latency_ms;
} metric_record_t;

typedef struct {
    int64_t requests;
    int64_t failed;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t reasoning_tokens;
    int64_t cache_read_tokens;
    int64_t cache_creation_tokens;
    int64_t total_tokens;
} totals_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static metric_record_t g_records[MAX_RECORDS];
static size_t g_head = 0;   /* index of oldest record */
static size_t g_count = 0;  /* number of valid records */
static totals_t g_all_time = {0};
static bool g_loaded = false;
static time_t g_last_save = 0;
static int g_dirty_since_save = 0;

static const char* get_state_path(void) {
    const char* env_p = getenv("USAGE_METRICS_STATE_PATH");
    if (env_p && env_p[0] != '\0') return env_p;
    return DEFAULT_STATE_PATH;
}

static void copy_str(char* dst, size_t cap, const char* src, const char* fallback) {
    const char* s = (src && src[0] != '\0') ? src : (fallback ? fallback : "");
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static bool contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    return strcasestr(hay, needle) != NULL;
}

/* Parse RFC3339 timestamp e.g. 2026-11-26T18:50:12.123456Z or +08:00 */
static int64_t parse_rfc3339_ts(const char* s) {
    if (!s || strlen(s) < 19) return (int64_t)time(NULL);
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) {
        return (int64_t)time(NULL);
    }
    if (Y < 2000 || M < 1 || M > 12 || D < 1 || D > 31) return (int64_t)time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = m;
    tmv.tm_sec = sec;
    time_t utc = timegm(&tmv);
    if (utc == (time_t)-1) return (int64_t)time(NULL);

    const char* tz = s + 19;
    while (*tz == '.' || isdigit((unsigned char)*tz)) tz++;
    if (*tz == '+' || *tz == '-') {
        int sign = (*tz == '+') ? 1 : -1;
        int tzh = 0, tzm = 0;
        if (sscanf(tz + 1, "%2d:%2d", &tzh, &tzm) == 2) {
            utc -= sign * (tzh * 3600 + tzm * 60);
        }
    }
    return (int64_t)utc;
}

static int64_t ns_to_ms(int64_t ns) {
    if (ns <= 0) return 0;
    int64_t ms = (ns + 500000) / 1000000;
    return ms > 0 ? ms : 1;
}

static double calc_hit_rate(int64_t cache_read, int64_t eff_input) {
    if (eff_input <= 0 || cache_read <= 0) return 0.0;
    double r = ((double)cache_read * 100.0) / (double)eff_input;
    if (r > 100.0) r = 100.0;
    if (r < 0.0) r = 0.0;
    return r;
}

static void push_record_locked(const metric_record_t* rec, bool update_all_time) {
    size_t idx;
    if (g_count < MAX_RECORDS) {
        idx = (g_head + g_count) % MAX_RECORDS;
        g_count++;
    } else {
        idx = g_head;
        g_head = (g_head + 1) % MAX_RECORDS;
    }
    g_records[idx] = *rec;
    if (update_all_time) {
        g_all_time.requests++;
        if (rec->failed) g_all_time.failed++;
        g_all_time.input_tokens += rec->input_tokens;
        g_all_time.output_tokens += rec->output_tokens;
        g_all_time.reasoning_tokens += rec->reasoning_tokens;
        g_all_time.cache_read_tokens += rec->cache_read_tokens;
        g_all_time.cache_creation_tokens += rec->cache_creation_tokens;
        g_all_time.total_tokens += rec->total_tokens;
    }
}

/* ---------- Persistence ---------- */

static void save_state_locked(void) {
    const char* path = get_state_path();
    char tmp_path[512];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)time(NULL));
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) return;

    sb_t sb;
    sb_init(&sb);
    sb_reserve(&sb, 4096 + g_count * 160);
    sb_appendf(&sb,
        "{\"version\":1,\"all_time\":{\"requests\":%lld,\"failed\":%lld,"
        "\"input_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld,"
        "\"cache_read_tokens\":%lld,\"cache_creation_tokens\":%lld,\"total_tokens\":%lld},"
        "\"records\":[",
        (long long)g_all_time.requests, (long long)g_all_time.failed,
        (long long)g_all_time.input_tokens, (long long)g_all_time.output_tokens,
        (long long)g_all_time.reasoning_tokens, (long long)g_all_time.cache_read_tokens,
        (long long)g_all_time.cache_creation_tokens, (long long)g_all_time.total_tokens);

    for (size_t i = 0; i < g_count; i++) {
        const metric_record_t* r = &g_records[(g_head + i) % MAX_RECORDS];
        if (i > 0) sb_append_char(&sb, ',');
        sb_appendf(&sb, "{\"ts\":%lld,\"model\":", (long long)r->ts);
        sb_append_json_escaped(&sb, r->model);
        sb_append_str(&sb, ",\"provider\":");
        sb_append_json_escaped(&sb, r->provider);
        sb_append_str(&sb, ",\"auth_id\":");
        sb_append_json_escaped(&sb, r->auth_id);
        sb_appendf(&sb,
            ",\"stream\":%s,\"failed\":%s,\"status_code\":%d,"
            "\"input_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld,"
            "\"cache_read_tokens\":%lld,\"cache_creation_tokens\":%lld,"
            "\"total_tokens\":%lld,\"ttft_ms\":%lld,\"latency_ms\":%lld}",
            r->stream ? "true" : "false",
            r->failed ? "true" : "false",
            (int)r->status_code,
            (long long)r->input_tokens, (long long)r->output_tokens,
            (long long)r->reasoning_tokens, (long long)r->cache_read_tokens,
            (long long)r->cache_creation_tokens, (long long)r->total_tokens,
            (long long)r->ttft_ms, (long long)r->latency_ms);
    }
    sb_append_str(&sb, "]}");

    if (!sb.data) return;
    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) {
        free(sb.data);
        return;
    }
    size_t written = fwrite(sb.data, 1, sb.len, fp);
    int rc = fclose(fp);
    free(sb.data);
    if (written == sb.len && rc == 0) {
        if (rename(tmp_path, path) == 0) {
            g_last_save = time(NULL);
            g_dirty_since_save = 0;
            return;
        }
    }
    remove(tmp_path);
}

static void load_state_locked(void) {
    if (g_loaded) return;
    g_loaded = true;
    g_head = 0;
    g_count = 0;
    memset(&g_all_time, 0, sizeof(g_all_time));
    const char* path = get_state_path();
    FILE* fp = fopen(path, "rb");
    if (!fp) return;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return; }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 32 * 1024 * 1024) { fclose(fp); return; }
    rewind(fp);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    jval_t* root = j_parse(buf, rd);
    free(buf);
    if (!root || root->type != J_OBJ) {
        j_free(root);
        return;
    }

    const jval_t* at = j_obj_get(root, "all_time");
    if (at && at->type == J_OBJ) {
        g_all_time.requests = j_obj_get_i64(at, "requests", 0);
        g_all_time.failed = j_obj_get_i64(at, "failed", 0);
        g_all_time.input_tokens = j_obj_get_i64(at, "input_tokens", 0);
        g_all_time.output_tokens = j_obj_get_i64(at, "output_tokens", 0);
        g_all_time.reasoning_tokens = j_obj_get_i64(at, "reasoning_tokens", 0);
        g_all_time.cache_read_tokens = j_obj_get_i64(at, "cache_read_tokens", 0);
        g_all_time.cache_creation_tokens = j_obj_get_i64(at, "cache_creation_tokens", 0);
        g_all_time.total_tokens = j_obj_get_i64(at, "total_tokens", 0);
    }

    const jval_t* arr = j_obj_get(root, "records");
    if (arr && arr->type == J_ARR) {
        for (size_t i = 0; i < arr->count; i++) {
            const jval_t* item = arr->items[i];
            if (!item || item->type != J_OBJ) continue;
            metric_record_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.ts = j_obj_get_i64(item, "ts", 0);
            copy_str(rec.model, sizeof(rec.model), j_obj_get_str(item, "model"), "unknown");
            copy_str(rec.provider, sizeof(rec.provider), j_obj_get_str(item, "provider"), "");
            copy_str(rec.auth_id, sizeof(rec.auth_id), j_obj_get_str(item, "auth_id"), "default");
            rec.stream = j_obj_get_bool(item, "stream", false) ? 1 : 0;
            rec.failed = j_obj_get_bool(item, "failed", false) ? 1 : 0;
            rec.status_code = (int32_t)j_obj_get_i64(item, "status_code", 0);
            rec.input_tokens = j_obj_get_i64(item, "input_tokens", 0);
            rec.output_tokens = j_obj_get_i64(item, "output_tokens", 0);
            rec.reasoning_tokens = j_obj_get_i64(item, "reasoning_tokens", 0);
            rec.cache_read_tokens = j_obj_get_i64(item, "cache_read_tokens", 0);
            rec.cache_creation_tokens = j_obj_get_i64(item, "cache_creation_tokens", 0);
            rec.total_tokens = j_obj_get_i64(item, "total_tokens", 0);
            rec.ttft_ms = j_obj_get_i64(item, "ttft_ms", 0);
            rec.latency_ms = j_obj_get_i64(item, "latency_ms", 0);
            push_record_locked(&rec, false);
        }
    }
    j_free(root);
    g_last_save = time(NULL);
}

/* ---------- Aggregation & Percentiles ---------- */

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a;
    int64_t y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

static int64_t percentile_sorted(const int64_t* arr, size_t n, double p) {
    if (!arr || n == 0) return 0;
    size_t idx = (size_t)((double)(n - 1) * p + 0.5);
    if (idx >= n) idx = n - 1;
    return arr[idx];
}

typedef struct {
    char key[96];
    char provider[48];
    int64_t requests;
    int64_t failed;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t reasoning_tokens;
    int64_t cache_read_tokens;
    int64_t cache_creation_tokens;
    int64_t total_tokens;
    int64_t ttft_sum;
    size_t ttft_count;
    int64_t* ttft_vals;
    size_t ttft_cap;
    int64_t lat_sum;
    size_t lat_count;
    int64_t gen_tok_sum;
    int64_t gen_ms_sum;
} agg_bucket_t;

static void agg_push_ttft(agg_bucket_t* b, int64_t ms) {
    if (ms <= 0) return;
    if (b->ttft_count == b->ttft_cap) {
        size_t ncap = b->ttft_cap ? b->ttft_cap * 2 : 16;
        int64_t* p = (int64_t*)realloc(b->ttft_vals, ncap * sizeof(int64_t));
        if (!p) return;
        b->ttft_vals = p;
        b->ttft_cap = ncap;
    }
    b->ttft_vals[b->ttft_count++] = ms;
    b->ttft_sum += ms;
}

static int cmp_bucket_tokens_desc(const void* a, const void* b) {
    const agg_bucket_t* x = (const agg_bucket_t*)a;
    const agg_bucket_t* y = (const agg_bucket_t*)b;
    if (y->total_tokens != x->total_tokens) return (y->total_tokens > x->total_tokens) ? 1 : -1;
    if (y->requests != x->requests) return (y->requests > x->requests) ? 1 : -1;
    return strcmp(x->key, y->key);
}

static int cmp_bucket_reqs_desc(const void* a, const void* b) {
    const agg_bucket_t* x = (const agg_bucket_t*)a;
    const agg_bucket_t* y = (const agg_bucket_t*)b;
    if (y->requests != x->requests) return (y->requests > x->requests) ? 1 : -1;
    if (y->total_tokens != x->total_tokens) return (y->total_tokens > x->total_tokens) ? 1 : -1;
    return strcmp(x->key, y->key);
}

static agg_bucket_t* find_or_add_bucket(agg_bucket_t* arr, size_t* n, const char* key, const char* provider) {
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(arr[i].key, key) == 0) return &arr[i];
    }
    if (*n >= MAX_AGG_BUCKETS) return NULL;
    agg_bucket_t* b = &arr[(*n)++];
    memset(b, 0, sizeof(*b));
    copy_str(b->key, sizeof(b->key), key, "unknown");
    copy_str(b->provider, sizeof(b->provider), provider, "");
    return b;
}

static int64_t compute_cutoff_ts(const char* range) {
    if (!range || strcmp(range, "all") == 0) return 0;
    time_t now = time(NULL);
    if (strcmp(range, "7d") == 0) {
        return (int64_t)(now - 7 * 86400);
    }
    /* default: today (local midnight) */
    struct tm lt;
    localtime_r(&now, &lt);
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    return (int64_t)mktime(&lt);
}

/* ---------- Usage Handler ---------- */

static void handle_usage_record(const uint8_t* req, size_t req_len) {
    jval_t* root = j_parse((const char*)req, req_len);
    if (!root || root->type != J_OBJ) {
        j_free(root);
        return;
    }

    metric_record_t rec;
    memset(&rec, 0, sizeof(rec));

    const char* req_at = j_obj_get_str(root, "RequestedAt");
    rec.ts = parse_rfc3339_ts(req_at);

    const char* model = j_obj_get_str(root, "Model");
    if (!model || !model[0]) model = j_obj_get_str(root, "ResponseModel");
    copy_str(rec.model, sizeof(rec.model), model, "unknown");

    const char* provider = j_obj_get_str(root, "Provider");
    const char* executor = j_obj_get_str(root, "ExecutorType");
    copy_str(rec.provider, sizeof(rec.provider), provider, executor ? executor : "");

    const char* auth_id = j_obj_get_str(root, "AuthID");
    if (!auth_id || !auth_id[0]) auth_id = j_obj_get_str(root, "Source");
    copy_str(rec.auth_id, sizeof(rec.auth_id), auth_id, "default");

    rec.stream = j_obj_get_bool(root, "Stream", false) ? 1 : 0;
    rec.failed = j_obj_get_bool(root, "Failed", false) ? 1 : 0;
    rec.ttft_ms = ns_to_ms(j_obj_get_i64(root, "TTFT", 0));
    rec.latency_ms = ns_to_ms(j_obj_get_i64(root, "Latency", 0));

    const jval_t* failure = j_obj_get(root, "Failure");
    if (failure && failure->type == J_OBJ) {
        rec.status_code = (int32_t)j_obj_get_i64(failure, "StatusCode", 0);
    }

    const jval_t* detail = j_obj_get(root, "Detail");
    if (detail && detail->type == J_OBJ) {
        int64_t raw_in = j_obj_get_i64(detail, "InputTokens", 0);
        int64_t raw_out = j_obj_get_i64(detail, "OutputTokens", 0);
        int64_t raw_reason = j_obj_get_i64(detail, "ReasoningTokens", 0);
        int64_t raw_cached = j_obj_get_i64(detail, "CachedTokens", 0);
        int64_t raw_cread = j_obj_get_i64(detail, "CacheReadTokens", 0);
        int64_t raw_cwrite = j_obj_get_i64(detail, "CacheCreationTokens", 0);
        int64_t raw_total = j_obj_get_i64(detail, "TotalTokens", 0);

        if (raw_in < 0) raw_in = 0;
        if (raw_out < 0) raw_out = 0;
        if (raw_reason < 0) raw_reason = 0;
        if (raw_cached < 0) raw_cached = 0;
        if (raw_cread < 0) raw_cread = 0;
        if (raw_cwrite < 0) raw_cwrite = 0;
        if (raw_total < 0) raw_total = 0;

        int64_t cache_read = raw_cread > 0 ? raw_cread : raw_cached;

        /* Match CPA sdk/cliproxy/usage/accounting.go semantics */
        bool is_native_claude =
            (contains_ci(provider, "claude") || contains_ci(provider, "anthropic") ||
             contains_ci(executor, "claude") || contains_ci(executor, "anthropic")) &&
            !contains_ci(provider, "antigravity") && !contains_ci(provider, "gemini") &&
            !contains_ci(provider, "vertex") && !contains_ci(executor, "antigravity");

        int64_t eff_in = raw_in;
        if (is_native_claude) {
            eff_in = raw_in + cache_read + raw_cwrite;
        } else {
            int64_t cache_sum = cache_read + raw_cwrite;
            if (cache_sum > eff_in) eff_in = cache_sum;
            if (raw_cached > eff_in) eff_in = raw_cached;
        }

        rec.input_tokens = eff_in;
        rec.output_tokens = raw_out;
        rec.reasoning_tokens = raw_reason;
        rec.cache_read_tokens = cache_read;
        rec.cache_creation_tokens = raw_cwrite;
        rec.total_tokens = raw_total > 0 ? raw_total : (eff_in + raw_out);
    }

    j_free(root);

    pthread_mutex_lock(&g_mu);
    load_state_locked();
    push_record_locked(&rec, true);
    g_dirty_since_save++;
    time_t now = time(NULL);
    if (g_dirty_since_save >= SAVE_DIRTY_THRESHOLD || (now - g_last_save) >= SAVE_MIN_INTERVAL_SEC) {
        save_state_locked();
    }
    pthread_mutex_unlock(&g_mu);
}

/* ---------- Management API Responses ---------- */

static char* make_mgmt_response_b64(int status, const char* content_type, const char* body_b64) {
    sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb,
        "{\"ok\":true,\"result\":{\"StatusCode\":%d,\"Headers\":{\"Content-Type\":[", status);
    sb_append_json_escaped(&sb, content_type ? content_type : "application/json; charset=utf-8");
    sb_append_str(&sb, "],\"Cache-Control\":[\"no-store\"]},\"Body\":");
    sb_append_json_escaped(&sb, body_b64 ? body_b64 : "");
    sb_append_str(&sb, "}}");
    return sb.data;
}

static char* make_mgmt_response_raw(int status, const char* content_type, const char* body, size_t len) {
    char* b64 = base64_encode((const uint8_t*)(body ? body : ""), len);
    if (!b64) return strdup("{\"ok\":false,\"error\":{\"code\":\"internal\",\"message\":\"oom\"}}");
    char* resp = make_mgmt_response_b64(status, content_type, b64);
    free(b64);
    return resp;
}

static int64_t calc_gen_ms(const metric_record_t* r) {
    if (!r || r->failed || r->latency_ms <= 0) return 0;
    if (r->stream && r->ttft_ms > 0 && r->latency_ms > r->ttft_ms) {
        return r->latency_ms - r->ttft_ms;
    }
    return r->latency_ms;
}

static char* build_summary_json_locked(const char* range, const char* filter_model, const char* filter_auth, int limit) {
    int64_t cutoff = compute_cutoff_ts(range);
    if (limit <= 0) limit = 200;
    if (limit > 500) limit = 500;

    int64_t tot_reqs = 0, tot_fail = 0, tot_stream = 0, tot_cache_hit_reqs = 0;
    int64_t tot_in = 0, tot_out = 0, tot_reason = 0, tot_cread = 0, tot_cwrite = 0, tot_tokens = 0;
    int64_t ttft_sum = 0, lat_sum = 0, gen_tok_sum = 0, gen_ms_sum = 0;

    int64_t* ttft_all = (int64_t*)malloc((g_count + 1) * sizeof(int64_t));
    int64_t* lat_all = (int64_t*)malloc((g_count + 1) * sizeof(int64_t));
    size_t ttft_n = 0, lat_n = 0;

    agg_bucket_t* m_buckets = (agg_bucket_t*)calloc(MAX_AGG_BUCKETS, sizeof(agg_bucket_t));
    agg_bucket_t* a_buckets = (agg_bucket_t*)calloc(MAX_AGG_BUCKETS, sizeof(agg_bucket_t));
    size_t m_count = 0, a_count = 0;

    if (!ttft_all || !lat_all || !m_buckets || !a_buckets) {
        free(ttft_all); free(lat_all); free(m_buckets); free(a_buckets);
        return strdup("{\"error\":\"oom\"}");
    }

    for (size_t i = 0; i < g_count; i++) {
        const metric_record_t* r = &g_records[(g_head + i) % MAX_RECORDS];
        if (cutoff > 0 && r->ts < cutoff) continue;

        /* Model & Auth buckets always reflect full time range (so filter dropdown stays populated) */
        agg_bucket_t* mb = find_or_add_bucket(m_buckets, &m_count, r->model, r->provider);
        if (mb) {
            mb->requests++;
            if (r->failed) mb->failed++;
            mb->input_tokens += r->input_tokens;
            mb->output_tokens += r->output_tokens;
            mb->reasoning_tokens += r->reasoning_tokens;
            mb->cache_read_tokens += r->cache_read_tokens;
            mb->cache_creation_tokens += r->cache_creation_tokens;
            mb->total_tokens += r->total_tokens;
            agg_push_ttft(mb, r->ttft_ms);
            if (r->latency_ms > 0) { mb->lat_sum += r->latency_ms; mb->lat_count++; }
            if (!r->failed) {
                int64_t gt = r->output_tokens + r->reasoning_tokens;
                int64_t gm = calc_gen_ms(r);
                if (gt > 0 && gm > 0) { mb->gen_tok_sum += gt; mb->gen_ms_sum += gm; }
            }
        }

        agg_bucket_t* ab = find_or_add_bucket(a_buckets, &a_count, r->auth_id, r->provider);
        if (ab) {
            ab->requests++;
            if (r->failed) ab->failed++;
            ab->input_tokens += r->input_tokens;
            ab->output_tokens += r->output_tokens;
            ab->reasoning_tokens += r->reasoning_tokens;
            ab->cache_read_tokens += r->cache_read_tokens;
            ab->cache_creation_tokens += r->cache_creation_tokens;
            ab->total_tokens += r->total_tokens;
            agg_push_ttft(ab, r->ttft_ms);
            if (r->latency_ms > 0) { ab->lat_sum += r->latency_ms; ab->lat_count++; }
            if (!r->failed) {
                int64_t gt = r->output_tokens + r->reasoning_tokens;
                int64_t gm = calc_gen_ms(r);
                if (gt > 0 && gm > 0) { ab->gen_tok_sum += gt; ab->gen_ms_sum += gm; }
            }
        }

        if (filter_model && filter_model[0] && strcmp(r->model, filter_model) != 0) continue;
        if (filter_auth && filter_auth[0] && strcmp(r->auth_id, filter_auth) != 0) continue;

        tot_reqs++;
        if (r->failed) tot_fail++;
        if (r->stream) tot_stream++;
        if (r->cache_read_tokens > 0) tot_cache_hit_reqs++;
        tot_in += r->input_tokens;
        tot_out += r->output_tokens;
        tot_reason += r->reasoning_tokens;
        tot_cread += r->cache_read_tokens;
        tot_cwrite += r->cache_creation_tokens;
        tot_tokens += r->total_tokens;
        if (r->ttft_ms > 0) {
            ttft_all[ttft_n++] = r->ttft_ms;
            ttft_sum += r->ttft_ms;
        }
        if (r->latency_ms > 0) {
            lat_all[lat_n++] = r->latency_ms;
            lat_sum += r->latency_ms;
        }
        if (!r->failed) {
            int64_t gt = r->output_tokens + r->reasoning_tokens;
            int64_t gm = calc_gen_ms(r);
            if (gt > 0 && gm > 0) { gen_tok_sum += gt; gen_ms_sum += gm; }
        }
    }

    if (ttft_n > 1) qsort(ttft_all, ttft_n, sizeof(int64_t), cmp_i64);
    if (lat_n > 1) qsort(lat_all, lat_n, sizeof(int64_t), cmp_i64);
    if (m_count > 1) qsort(m_buckets, m_count, sizeof(agg_bucket_t), cmp_bucket_tokens_desc);
    if (a_count > 1) qsort(a_buckets, a_count, sizeof(agg_bucket_t), cmp_bucket_reqs_desc);

    sb_t sb;
    sb_init(&sb);
    sb_reserve(&sb, 8192);

    sb_appendf(&sb,
        "{\"version\":\"%s\",\"range\":", PLUGIN_VERSION);
    sb_append_json_escaped(&sb, (range && range[0]) ? range : "today");
    sb_appendf(&sb,
        ",\"window_count\":%zu,\"window_cap\":%d,"
        "\"overview\":{"
        "\"requests\":%lld,\"failed\":%lld,\"stream_requests\":%lld,\"cache_hit_requests\":%lld,"
        "\"input_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld,"
        "\"cache_read_tokens\":%lld,\"cache_creation_tokens\":%lld,\"total_tokens\":%lld,"
        "\"cache_hit_rate\":%.2f,\"tps_avg\":%.1f,"
        "\"ttft_samples\":%zu,\"ttft_avg_ms\":%lld,\"ttft_p50_ms\":%lld,\"ttft_p95_ms\":%lld,"
        "\"ttft_min_ms\":%lld,\"ttft_max_ms\":%lld,"
        "\"latency_avg_ms\":%lld,\"latency_p50_ms\":%lld,\"latency_p95_ms\":%lld,\"latency_max_ms\":%lld},"
        "\"all_time\":{"
        "\"requests\":%lld,\"failed\":%lld,"
        "\"input_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld,"
        "\"cache_read_tokens\":%lld,\"cache_creation_tokens\":%lld,\"total_tokens\":%lld,"
        "\"cache_hit_rate\":%.2f},\"by_model\":[",
        g_count, MAX_RECORDS,
        (long long)tot_reqs, (long long)tot_fail, (long long)tot_stream, (long long)tot_cache_hit_reqs,
        (long long)tot_in, (long long)tot_out, (long long)tot_reason,
        (long long)tot_cread, (long long)tot_cwrite, (long long)tot_tokens,
        calc_hit_rate(tot_cread, tot_in),
        (gen_ms_sum > 0 ? ((double)gen_tok_sum * 1000.0 / (double)gen_ms_sum) : 0.0),
        ttft_n,
        (long long)(ttft_n ? (ttft_sum / (int64_t)ttft_n) : 0),
        (long long)percentile_sorted(ttft_all, ttft_n, 0.50),
        (long long)percentile_sorted(ttft_all, ttft_n, 0.95),
        (long long)(ttft_n ? ttft_all[0] : 0),
        (long long)(ttft_n ? ttft_all[ttft_n - 1] : 0),
        (long long)(lat_n ? (lat_sum / (int64_t)lat_n) : 0),
        (long long)percentile_sorted(lat_all, lat_n, 0.50),
        (long long)percentile_sorted(lat_all, lat_n, 0.95),
        (long long)(lat_n ? lat_all[lat_n - 1] : 0),
        (long long)g_all_time.requests, (long long)g_all_time.failed,
        (long long)g_all_time.input_tokens, (long long)g_all_time.output_tokens,
        (long long)g_all_time.reasoning_tokens, (long long)g_all_time.cache_read_tokens,
        (long long)g_all_time.cache_creation_tokens, (long long)g_all_time.total_tokens,
        calc_hit_rate(g_all_time.cache_read_tokens, g_all_time.input_tokens));

    for (size_t i = 0; i < m_count; i++) {
        agg_bucket_t* b = &m_buckets[i];
        if (b->ttft_count > 1) qsort(b->ttft_vals, b->ttft_count, sizeof(int64_t), cmp_i64);
        if (i > 0) sb_append_char(&sb, ',');
        sb_append_str(&sb, "{\"model\":");
        sb_append_json_escaped(&sb, b->key);
        sb_appendf(&sb,
            ",\"requests\":%lld,\"failed\":%lld,"
            "\"input_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld,"
            "\"cache_read_tokens\":%lld,\"total_tokens\":%lld,\"cache_hit_rate\":%.2f,"
            "\"tps_avg\":%.1f,\"ttft_avg_ms\":%lld,\"ttft_p95_ms\":%lld,\"latency_avg_ms\":%lld}",
            (long long)b->requests, (long long)b->failed,
            (long long)b->input_tokens, (long long)b->output_tokens,
            (long long)b->reasoning_tokens, (long long)b->cache_read_tokens,
            (long long)b->total_tokens, calc_hit_rate(b->cache_read_tokens, b->input_tokens),
            (b->gen_ms_sum > 0 ? ((double)b->gen_tok_sum * 1000.0 / (double)b->gen_ms_sum) : 0.0),
            (long long)(b->ttft_count ? (b->ttft_sum / (int64_t)b->ttft_count) : 0),
            (long long)percentile_sorted(b->ttft_vals, b->ttft_count, 0.95),
            (long long)(b->lat_count ? (b->lat_sum / (int64_t)b->lat_count) : 0));
    }

    sb_append_str(&sb, "],\"by_auth\":[");
    for (size_t i = 0; i < a_count; i++) {
        agg_bucket_t* b = &a_buckets[i];
        if (i > 0) sb_append_char(&sb, ',');
        sb_append_str(&sb, "{\"auth_id\":");
        sb_append_json_escaped(&sb, b->key);
        sb_append_str(&sb, ",\"provider\":");
        sb_append_json_escaped(&sb, b->provider);
        sb_appendf(&sb,
            ",\"requests\":%lld,\"failed\":%lld,"
            "\"input_tokens\":%lld,\"cache_read_tokens\":%lld,"
            "\"total_tokens\":%lld,\"cache_hit_rate\":%.2f,"
            "\"ttft_avg_ms\":%lld,\"latency_avg_ms\":%lld}",
            (long long)b->requests, (long long)b->failed,
            (long long)b->input_tokens, (long long)b->cache_read_tokens,
            (long long)b->total_tokens, calc_hit_rate(b->cache_read_tokens, b->input_tokens),
            (long long)(b->ttft_count ? (b->ttft_sum / (int64_t)b->ttft_count) : 0),
            (long long)(b->lat_count ? (b->lat_sum / (int64_t)b->lat_count) : 0));
    }

    sb_append_str(&sb, "],\"recent\":[");
    int emitted = 0;
    for (size_t k = 0; k < g_count && emitted < limit; k++) {
        size_t idx = (g_head + g_count - 1 - k) % MAX_RECORDS;
        const metric_record_t* r = &g_records[idx];
        if (cutoff > 0 && r->ts < cutoff) continue;
        if (filter_model && filter_model[0] && strcmp(r->model, filter_model) != 0) continue;
        if (filter_auth && filter_auth[0] && strcmp(r->auth_id, filter_auth) != 0) continue;
        if (emitted > 0) sb_append_char(&sb, ',');
        sb_appendf(&sb, "{\"ts\":%lld,\"model\":", (long long)r->ts);
        sb_append_json_escaped(&sb, r->model);
        sb_append_str(&sb, ",\"provider\":");
        sb_append_json_escaped(&sb, r->provider);
        sb_append_str(&sb, ",\"auth_id\":");
        sb_append_json_escaped(&sb, r->auth_id);
        sb_appendf(&sb,
            ",\"stream\":%s,\"failed\":%s,\"status_code\":%d,"
            "\"input_tokens\":%lld,\"output_tokens\":%lld,\"reasoning_tokens\":%lld,"
            "\"cache_read_tokens\":%lld,\"cache_creation_tokens\":%lld,"
            "\"total_tokens\":%lld,\"cache_hit_rate\":%.2f,"
            "\"ttft_ms\":%lld,\"latency_ms\":%lld}",
            r->stream ? "true" : "false",
            r->failed ? "true" : "false",
            (int)r->status_code,
            (long long)r->input_tokens, (long long)r->output_tokens,
            (long long)r->reasoning_tokens, (long long)r->cache_read_tokens,
            (long long)r->cache_creation_tokens, (long long)r->total_tokens,
            calc_hit_rate(r->cache_read_tokens, r->input_tokens),
            (long long)r->ttft_ms, (long long)r->latency_ms);
        emitted++;
    }
    sb_append_str(&sb, "]}");

    for (size_t i = 0; i < m_count; i++) free(m_buckets[i].ttft_vals);
    for (size_t i = 0; i < a_count; i++) free(a_buckets[i].ttft_vals);
    free(m_buckets);
    free(a_buckets);
    free(ttft_all);
    free(lat_all);

    return sb.data ? sb.data : strdup("{\"error\":\"oom\"}");
}

static bool ends_with(const char* s, const char* suf) {
    if (!s || !suf) return false;
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return false;
    return strcmp(s + ls - lf, suf) == 0;
}

static char* handle_management_call(const uint8_t* req, size_t req_len) {
    jval_t* root = j_parse((const char*)req, req_len);
    if (!root || root->type != J_OBJ) {
        j_free(root);
        return make_mgmt_response_raw(400, "application/json; charset=utf-8",
                                      "{\"error\":\"invalid request\"}", 27);
    }

    const char* path = j_obj_get_str(root, "Path");
    const jval_t* query = j_obj_get(root, "Query");
    if (!query) query = j_obj_get_ci(root, "query");

    if (ends_with(path, "/ui") || ends_with(path, "/ui/")) {
        j_free(root);
        return make_mgmt_response_b64(200, "text/html; charset=utf-8", EMBEDDED_INDEX_HTML_B64);
    }

    if (ends_with(path, "/api") || ends_with(path, "/api/")) {
        const char* op = j_query_get(query, "op");
        if (!op || !op[0]) op = "summary";

        if (strcmp(op, "clear") == 0) {
            pthread_mutex_lock(&g_mu);
            load_state_locked();
            g_head = 0;
            g_count = 0;
            memset(&g_all_time, 0, sizeof(g_all_time));
            save_state_locked();
            pthread_mutex_unlock(&g_mu);
            j_free(root);
            const char* ok_body = "{\"ok\":true,\"cleared\":true}";
            return make_mgmt_response_raw(200, "application/json; charset=utf-8", ok_body, strlen(ok_body));
        }

        if (strcmp(op, "export") == 0) {
            pthread_mutex_lock(&g_mu);
            load_state_locked();
            char* body = build_summary_json_locked("all", NULL, NULL, MAX_RECORDS);
            pthread_mutex_unlock(&g_mu);
            j_free(root);
            char* resp = make_mgmt_response_raw(200, "application/json; charset=utf-8", body, strlen(body));
            free(body);
            return resp;
        }

        const char* range = j_query_get(query, "range");
        const char* model = j_query_get(query, "model");
        const char* auth = j_query_get(query, "auth");
        const char* lim_s = j_query_get(query, "limit");
        int limit = lim_s ? atoi(lim_s) : 200;

        pthread_mutex_lock(&g_mu);
        load_state_locked();
        char* body = build_summary_json_locked(range ? range : "today", model, auth, limit);
        pthread_mutex_unlock(&g_mu);
        j_free(root);
        char* resp = make_mgmt_response_raw(200, "application/json; charset=utf-8", body, strlen(body));
        free(body);
        return resp;
    }

    j_free(root);
    const char* nf = "{\"error\":\"not found\"}";
    return make_mgmt_response_raw(404, "application/json; charset=utf-8", nf, strlen(nf));
}

/* ---------- Registration & Entrypoint ---------- */

static const char* REGISTRATION_JSON =
    "{\"ok\":true,\"result\":{\"schema_version\":10,"
    "\"metadata\":{\"Name\":\"usage-metrics\",\"Version\":\"" PLUGIN_VERSION "\",\"Author\":\"yangzhicong\","
    "\"GitHubRepository\":\"https://github.com/router-for-me/CLIProxyAPI\","
    "\"Logo\":\"data:image/svg+xml;utf8,<svg xmlns=\\\"http://www.w3.org/2000/svg\\\" viewBox=\\\"0 0 24 24\\\" fill=\\\"none\\\" stroke=\\\"%234f46e5\\\" stroke-width=\\\"2\\\" stroke-linecap=\\\"round\\\" stroke-linejoin=\\\"round\\\"><path d=\\\"M3 3v18h18\\\"/><path d=\\\"M7 15l4-4 3 3 5-6\\\"/></svg>\","
    "\"ConfigFields\":[]},"
    "\"capabilities\":{\"usage_plugin\":true,\"management_api\":true}}}";

static const char* MANAGEMENT_REGISTER_JSON =
    "{\"ok\":true,\"result\":{"
    "\"routes\":[],"
    "\"resources\":["
    "{\"Path\":\"/ui\",\"Menu\":\"用量统计\",\"Description\":\"Token 用量、缓存命中率与首字延迟监控面板\"},"
    "{\"Path\":\"/api\"}"
    "]}}";

static void write_buffer(cliproxy_buffer* response, const char* text) {
    if (!response || !text) return;
    size_t len = strlen(text);
    void* ptr = malloc(len);
    if (!ptr) {
        response->ptr = NULL;
        response->len = 0;
        return;
    }
    memcpy(ptr, text, len);
    response->ptr = ptr;
    response->len = len;
}

static int plugin_call(const char* method, const uint8_t* request, size_t request_len, cliproxy_buffer* response) {
    if (response) {
        response->ptr = NULL;
        response->len = 0;
    }
    if (!method) {
        write_buffer(response, "{\"ok\":false,\"error\":{\"code\":\"invalid_method\",\"message\":\"method is required\"}}");
        return 1;
    }
    if (strcmp(method, "plugin.register") == 0 || strcmp(method, "plugin.reconfigure") == 0) {
        pthread_mutex_lock(&g_mu);
        load_state_locked();
        pthread_mutex_unlock(&g_mu);
        write_buffer(response, REGISTRATION_JSON);
        return 0;
    }
    if (strcmp(method, "management.register") == 0) {
        write_buffer(response, MANAGEMENT_REGISTER_JSON);
        return 0;
    }
    if (strcmp(method, "management.handle") == 0) {
        char* dyn = handle_management_call(request, request_len);
        write_buffer(response, dyn ? dyn : "{\"ok\":false,\"error\":{\"code\":\"internal\",\"message\":\"oom\"}}");
        free(dyn);
        return 0;
    }
    if (strcmp(method, "usage.handle") == 0) {
        handle_usage_record(request, request_len);
        write_buffer(response, "{\"ok\":true,\"result\":{}}");
        return 0;
    }
    write_buffer(response, "{\"ok\":false,\"error\":{\"code\":\"unknown_method\",\"message\":\"unknown method\"}}");
    return 0;
}

static void plugin_free(void* ptr, size_t len) {
    (void)len;
    free(ptr);
}

static void plugin_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    if (g_loaded && g_dirty_since_save > 0) {
        save_state_locked();
    }
    g_loaded = false;
    pthread_mutex_unlock(&g_mu);
}

CLIPROXY_EXPORT int cliproxy_plugin_init(const cliproxy_host_api* host, cliproxy_plugin_api* plugin) {
    (void)host;
    if (!plugin) return 1;
    plugin->abi_version = ABI_VERSION;
    plugin->call = plugin_call;
    plugin->free_buffer = plugin_free;
    plugin->shutdown = plugin_shutdown;
    return 0;
}
