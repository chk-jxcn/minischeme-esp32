/**
 * @file msbc.h
 * @brief MiniScheme ByteCode (MSBC) Binary Format Serializer and Deserializer.
 *
 * Provides a portable binary serialization for CodeObj and constants between
 * WASM (compiler host) and ESP32 / sim_esp32 (execution target).
 *
 * Layout: u32 magic "MSBC", u16 version, u16 reserved, then (version 2) the
 * symbol table: u16 count, each name as u16 length + bytes, the names used
 * most first; then the top-level code object (no tag byte). Version 2 refers
 * to symbols by index (MSBC_CONST_SYMREF + the index as a LEB128 varint: one
 * byte for the 128 most used names) instead of writing the name in every
 * constant pool that uses it: a quarter of an app image was repeated names.
 * A version 2 code object's header is varints too: code length, parameters,
 * stack size, flags (1 variadic, 2 inner lambdas), constants; version 1 has
 * u32, u16, u16, u8, u8, u16.
 * Version 1 images (names inline) are still read;
 * msbc_serialize_version(..., 1) still writes them.
 */

#ifndef MSBC_H
#define MSBC_H

#include "minischeme.h"
#include "minischeme_internal.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define MSBC_MAGIC 0x4342534D /* "MSBC" */
#define MSBC_VERSION 2          /* written by default */
#define MSBC_VERSION_MIN 1      /* the oldest still read */

enum {
    MSBC_CONST_FIXNUM = 0,
    MSBC_CONST_FLONUM = 1,
    MSBC_CONST_BOOL   = 2,
    MSBC_CONST_STRING = 3,
    MSBC_CONST_SYMBOL = 4,
    MSBC_CONST_NIL    = 5,
    MSBC_CONST_VOID   = 6,
    MSBC_CONST_CODE   = 7,
    MSBC_CONST_PAIR   = 8,
    MSBC_CONST_VECTOR = 9,
    MSBC_CONST_LSTRING = 10,  /* string with a u32 length (over 65535 bytes) */
    MSBC_CONST_BIGINT = 11,   /* integer outside the 32-bit targets' fixnums: i8 sign,
                               * u16 limb count, u32 limbs (least significant first) */
    MSBC_CONST_SYMREF = 12    /* version 2: a symbol by its index in the symbol table (LEB128 varint) */
};

/* ========================================================================= */
/* Serializer                                                                */
/* ========================================================================= */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
    Value *syms;        /* version 2: the symbol table (the names used most first) */
    uint32_t *uses;     /* how often each is referred to */
    uint32_t nsyms;
    uint32_t syms_cap;
    bool symref;        /* constants refer to syms by index */
} MsbcWriter;

static inline void msbc_writer_init(MsbcWriter *w) {
    memset(w, 0, sizeof(*w));
}

static inline void msbc_writer_free(MsbcWriter *w) {
    if (w->data) {
        free(w->data);
        w->data = NULL;
    }
    free(w->syms);
    free(w->uses);
    w->syms = NULL;
    w->uses = NULL;
    w->nsyms = w->syms_cap = 0;
    w->size = 0;
    w->cap = 0;
}

/* The index of symbol v in the writer's table, -1 when absent. Symbols are
 * interned: the same name is the same Value. A few hundred per app. */
static inline int32_t msbc_sym_index(const MsbcWriter *w, Value v) {
    for (uint32_t i = 0; i < w->nsyms; i++) {
        if (w->syms[i] == v) return (int32_t)i;
    }
    return -1;
}

/* Every symbol among the constants of co and its inner code, once each, in
 * order of first use; false when out of memory */
static bool msbc_collect_syms_val(MiniVM *vm, MsbcWriter *w, Value v);

static bool msbc_collect_syms_code(MiniVM *vm, MsbcWriter *w, CodeObj *co) {
    Value *consts = CODE_CONSTS(co);
    for (uint16_t i = 0; i < co->num_consts; i++) {
        if (!msbc_collect_syms_val(vm, w, consts[i])) return false;
    }
    return true;
}

