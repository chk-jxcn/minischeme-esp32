/**
 * @file minischeme_api.c
 * @brief API version, the API table (generated) and the procedures that read
 *        it: (api-version), (api-has? 'name), (api-info 'name), ...
 */

#include "minischeme_api.h"
#include "minischeme_api_table.h"
#include "minischeme_internal.h"
#include "minischeme_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_ext_mask = 0;

const char *mini_api_version(void) { return MINI_API_VERSION; }

void mini_api_set_extensions(unsigned mask) { s_ext_mask = mask; }

unsigned mini_api_get_extensions(void) { return s_ext_mask; }

unsigned mini_api_extension_bit(const char *id) {
    for (int i = 0; id && i < MINI_API_EXTENSION_COUNT; i++) {
        if (strcmp(mini_api_extensions[i][0], id) == 0) return 1u << i;
    }
    return 0;
}

static const MiniApiEntry *api_find(const char *name) {
    size_t lo = 0, hi = MINI_API_ENTRY_COUNT;
    while (name && lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, mini_api_table[mid].name);
        if (c == 0) return &mini_api_table[mid];
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* "1", "1.2" or "1.2.3" -> parts; false when not a version */
static bool parse_version(const char *s, long v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        long n = 0;
        while (*s >= '0' && *s <= '9' && n < 100000) n = n * 10 + (*s++ - '0');
        v[i] = n;
        if (*s != '.') break;
        s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    return *s == '\0';
}

bool mini_api_version_ok(const char *required, const char *provided) {
    long r[3], p[3];
    if (!parse_version(required, r) || !parse_version(provided, p)) return false;
    return r[0] == p[0] && r[1] <= p[1];
}

static void format_since(uint32_t since, char *buf, size_t cap) {
    snprintf(buf, cap, "%u.%u.%u", (unsigned)(since >> 16), (unsigned)((since >> 8) & 0xFF), (unsigned)(since & 0xFF));
}

static bool ext_present(int ext) { return ext > 0 && (s_ext_mask & (1u << (ext - 1))) != 0; }

static int ext_index(const char *id, size_t n) {
    for (int i = 0; i < MINI_API_EXTENSION_COUNT; i++) {
        if (strlen(mini_api_extensions[i][0]) == n && strncmp(mini_api_extensions[i][0], id, n) == 0) return i;
    }
    return -1;
}

bool mini_api_check_meta(const char *meta, char *err, size_t cap) {
    char val[96];
    if (err && cap) err[0] = '\0';
    if (!meta) return true;
    if (mini_device_config_value(meta, "api", val, sizeof(val)) && val[0] &&
        !mini_api_version_ok(val, MINI_API_VERSION)) {
        if (err) snprintf(err, cap, "app needs API %s, the firmware provides %s", val, MINI_API_VERSION);
        return false;
    }
    /* extensions = esp32 1.0, other (a version is optional) */
    if (!mini_device_config_value(meta, "extensions", val, sizeof(val))) return true;
    for (const char *p = val; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        const char *id = p;
        while (*p && *p != ' ' && *p != ',') p++;
        size_t n = (size_t)(p - id);
        while (*p == ' ') p++;
        char need[16] = "";
        if (*p >= '0' && *p <= '9') {
            size_t k = 0;
            while (*p && *p != ',' && *p != ' ' && k + 1 < sizeof(need)) need[k++] = *p++;
            need[k] = '\0';
            while (*p && *p != ',') p++;
        }
        if (n == 0) continue;
        int x = ext_index(id, n);
        if (x < 0 || !ext_present(x + 1)) {
            if (err) snprintf(err, cap, "app needs the %.*s extension API, this device has none", (int)n, id);
            return false;
        }
        if (need[0] && !mini_api_version_ok(need, mini_api_extensions[x][1])) {
            if (err) snprintf(err, cap, "app needs %.*s API %s, the device provides %s", (int)n, id, need,
                              mini_api_extensions[x][1]);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* VM hooks: older names and unbound-name hints                              */
/* ------------------------------------------------------------------------- */

/* The i-th other name of an API procedure: an older name's current name, or
 * a current name's older names (the implementation may sit under either) */
static const char *api_other_name(const char *name, size_t i) {
    const MiniApiEntry *e = api_find(name);
    if (!e) return NULL;
    if (e->status == MINI_API_ALIAS) return i == 0 ? mini_api_table[e->alias].name : NULL;
    int self = (int)(e - mini_api_table);
    for (size_t k = 0; k < MINI_API_ENTRY_COUNT; k++) {
        if (mini_api_table[k].status == MINI_API_ALIAS && mini_api_table[k].alias == self && i-- == 0) {
            return mini_api_table[k].name;
        }
    }
    return NULL;
}

static const char *api_unbound_hint(const char *name) {
    static char hint[96];
    const MiniApiEntry *e = api_find(name);
    if (!e) return NULL;
    char since[16];
    format_since(e->since, since, sizeof(since));
    if (e->status == MINI_API_RESERVED) {
        snprintf(hint, sizeof(hint), " (reserved API since %s: not provided by this firmware, API %s)", since,
                 MINI_API_VERSION);
    } else if (e->ext && !ext_present(e->ext)) {
        snprintf(hint, sizeof(hint), " (%s extension API: not on this device)", mini_api_extensions[e->ext - 1][0]);
    } else if (e->where == MINI_API_PC) {
        snprintf(hint, sizeof(hint), " (PC REPL only)");
    } else if (e->where == MINI_API_DEVICE) {
        snprintf(hint, sizeof(hint), " (device API: firmware and WASM simulator only)");
    } else {
        return NULL;
    }
    return hint;
}

/* ------------------------------------------------------------------------- */
/* Procedures                                                                */
/* ------------------------------------------------------------------------- */

static const char *name_arg(MiniVM *vm, Value v, const char *who) {
    if (mini_is_symbol(v)) return mini_symbol_name(v);
    if (mini_is_string(v)) return mini_string_data(v);
    vm_error(vm, "%s: expected a symbol or string", who);
    return NULL;
}

static Value prim_api_version(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_string(vm, MINI_API_VERSION);
}

static Value prim_api_compatible(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "api-compatible?: expected a version string");
        return mini_void();
    }
    return mini_api_version_ok(mini_string_data(argv[0]), MINI_API_VERSION) ? mini_true() : mini_false();
}

static Value prim_api_has(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) {
        vm_error(vm, "api-has?: expected 1 argument");
        return mini_void();
    }
    const char *name = name_arg(vm, argv[0], "api-has?");
    if (!name) return mini_void();
    const MiniApiEntry *e = api_find(name);
    if (!e || e->status == MINI_API_RESERVED || (e->ext && !ext_present(e->ext))) return mini_false();
    if (strcmp(mini_api_components[e->component], "syntax") == 0) return mini_true();
    char copy[64];   /* the name may live in the heap: the lookup can collect */
    snprintf(copy, sizeof(copy), "%s", name);
    Value v = env_lookup_name(vm, copy);
    return (mini_is_cfunc(v) || mini_is_closure(v)) ? mini_true() : mini_false();
}

/* (component since status extension where) */
static Value prim_api_info(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) {
        vm_error(vm, "api-info: expected 1 argument");
        return mini_void();
    }
    const char *name = name_arg(vm, argv[0], "api-info");
    if (!name) return mini_void();
    const MiniApiEntry *e = api_find(name);
    if (!e) return mini_false();
    static const char *const status[] = { "stable", "reserved", "alias" };
    static const char *const where[] = { "all", "device", "pc" };
    char since[16];
    format_since(e->since, since, sizeof(since));
    Value l = mini_nil(), v = mini_void();
    mini_push_root(vm, &l);
    mini_push_root(vm, &v);
    v = mini_make_symbol(vm, where[e->where]);
    l = mini_make_cons(vm, v, l);
    v = e->ext ? mini_make_string(vm, mini_api_extensions[e->ext - 1][0]) : mini_false();
    l = mini_make_cons(vm, v, l);
    v = mini_make_symbol(vm, status[e->status]);
    l = mini_make_cons(vm, v, l);
    v = mini_make_string(vm, since);
    l = mini_make_cons(vm, v, l);
    v = mini_make_string(vm, mini_api_components[e->component]);
    l = mini_make_cons(vm, v, l);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return l;
}

