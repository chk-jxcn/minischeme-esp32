/**
 * @file msbc.h
 * @brief MiniScheme ByteCode (MSBC) Binary Format Serializer and Deserializer.
 *
 * Provides a portable binary serialization for CodeObj and constants between
 * WASM (compiler host) and ESP32 / sim_esp32 (execution target).
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
#define MSBC_VERSION 1

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
    MSBC_CONST_BIGINT = 11    /* integer outside the 32-bit targets' fixnums: i8 sign,
                               * u16 limb count, u32 limbs (least significant first) */
};

/* ========================================================================= */
/* Serializer                                                                */
/* ========================================================================= */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} MsbcWriter;

static inline void msbc_writer_init(MsbcWriter *w) {
    w->data = NULL;
    w->size = 0;
    w->cap = 0;
}

static inline void msbc_writer_free(MsbcWriter *w) {
    if (w->data) {
        free(w->data);
        w->data = NULL;
    }
    w->size = 0;
    w->cap = 0;
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
    msbc_write_u32(w, co->code_len);
    msbc_write_u16(w, co->num_params);
    msbc_write_u16(w, co->max_stack);
    msbc_write_u8(w, co->is_variadic);
    msbc_write_u8(w, co->has_inner_lambdas);
    msbc_write_u16(w, co->num_consts);

    if (co->code_len > 0) {
        msbc_write_bytes(w, CODE_BYTES(co), co->code_len);
    }

    Value *consts = CODE_CONSTS(co);
    for (uint16_t i = 0; i < co->num_consts; i++) {
        msbc_serialize_val(vm, w, consts[i]);
    }
}

static inline bool msbc_serialize(MiniVM *vm, Value code_val, uint8_t **out_buf, size_t *out_size) {
    if (!mini_is_code(code_val)) return false;
    CodeObj *co = (CodeObj *)UNTAG_PTR(code_val);

    MsbcWriter w;
    msbc_writer_init(&w);

    uint32_t magic = MSBC_MAGIC;
    uint16_t version = MSBC_VERSION;
    uint16_t reserved = 0;

    msbc_write_u32(&w, magic);
    msbc_write_u16(&w, version);
    msbc_write_u16(&w, reserved);

    msbc_serialize_code(vm, &w, co);

    *out_buf = w.data;
    *out_size = w.size;
    return true;
}

/* ========================================================================= */
/* Deserializer                                                              */
/* ========================================================================= */

typedef struct {
    const uint8_t *cur;
    const uint8_t *end;
    FILE *fp;
    bool error;
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
        case MSBC_CONST_SYMBOL: {
            uint16_t slen = msbc_read_u16(r);
            char tmp[256];
            char *buf = (slen < 256) ? tmp : (char *)malloc(slen + 1);
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

    uint32_t code_len = msbc_read_u32(r);
    uint16_t num_params = msbc_read_u16(r);
    uint16_t max_stack = msbc_read_u16(r);
    uint8_t  is_variadic = msbc_read_u8(r);
    uint8_t  has_inner_lambdas = msbc_read_u8(r);
    uint16_t num_consts = msbc_read_u16(r);

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

    uint32_t magic = msbc_read_u32(&r);
    uint16_t version = msbc_read_u16(&r);
    (void)msbc_read_u16(&r); /* reserved */

    if (magic != MSBC_MAGIC || version != MSBC_VERSION) {
        vm_error(vm, "invalid MSBC header (magic=0x%08X, ver=%u)", (unsigned)magic, (unsigned)version);
        return mini_void();
    }

    Value co = msbc_deserialize_code(vm, &r);
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

    uint32_t magic = msbc_read_u32(r);
    uint16_t version = msbc_read_u16(r);
    (void)msbc_read_u16(r);

    if (magic != MSBC_MAGIC || version != MSBC_VERSION) {
        vm_error(vm, "invalid MSBC header (magic=0x%08X, ver=%u)", (unsigned)magic, (unsigned)version);
        free(r);
        return mini_void();
    }

    Value co = msbc_deserialize_code(vm, r);
    if (r->error || !mini_is_code(co)) {
        vm_error(vm, "failed to deserialize MSBC code object from file");
        free(r);
        return mini_void();
    }
    free(r);
    return co;
}

#endif /* MSBC_H */