static bool msbc_collect_syms_val(MiniVM *vm, MsbcWriter *w, Value v) {
    if (mini_is_symbol(v)) {
        int32_t at = msbc_sym_index(w, v);
        if (at >= 0) {
            w->uses[at]++;
            return true;
        }
        if (w->nsyms == w->syms_cap) {
            uint32_t cap = w->syms_cap ? w->syms_cap * 2 : 256;
            Value *n = (Value *)realloc(w->syms, cap * sizeof(Value));
            if (!n) return false;
            w->syms = n;
            uint32_t *u = (uint32_t *)realloc(w->uses, cap * sizeof(uint32_t));
            if (!u) return false;
            w->uses = u;
            w->syms_cap = cap;
        }
        w->uses[w->nsyms] = 1;
        w->syms[w->nsyms++] = v;
        return true;
    }
    if (mini_is_code(v)) return msbc_collect_syms_code(vm, w, (CodeObj *)UNTAG_PTR(v));
    while (mini_is_pair(v)) {      /* lists: the cdr chain in a loop */
        if (!msbc_collect_syms_val(vm, w, mini_car(v))) return false;
        v = mini_cdr(v);
    }
    if (mini_is_symbol(v)) return msbc_collect_syms_val(vm, w, v);
    if (mini_is_vector(v)) {
        size_t n = mini_vector_length(v);
        for (size_t i = 0; i < n && i < 65535; i++) {
            if (!msbc_collect_syms_val(vm, w, mini_vector_ref(vm, v, i))) return false;
        }
    }
    return true;
}

static inline void msbc_write_bytes(MsbcWriter *w, const void *ptr, size_t n) {
    if (w->size + n > w->cap) {
        size_t new_cap = w->cap ? w->cap * 2 : 256;
        while (new_cap < w->size + n) new_cap *= 2;
        w->data = (uint8_t *)realloc(w->data, new_cap);
        w->cap = new_cap;
    }
    memcpy(w->data + w->size, ptr, n);
    w->size += n;
}

static inline void msbc_write_u8(MsbcWriter *w, uint8_t v) {
    msbc_write_bytes(w, &v, sizeof(v));
}

static inline void msbc_write_u16(MsbcWriter *w, uint16_t v) {
    msbc_write_bytes(w, &v, sizeof(v));
}

static inline void msbc_write_u32(MsbcWriter *w, uint32_t v) {
    msbc_write_bytes(w, &v, sizeof(v));
}

static inline void msbc_write_u64(MsbcWriter *w, uint64_t v) {
    msbc_write_bytes(w, &v, sizeof(v));
}