static Value prim_api_list(MiniVM *vm, int argc, Value *argv) {
    int comp = -1;
    if (argc >= 1) {
        const char *c = name_arg(vm, argv[0], "api-list");
        if (!c) return mini_void();
        for (int i = 0; i < MINI_API_COMPONENT_COUNT; i++) {
            if (strcmp(mini_api_components[i], c) == 0) comp = i;
        }
        if (comp < 0) return mini_nil();
    }
    Value l = mini_nil(), s = mini_void();
    mini_push_root(vm, &l);
    mini_push_root(vm, &s);
    for (size_t i = MINI_API_ENTRY_COUNT; i-- > 0;) {
        if (comp >= 0 && mini_api_table[i].component != comp) continue;
        s = mini_make_string(vm, mini_api_table[i].name);
        l = mini_make_cons(vm, s, l);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return l;
}

static Value prim_api_components(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value l = mini_nil(), s = mini_void();
    mini_push_root(vm, &l);
    mini_push_root(vm, &s);
    for (int i = MINI_API_COMPONENT_COUNT; i-- > 0;) {
        s = mini_make_string(vm, mini_api_components[i]);
        l = mini_make_cons(vm, s, l);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return l;
}

static Value prim_api_extensions(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value l = mini_nil(), a = mini_void(), b = mini_void();
    mini_push_root(vm, &l);
    mini_push_root(vm, &a);
    mini_push_root(vm, &b);
    for (int i = MINI_API_EXTENSION_COUNT; i-- > 0;) {
        if (!ext_present(i + 1)) continue;
        a = mini_make_string(vm, mini_api_extensions[i][0]);
        b = mini_make_string(vm, mini_api_extensions[i][1]);
        a = mini_make_cons(vm, a, b);
        l = mini_make_cons(vm, a, l);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return l;
}

static const MiniPrimEntry API_PRIMS[] = {
    { "api-version", prim_api_version },
    { "api-compatible?", prim_api_compatible },
    { "api-has?", prim_api_has },
    { "api-info", prim_api_info },
    { "api-list", prim_api_list },
    { "api-components", prim_api_components },
    { "api-extensions", prim_api_extensions },
};

void mini_register_api_lib(MiniVM *vm) {
    mini_api_alias_hook = api_other_name;
    mini_api_unbound_hook = api_unbound_hint;
    mini_register_prim_table(vm, API_PRIMS, sizeof(API_PRIMS) / sizeof(API_PRIMS[0]));
}