/* LEB128: 7 bits a byte, the high bit set on all but the last */
static inline void msbc_write_varint(MsbcWriter *w, uint32_t v) {
    while (v >= 0x80) {
        msbc_write_u8(w, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    msbc_write_u8(w, (uint8_t)v);
}

static void msbc_serialize_code(MiniVM *vm, MsbcWriter *w, CodeObj *co);

/* The fixnum range every target shares (29-bit fixnums on 32-bit targets):
 * integers outside it are written as MSBC_CONST_BIGINT, whatever the
 * compiling machine's own fixnums */
#define MSBC_FIXNUM_MIN (-((int64_t)1 << 28))
#define MSBC_FIXNUM_MAX (((int64_t)1 << 28) - 1)

static void msbc_write_bigint(MsbcWriter *w, int sign, const uint32_t *limbs, uint32_t n) {
    msbc_write_u8(w, MSBC_CONST_BIGINT);
    msbc_write_u8(w, (uint8_t)(int8_t)(sign < 0 ? -1 : 1));
    msbc_write_u16(w, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) msbc_write_u32(w, limbs[i]);
}

static void msbc_serialize_val(MiniVM *vm, MsbcWriter *w, Value v) {
    if (mini_is_fixnum(v)) {
        int64_t iv = (int64_t)mini_to_fixnum(v);
        if (iv >= MSBC_FIXNUM_MIN && iv <= MSBC_FIXNUM_MAX) {
            msbc_write_u8(w, MSBC_CONST_FIXNUM);
            msbc_write_u64(w, (uint64_t)iv);
        } else {
            uint64_t mag = iv < 0 ? (uint64_t)0 - (uint64_t)iv : (uint64_t)iv;
            uint32_t limbs[2] = { (uint32_t)mag, (uint32_t)(mag >> 32) };
            msbc_write_bigint(w, iv < 0 ? -1 : 1, limbs, limbs[1] ? 2 : 1);
        }
    } else if (mini_is_bigint(v)) {
        const BigIntObj *b = (const BigIntObj *)UNTAG_PTR(v);
        msbc_write_bigint(w, b->sign, b->digits, b->length);
    } else if (mini_is_flonum(v)) {
        msbc_write_u8(w, MSBC_CONST_FLONUM);
        double dv = mini_to_flonum(v);
        msbc_write_bytes(w, &dv, sizeof(double));
    } else if (mini_is_true(v)) {
        msbc_write_u8(w, MSBC_CONST_BOOL);
        msbc_write_u8(w, 1);
    } else if (mini_is_false(v)) {
        msbc_write_u8(w, MSBC_CONST_BOOL);
        msbc_write_u8(w, 0);
    } else if (mini_is_nil(v)) {
        msbc_write_u8(w, MSBC_CONST_NIL);
    } else if (mini_is_void(v)) {
        msbc_write_u8(w, MSBC_CONST_VOID);
    } else if (mini_is_string(v)) {
        const char *s = mini_string_data(v);
        size_t slen = mini_string_len(v);
        if (slen > 65535) {
            msbc_write_u8(w, MSBC_CONST_LSTRING);
            msbc_write_u32(w, (uint32_t)slen);
        } else {
            msbc_write_u8(w, MSBC_CONST_STRING);
            msbc_write_u16(w, (uint16_t)slen);
        }
        msbc_write_bytes(w, s, slen);
    } else if (mini_is_symbol(v) && w->symref) {
        msbc_write_u8(w, MSBC_CONST_SYMREF);
        msbc_write_varint(w, (uint32_t)msbc_sym_index(w, v));
    } else if (mini_is_symbol(v)) {
        msbc_write_u8(w, MSBC_CONST_SYMBOL);
        const char *s = mini_symbol_name(v);
        size_t slen = strlen(s);
        if (slen > 65535) slen = 65535;
        msbc_write_u16(w, (uint16_t)slen);
        msbc_write_bytes(w, s, slen);
    } else if (mini_is_code(v)) {
        msbc_write_u8(w, MSBC_CONST_CODE);
        msbc_serialize_code(vm, w, (CodeObj *)UNTAG_PTR(v));
    } else if (mini_is_pair(v)) {
        msbc_write_u8(w, MSBC_CONST_PAIR);
        msbc_serialize_val(vm, w, mini_car(v));
        msbc_serialize_val(vm, w, mini_cdr(v));
    } else if (mini_is_vector(v)) {
        msbc_write_u8(w, MSBC_CONST_VECTOR);
        size_t vlen = mini_vector_length(v);
        if (vlen > 65535) vlen = 65535;
        msbc_write_u16(w, (uint16_t)vlen);
        for (size_t i = 0; i < vlen; i++) {
            msbc_serialize_val(vm, w, mini_vector_ref(vm, v, i));
        }
    } else {
        /* Fallback for unsupported constant */
        msbc_write_u8(w, MSBC_CONST_VOID);
    }
}

static void msbc_serialize_code(MiniVM *vm, MsbcWriter *w, CodeObj *co) {
    if (w->symref) {          /* version 2: the header as varints */
        msbc_write_varint(w, co->code_len);
        msbc_write_varint(w, co->num_params);
        msbc_write_varint(w, co->max_stack);
        msbc_write_u8(w, (uint8_t)((co->is_variadic ? 1 : 0) | (co->has_inner_lambdas ? 2 : 0)));
        msbc_write_varint(w, co->num_consts);
    } else {
        msbc_write_u32(w, co->code_len);
        msbc_write_u16(w, co->num_params);
        msbc_write_u16(w, co->max_stack);
        msbc_write_u8(w, co->is_variadic);
        msbc_write_u8(w, co->has_inner_lambdas);
        msbc_write_u16(w, co->num_consts);
    }

    if (co->code_len > 0) {
        msbc_write_bytes(w, CODE_BYTES(co), co->code_len);
    }

    Value *consts = CODE_CONSTS(co);
    for (uint16_t i = 0; i < co->num_consts; i++) {
        msbc_serialize_val(vm, w, consts[i]);
    }
}

/* code_val as MSBC of the given version (1: names inline, 2: a symbol
 * table); *out_buf is malloc'ed. Version 2 falls back to 1 for more than
 * 65535 distinct symbols. */
static inline bool msbc_serialize_version(MiniVM *vm, Value code_val, uint8_t **out_buf, size_t *out_size,
                                          uint16_t version) {
    if (!mini_is_code(code_val)) return false;
    CodeObj *co = (CodeObj *)UNTAG_PTR(code_val);

    MsbcWriter w;
    msbc_writer_init(&w);
    if (version >= 2) {
        if (!msbc_collect_syms_code(vm, &w, co)) {
            msbc_writer_free(&w);
            return false;
        }
        if (w.nsyms > 65535) version = 1;
        /* the names used most first: their indexes take one byte (stable:
         * equal counts keep the order of first use) */
        for (uint32_t i = 1; i < w.nsyms; i++) {
            Value sv = w.syms[i];
            uint32_t su = w.uses[i], j = i;
            while (j > 0 && w.uses[j - 1] < su) {
                w.syms[j] = w.syms[j - 1];
                w.uses[j] = w.uses[j - 1];
                j--;
            }
            w.syms[j] = sv;
            w.uses[j] = su;
        }
    }
    w.symref = version >= 2;

    msbc_write_u32(&w, MSBC_MAGIC);
    msbc_write_u16(&w, version >= 2 ? 2 : 1);
    msbc_write_u16(&w, 0);   /* reserved */
    if (w.symref) {
        msbc_write_u16(&w, (uint16_t)w.nsyms);
        for (uint32_t i = 0; i < w.nsyms; i++) {
            const char *s = mini_symbol_name(w.syms[i]);
            size_t slen = strlen(s);
            if (slen > 65535) slen = 65535;
            msbc_write_u16(&w, (uint16_t)slen);
            msbc_write_bytes(&w, s, slen);
        }
    }

    msbc_serialize_code(vm, &w, co);

    free(w.syms);
    free(w.uses);
    *out_buf = w.data;
    *out_size = w.size;
    return true;
}

static inline bool msbc_serialize(MiniVM *vm, Value code_val, uint8_t **out_buf, size_t *out_size) {
    return msbc_serialize_version(vm, code_val, out_buf, out_size, MSBC_VERSION);
}

/* ========================================================================= */
/* Deserializer                                                              */
/* ========================================================================= */

typedef struct {
    const uint8_t *cur;
    const uint8_t *end;
    FILE *fp;
    bool error;
    Value *syms;        /* version 2: a rooted vector of the table's symbols */
    uint16_t nsyms;
    uint16_t version;
    uint8_t buffer[1024];
} MsbcReader;

static inline bool msbc_fill_buffer(MsbcReader *r, size_t needed) {
    if (!r->fp) return false;
    size_t remaining = (size_t)(r->end - r->cur);
    if (remaining > 0 && r->cur > r->buffer) {
        memmove(r->buffer, r->cur, remaining);
    }
    r->cur = r->buffer;
    r->end = r->buffer + remaining;
    size_t space = sizeof(r->buffer) - remaining;
    if (space > 0) {
        size_t n = fread(r->buffer + remaining, 1, space, r->fp);
        r->end += n;
    }
    return (size_t)(r->end - r->cur) >= needed;
}

static inline uint8_t msbc_read_u8(MsbcReader *r) {
    if (r->cur >= r->end) {
        if (!msbc_fill_buffer(r, 1)) { r->error = true; return 0; }
    }
    return *r->cur++;
}

static inline uint16_t msbc_read_u16(MsbcReader *r) {
    if (r->cur + 2 > r->end) {
        if (!msbc_fill_buffer(r, 2)) { r->error = true; return 0; }
    }
    uint16_t v;
    memcpy(&v, r->cur, 2);
    r->cur += 2;
    return v;
}

static inline uint32_t msbc_read_u32(MsbcReader *r) {
    if (r->cur + 4 > r->end) {
        if (!msbc_fill_buffer(r, 4)) { r->error = true; return 0; }
    }
    uint32_t v;
    memcpy(&v, r->cur, 4);
    r->cur += 4;
    return v;
}

static inline uint64_t msbc_read_u64(MsbcReader *r) {
    if (r->cur + 8 > r->end) {
        if (!msbc_fill_buffer(r, 8)) { r->error = true; return 0; }
    }
    uint64_t v;
    memcpy(&v, r->cur, 8);
    r->cur += 8;
    return v;
}

static inline bool msbc_read_into(MsbcReader *r, uint8_t *dest, size_t n) {
    while (n > 0) {
        size_t avail = (size_t)(r->end - r->cur);
        if (avail > 0) {
            size_t take = (n < avail) ? n : avail;
            memcpy(dest, r->cur, take);
            r->cur += take;
            dest += take;
            n -= take;
        } else if (r->fp) {
            size_t got = fread(dest, 1, n, r->fp);
            if (got != n) {
                r->error = true;
                return false;
            }
            return true;
        } else {
            r->error = true;
            return false;
        }
    }
    return true;
}

static inline const uint8_t *msbc_read_bytes(MsbcReader *r, size_t n) {
    if (r->cur + n > r->end) {
        if (!msbc_fill_buffer(r, n)) { r->error = true; return NULL; }
    }
    const uint8_t *p = r->cur;
    r->cur += n;
    return p;
}

static Value msbc_deserialize_code(MiniVM *vm, MsbcReader *r);

/* A LEB128 varint of at most 5 bytes (32 bits) */
static uint32_t msbc_read_varint(MsbcReader *r) {
    uint32_t v = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        uint8_t b = msbc_read_u8(r);
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    r->error = true;
    return 0;
}

/* A symbol written as u16 length + name, interned */
static Value msbc_read_symbol(MiniVM *vm, MsbcReader *r) {
    uint16_t slen = msbc_read_u16(r);
    char tmp[256];
    char *buf = (slen < 256) ? tmp : (char *)malloc((size_t)slen + 1);
    if (!buf || !msbc_read_into(r, (uint8_t *)buf, slen)) {
        if (buf != tmp && buf) free(buf);
        r->error = true;
        return mini_void();
    }
    buf[slen] = '\0';
    Value res = mini_make_symbol(vm, buf);
    if (buf != tmp) free(buf);
    return res;
}

/* The header after the magic: version check, then (version 2) the symbol
 * table into *syms (a vector the caller has rooted); false on failure */
static bool msbc_read_header(MiniVM *vm, MsbcReader *r, Value *syms) {
    uint32_t magic = msbc_read_u32(r);
    uint16_t version = msbc_read_u16(r);
    (void)msbc_read_u16(r); /* reserved */
    if (magic != MSBC_MAGIC || version < MSBC_VERSION_MIN || version > MSBC_VERSION) {
        vm_error(vm, "invalid MSBC header (magic=0x%08X, ver=%u)", (unsigned)magic, (unsigned)version);
        return false;
    }
    r->version = version;
    if (version < 2) return true;
    uint16_t n = msbc_read_u16(r);
    if (r->error) {
        vm_error(vm, "truncated MSBC symbol table");
        return false;
    }
    *syms = mini_make_vector(vm, n, mini_void());
    if (!mini_is_vector(*syms)) return false;
    for (uint16_t i = 0; i < n && !r->error; i++) {
        Value sym = msbc_read_symbol(vm, r);
        mini_vector_set(vm, *syms, i, sym);
    }
    r->syms = syms;
    r->nsyms = n;
    if (r->error) vm_error(vm, "truncated MSBC symbol table");
    return !r->error;
}

static Value msbc_deserialize_val(MiniVM *vm, MsbcReader *r) {
    if (r->error) return mini_void();
    if (r->cur >= r->end) {
        if (!msbc_fill_buffer(r, 1)) return mini_void();
    }
    uint8_t tag = msbc_read_u8(r);
    switch (tag) {
        case MSBC_CONST_FIXNUM: {
            /* older images: a 64-bit compiler's fixnum may not fit here */
            uint64_t uv = msbc_read_u64(r);
            return bigint_from_int64(vm, (int64_t)uv);
        }
        case MSBC_CONST_BIGINT: {
            int8_t sign = (int8_t)msbc_read_u8(r);
            uint16_t n = msbc_read_u16(r);
            uint32_t small[4];
            uint32_t *limbs = n <= 4 ? small : (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
            if (!limbs || !msbc_read_into(r, (uint8_t *)limbs, (size_t)n * sizeof(uint32_t))) {
                if (limbs && limbs != small) free(limbs);
                r->error = true;
                return mini_void();
            }
            Value res = bigint_from_limbs(vm, sign < 0 ? -1 : 1, limbs, n);
            if (limbs != small) free(limbs);
            return res;
        }
        case MSBC_CONST_FLONUM: {
            double dv;
            if (!msbc_read_into(r, (uint8_t *)&dv, sizeof(double))) return mini_void();
            return mini_make_flonum(vm, dv);
        }
        case MSBC_CONST_BOOL: {
            uint8_t bv = msbc_read_u8(r);
            return bv ? mini_true() : mini_false();
        }
        case MSBC_CONST_STRING:
        case MSBC_CONST_LSTRING: {
            /* binary safe: embedded fonts and other include-bytes data */
            uint32_t slen = tag == MSBC_CONST_LSTRING ? msbc_read_u32(r) : msbc_read_u16(r);
            char tmp[256];
            char *buf = (slen < 256) ? tmp : (char *)malloc((size_t)slen + 1);
            if (!buf || !msbc_read_into(r, (uint8_t *)buf, slen)) {
                if (buf != tmp && buf) free(buf);
                r->error = true;
                return mini_void();
            }
            buf[slen] = '\0';
            Value res = mini_make_string_len(vm, buf, slen);
            if (buf != tmp) free(buf);
            return res;
        }
        case MSBC_CONST_SYMBOL:
            return msbc_read_symbol(vm, r);
        case MSBC_CONST_SYMREF: {
            uint32_t idx = msbc_read_varint(r);
            if (r->error || !r->syms || idx >= r->nsyms) {
                r->error = true;
                return mini_void();
            }
            return mini_vector_ref(vm, *r->syms, idx);
        }
        case MSBC_CONST_NIL:
            return mini_nil();
        case MSBC_CONST_VOID:
            return mini_void();
        case MSBC_CONST_CODE:
            return msbc_deserialize_code(vm, r);
        case MSBC_CONST_PAIR: {
            Value a = msbc_deserialize_val(vm, r);
            mini_push_root(vm, &a);
            Value d = msbc_deserialize_val(vm, r);
            Value p = mini_make_cons(vm, a, d);
            mini_pop_root(vm);
            return p;
        }
        case MSBC_CONST_VECTOR: {
            uint16_t vlen = msbc_read_u16(r);
            Value vec = mini_make_vector(vm, vlen, mini_void());
            mini_push_root(vm, &vec);
            for (uint16_t i = 0; i < vlen; i++) {
                Value elem = msbc_deserialize_val(vm, r);
                mini_vector_set(vm, vec, i, elem);
            }
            mini_pop_root(vm);
            return vec;
        }
        default:
            r->error = true;
            return mini_void();
    }
}

static Value msbc_deserialize_code(MiniVM *vm, MsbcReader *r) {
    if (r->error) return mini_void();

    uint32_t code_len;
    uint16_t num_params, max_stack, num_consts;
    uint8_t is_variadic, has_inner_lambdas;
    if (r->version >= 2) {
        code_len = msbc_read_varint(r);
        uint32_t np = msbc_read_varint(r), ms = msbc_read_varint(r);
        uint8_t flags = msbc_read_u8(r);
        uint32_t nc = msbc_read_varint(r);
        if (np > 0xFFFF || ms > 0xFFFF || nc > 0xFFFF) r->error = true;
        num_params = (uint16_t)np;
        max_stack = (uint16_t)ms;
        num_consts = (uint16_t)nc;
        is_variadic = flags & 1;
        has_inner_lambdas = (flags >> 1) & 1;
    } else {
        code_len = msbc_read_u32(r);
        num_params = msbc_read_u16(r);
        max_stack = msbc_read_u16(r);
        is_variadic = msbc_read_u8(r);
        has_inner_lambdas = msbc_read_u8(r);
        num_consts = msbc_read_u16(r);
    }

    if (r->error) return mini_void();

    Value code_val = mini_make_code(vm, code_len, NULL,
                                    num_params, max_stack,
                                    is_variadic, has_inner_lambdas,
                                    num_consts, NULL);
    if (mini_is_void(code_val)) return mini_void();
    mini_push_root(vm, &code_val);

    CodeObj *co = (CodeObj *)UNTAG_PTR(code_val);
    if (code_len > 0) {
        if (!msbc_read_into(r, CODE_BYTES(co), code_len)) {
            mini_pop_root(vm);
            return mini_void();
        }
    }

    Value *c_dest = CODE_CONSTS(co);
    for (uint16_t i = 0; i < num_consts; i++) {
        c_dest[i] = mini_void();
    }

    for (uint16_t i = 0; i < num_consts; i++) {
        Value val = msbc_deserialize_val(vm, r);
        if (r->error) {
            mini_pop_root(vm);
            return mini_void();
        }
        co = (CodeObj *)UNTAG_PTR(code_val);
        c_dest = CODE_CONSTS(co);
        c_dest[i] = val;
        mini_write_barrier(vm, code_val, val);
    }

    mini_pop_root(vm);
    return code_val;
}

static inline Value msbc_deserialize(MiniVM *vm, const uint8_t *buf, size_t size) {
    if (!vm || !buf || size < 8) {
        if (vm) vm_error(vm, "invalid bytecode buffer");
        return mini_void();
    }

    MsbcReader r;
    memset(&r, 0, sizeof(r));
    r.cur = buf;
    r.end = buf + size;
    r.fp = NULL;
    r.error = false;

    Value syms = mini_nil();
    mini_push_root(vm, &syms);
    bool header_ok = msbc_read_header(vm, &r, &syms);
    Value co = header_ok ? msbc_deserialize_code(vm, &r) : mini_void();
    mini_pop_root(vm);
    if (!header_ok) return mini_void();
    if (r.error || !mini_is_code(co)) {
        vm_error(vm, "failed to deserialize MSBC code object");
        return mini_void();
    }
    return co;
}

static inline Value msbc_deserialize_file(MiniVM *vm, FILE *fp) {
    if (!vm || !fp) {
        if (vm) vm_error(vm, "invalid file handle");
        return mini_void();
    }
    MsbcReader *r = (MsbcReader *)calloc(1, sizeof(MsbcReader));
    if (!r) {
        vm_error(vm, "out of memory allocating MsbcReader");
        return mini_void();
    }
    r->fp = fp;
    r->cur = r->buffer;
    r->end = r->buffer;
    r->error = false;

    Value syms = mini_nil();
    mini_push_root(vm, &syms);
    bool header_ok = msbc_read_header(vm, r, &syms);
    Value co = header_ok ? msbc_deserialize_code(vm, r) : mini_void();
    mini_pop_root(vm);
    if (!header_ok) {
        free(r);
        return mini_void();
    }
    if (r->error || !mini_is_code(co)) {
        vm_error(vm, "failed to deserialize MSBC code object from file");
        free(r);
        return mini_void();
    }
    free(r);
    return co;
}

#endif /* MSBC_H */
