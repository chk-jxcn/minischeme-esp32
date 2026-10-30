/**
 * @file minischeme_fasl.c
 * @brief MiniScheme Fast Loading (FASL) & Whole-Machine Relocation Snapshot Engine.
 *
 * Implements:
 *  - 100% Pure C heap compaction, pointer-to-offset serialization, and restoration.
 *  - Zero dependency on Scheme code, Scheme compiler, or reader.
 *  - 4-level dynamic C function relocation (built-ins -> host registered -> dynamic callback -> safety stub).
 *  - High-performance binary file and in-memory buffer dump & load.
 *  - Native support for arbitrary-precision integers (BigInt).
 */

#include "minischeme_internal.h"
#include "minischeme_gui.h"
#include "minischeme_cfasl.h"
#include "minischeme_help.h"
#include <stddef.h>

static inline bool is_heap_val(Value v) {
    uintptr_t tag = v & MINI_TAG_MASK;
    return tag != MINI_TAG_FIXNUM && tag != MINI_TAG_CONST;
}

static inline Value val_to_offset(Value v, char *base) {
    uintptr_t tag = v & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) {
        return v;
    }
    char *ptr = (char *)UNTAG_PTR(v);
    uintptr_t offset = (uintptr_t)(ptr - base);
    return (Value)(offset | tag);
}

static inline Value offset_to_val(uint64_t rel, char *base) {
    uintptr_t tag = rel & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) {
        return (Value)rel;
    }
    uintptr_t offset = rel & ~(uintptr_t)MINI_TAG_MASK;
    return (Value)(((uintptr_t)base + offset) | tag);
}

static Value unresolved_cfunc_stub(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    const char *name = "unknown";
    if (argv) {
        Value callee = argv[-1];
        if (mini_is_cfunc(callee)) {
            CFuncObj *cf = (CFuncObj *)UNTAG_PTR(callee);
            if (cf->name) name = cf->name;
        }
    }
    vm_error(vm, "attempt to call unresolved C function '%s' from FASL image", name);
    return mini_void();
}

static MiniCFunc resolve_cfunc(MiniVM *vm, const char *name) {
    /* Level 1: Built-in primitives table */
    for (size_t i = 0; BUILTIN_CFUNCS[i].name != NULL; i++) {
        if (strcmp(BUILTIN_CFUNCS[i].name, name) == 0) {
            return BUILTIN_CFUNCS[i].fn;
        }
    }

    /* Level 1b: Registered static primitive tables */
    const MiniPrimEntry *pe = prim_table_find(vm, name);
    if (pe) return pe->fn;

    /* Level 2: Host pre-registered C functions */
    for (size_t i = 0; i < vm->host_cfuncs_count; i++) {
        if (strcmp(vm->host_cfuncs[i].name, name) == 0) {
            return vm->host_cfuncs[i].fn;
        }
    }

    /* Level 3: Host dynamic resolver callback */
    if (vm->c_resolver) {
        MiniCFunc fn = vm->c_resolver(name, vm->c_resolver_data);
        if (fn) return fn;
    }

    /* Level 4: Unresolved safety stub */
    return unresolved_cfunc_stub;
}

static const char *vm_persist_name(MiniVM *vm, const char *name) {
    if (!name) return "";
    for (size_t i = 0; BUILTIN_CFUNCS[i].name != NULL; i++) {
        if (strcmp(BUILTIN_CFUNCS[i].name, name) == 0) {
            return BUILTIN_CFUNCS[i].name;
        }
    }
    for (size_t i = 0; i < vm->host_cfuncs_count; i++) {
        if (strcmp(vm->host_cfuncs[i].name, name) == 0) {
            return vm->host_cfuncs[i].name;
        }
    }
    for (size_t i = 0; i < vm->cfunc_names_count; i++) {
        if (strcmp(vm->cfunc_names[i], name) == 0) {
            return vm->cfunc_names[i];
        }
    }
    if (vm->cfunc_names_count >= vm->cfunc_names_cap) {
        size_t new_cap = vm->cfunc_names_cap ? vm->cfunc_names_cap * 2 : 16;
        char **new_names = (char **)realloc(vm->cfunc_names, new_cap * sizeof(char *));
        if (!new_names) return "";
        vm->cfunc_names = new_names;
        vm->cfunc_names_cap = new_cap;
    }
    char *dup = vm_strdup(name);
    if (!dup) return "";
    vm->cfunc_names[vm->cfunc_names_count++] = dup;
    return dup;
}

void mini_set_c_resolver(MiniVM *vm, MiniCResolver resolver, void *user_data) {
    if (!vm) return;
    vm->c_resolver = resolver;
    vm->c_resolver_data = user_data;
}

typedef struct {
    uint8_t  magic[4];           /* 'M', 'F', 'A', 'S' */
    uint32_t version;            /* 1 */
    uint32_t endian_check;       /* 0x01020304 */
    uint32_t flags;              /* pointer size: sizeof(Value) e.g. 4 or 8 */
    uint64_t heap_payload_bytes; /* size of live compacted heap */
    uint64_t global_env_offset;  /* relative Value of global_env */
    uint64_t symbols_offset;     /* relative Value of symbols */
    uint32_t cfunc_count;        /* number of C relocation entries */
    uint32_t payload_offset;     /* byte offset to 8-byte aligned heap payload */
} MiniFaslHeader;

/* Rewrites the pointer fields of one heap object between absolute addresses
 * and base-relative offsets. Dumping relocates an object in place, writes it
 * and relocates it back, so a snapshot needs no copy of the heap. */
static void fasl_reloc_object(HeapObj *obj, char *base, bool to_offset) {
#define FASL_RELOC(field) do { \
        if (is_heap_val(field)) field = to_offset ? val_to_offset(field, base) : offset_to_val(field, base); \
    } while (0)
    switch (obj->type & MINI_OBJ_TYPE_MASK) {
        case OBJ_PAIR: {
            PairObj *p = (PairObj *)obj;
            FASL_RELOC(p->car);
            FASL_RELOC(p->cdr);
            break;
        }
        case OBJ_CLOSURE: {
            ClosureObj *c = (ClosureObj *)obj;
            FASL_RELOC(c->code);
            FASL_RELOC(c->env);
            break;
        }
        case OBJ_CODE: {
            CodeObj *co = (CodeObj *)obj;
            Value *c_arr = CODE_CONSTS(co);
            for (uint16_t i = 0; i < co->num_consts; i++) FASL_RELOC(c_arr[i]);
            break;
        }
        case OBJ_VECTOR: {
            VectorObj *vec = (VectorObj *)obj;
            for (size_t i = 0; i < vec->length; i++) FASL_RELOC(vec->data[i]);
            break;
        }
        case OBJ_CONTINUATION: {
            ContinuationObj *cont = (ContinuationObj *)obj;
            SavedFrame *sf = CONT_FRAMES(cont);
            for (uint32_t i = 0; i < cont->frame_count; i++) {
                FASL_RELOC(sf[i].closure_val);
                FASL_RELOC(sf[i].code_val);
                FASL_RELOC(sf[i].env);
            }
            Value *stk = CONT_STACK(cont);
            for (uint32_t i = 0; i < cont->stack_len; i++) FASL_RELOC(stk[i]);
            break;
        }
        case OBJ_MACRO: {
            MacroObj *m = (MacroObj *)obj;
            FASL_RELOC(m->proc_or_literals);
            FASL_RELOC(m->rules);
            break;
        }
        default:
            break;
    }
#undef FASL_RELOC
}

/* ========================================================================= */
/* Portable FASL layout                                                      */
/* ========================================================================= */
/* A FASL payload is the compacted heap in one build's object layout; the
 * header names it by pointer width (flags). Every target is little-endian
 * with an 8-byte object header, so all 32-bit builds (ESP32, WASM, i386)
 * share one layout byte for byte. That 32-bit layout is the portable one:
 * 64-bit hosts dump it by default and convert images of the other width
 * on load, while 32-bit targets load portable images with a plain copy. */

#define FL_HDR          8u                             /* HeapObj */
#define FL_STR_SIZE(P)  ((P) == 4 ? 16u : 24u)         /* sizeof(StringObj), sizeof(SymbolObj) */
#define FL_CODE_HDR     20u                            /* sizeof(CodeObj): no pointer fields */
#define FL_FRAME(P)     ((P) == 4 ? 24u : 32u)         /* sizeof(SavedFrame) */
#define FL_CONT_HDR     24u                            /* sizeof(ContinuationObj) */
#define FL_MACRO(P)     ((P) == 4 ? 24u : 32u)         /* sizeof(MacroObj) */
#define FL_BIGINT_HDR   20u                            /* sizeof(BigIntObj) */

/* The converter hard-codes both layouts: fail the build if this one drifts */
#define FL_CHECK(name, cond) typedef char fl_check_##name[(cond) ? 1 : -1]
FL_CHECK(header, sizeof(HeapObj) == FL_HDR);
FL_CHECK(string, sizeof(StringObj) == FL_STR_SIZE(sizeof(Value)) &&
                 offsetof(StringObj, data) == FL_HDR + sizeof(Value));
FL_CHECK(symbol, sizeof(SymbolObj) == FL_STR_SIZE(sizeof(Value)) &&
                 offsetof(SymbolObj, name) == FL_HDR + sizeof(Value));
FL_CHECK(vector, sizeof(VectorObj) == FL_HDR + 2 * sizeof(Value) &&
                 offsetof(VectorObj, data) == FL_HDR + sizeof(Value));
FL_CHECK(code, sizeof(CodeObj) == FL_CODE_HDR && offsetof(CodeObj, num_consts) == 18);
FL_CHECK(frame, sizeof(SavedFrame) == FL_FRAME(sizeof(Value)) &&
                offsetof(SavedFrame, pc_offset) == 2 * sizeof(Value) &&
                offsetof(SavedFrame, env) == 2 * sizeof(Value) + 8);
FL_CHECK(cont, sizeof(ContinuationObj) == FL_CONT_HDR);
FL_CHECK(macro, sizeof(MacroObj) == FL_MACRO(sizeof(Value)) &&
                offsetof(MacroObj, proc_or_literals) == 16);
FL_CHECK(bigint, sizeof(BigIntObj) == FL_BIGINT_HDR && offsetof(BigIntObj, digits) == 16);
FL_CHECK(flonum, sizeof(FlonumObj) == 16 && offsetof(FlonumObj, val) == 8);

static uint64_t fl_rd(const uint8_t *p, unsigned n) {
    if (n == 4) {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void fl_wr(uint8_t *p, uint64_t v, unsigned n) {
    if (n == 4) {
        uint32_t w = (uint32_t)v;
        memcpy(p, &w, 4);
    } else {
        memcpy(p, &v, 8);
    }
}

static uint32_t fl_rd32(const uint8_t *p) { return (uint32_t)fl_rd(p, 4); }

/* Converts a payload from pointer width sp to dp. Values in the source are
 * base-relative offsets (a file image) or addresses in a live heap whose
 * start is live_base; the output is always offset-encoded. */
typedef struct {
    const uint8_t *src;
    size_t src_bytes;
    unsigned sp, dp;
    uintptr_t live_base;   /* 0 for an offset-encoded source */
    uint32_t *src_off;     /* object starts, ascending; [count] = src_bytes */
    uint32_t *dst_off;     /* matching output starts; [count] = output objects */
    size_t count;
    uint8_t *dst;          /* NULL while measuring */
    size_t extra_bytes;    /* bigints appended for fixnums too wide for dp */
    size_t extra_at;
    const char *error;
} FaslConv;

static int64_t fl_fixnum_min(unsigned p) { return p == 4 ? -((int64_t)1 << 28) : -((int64_t)1 << 60); }
static int64_t fl_fixnum_max(unsigned p) { return p == 4 ? ((int64_t)1 << 28) - 1 : ((int64_t)1 << 60) - 1; }

/* Size of an object in layout p, from the counts stored in its source copy */
static size_t fl_obj_size(const uint8_t *s, unsigned sp, unsigned p) {
    uint32_t type = fl_rd32(s) & MINI_OBJ_TYPE_MASK;
    switch (type) {
        case OBJ_PAIR:
        case OBJ_CLOSURE:
        case OBJ_CFUNC:
            return ALIGN8(FL_HDR + 2 * p);
        case OBJ_SYMBOL:
        case OBJ_STRING:
            return ALIGN8(FL_STR_SIZE(p) + (size_t)fl_rd(s + FL_HDR, sp));
        case OBJ_FLONUM:
            return 16;
        case OBJ_BIGINT: {
            uint32_t len = fl_rd32(s + 12);
            return ALIGN8(FL_BIGINT_HDR + (len > 1 ? (size_t)(len - 1) * 4 : 0));
        }
        case OBJ_VECTOR: {
            size_t len = (size_t)fl_rd(s + FL_HDR, sp);
            return ALIGN8(FL_HDR + 2 * p + (len > 1 ? (len - 1) * p : 0));
        }
        case OBJ_CODE: {
            uint32_t code_len = fl_rd32(s + 8);
            uint16_t nc;
            memcpy(&nc, s + 18, 2);
            return ALIGN8(FL_CODE_HDR + (size_t)nc * p + code_len);
        }
        case OBJ_CONTINUATION: {
            uint32_t fc = fl_rd32(s + 8), sl = fl_rd32(s + 12);
            return ALIGN8(FL_CONT_HDR + (size_t)fc * FL_FRAME(p) + (size_t)sl * p);
        }
        case OBJ_MACRO:
            return FL_MACRO(p);
        default:
            return 0;
    }
}

static bool fl_find(const FaslConv *c, uint64_t off, size_t *idx) {
    size_t lo = 0, hi = c->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->src_off[mid] < off) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= c->count || c->src_off[lo] != off) return false;
    *idx = lo;
    return true;
}

/* A source bigint that fits a dp fixnum (64-bit targets keep them normalized) */
static bool fl_small_bigint(const FaslConv *c, const uint8_t *s, int64_t *out) {
    int32_t sign;
    memcpy(&sign, s + 8, 4);
    uint32_t len = fl_rd32(s + 12);
    if (len > 2) return false;
    uint64_t mag = fl_rd32(s + 16);
    if (len == 2) mag |= (uint64_t)fl_rd32(s + 20) << 32;
    if (mag > (uint64_t)fl_fixnum_max(c->dp) + (sign < 0 ? 1u : 0u)) return false;
    *out = sign < 0 ? -(int64_t)mag : (int64_t)mag;
    return true;
}

static uint64_t fl_value(FaslConv *c, uint64_t r) {
    uint64_t mask = c->dp == 4 ? 0xFFFFFFFFu : ~(uint64_t)0;
    unsigned tag = (unsigned)(r & MINI_TAG_MASK);
    if (tag == MINI_TAG_CONST) return r & mask;
    if (tag == MINI_TAG_FIXNUM) {
        int64_t n = c->sp == 4 ? (int64_t)((int32_t)(uint32_t)r >> 3) : (int64_t)r >> 3;
        if (n >= fl_fixnum_min(c->dp) && n <= fl_fixnum_max(c->dp)) return ((uint64_t)n << 3) & mask;
        /* Too wide for a 32-bit fixnum: becomes a bigint appended to the image */
        uint64_t mag = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
        uint32_t len = (mag >> 32) ? 2 : 1;
        if (!c->dst) {
            c->extra_bytes += 24;
            return 0;
        }
        uint8_t *b = c->dst + c->extra_at;
        memset(b, 0, 24);
        uint32_t type = OBJ_BIGINT, size = 24;
        int32_t sign = n < 0 ? -1 : 1;
        memcpy(b, &type, 4);
        memcpy(b + 4, &size, 4);
        memcpy(b + 8, &sign, 4);
        memcpy(b + 12, &len, 4);
        fl_wr(b + 16, (uint32_t)mag, 4);
        fl_wr(b + 20, (uint32_t)(mag >> 32), 4);
        uint64_t res = (uint64_t)c->extra_at | MINI_TAG_RECORD;
        c->extra_at += 24;
        return res;
    }
    uint64_t off = (r & ~(uint64_t)MINI_TAG_MASK) - c->live_base;
    size_t idx;
    if (!fl_find(c, off, &idx)) {
        c->error = "FASL image holds a reference to no object";
        return 0;
    }
    const uint8_t *target = c->src + c->src_off[idx];
    if (c->dp == 8 && (fl_rd32(target) & MINI_OBJ_TYPE_MASK) == OBJ_BIGINT) {
        int64_t n;
        if (fl_small_bigint(c, target, &n)) return (uint64_t)n << 3;
    }
    return (uint64_t)c->dst_off[idx] | tag;
}

/* One value field: read at src offset so, written at dst offset dof */
#define FL_VAL(so, dof) do { \
        uint64_t v_ = fl_value(c, fl_rd(s + (so), sp)); \
        if (d) fl_wr(d + (dof), v_, dp); \
    } while (0)

static void fl_object(FaslConv *c, size_t i) {
    const uint8_t *s = c->src + c->src_off[i];
    unsigned sp = c->sp, dp = c->dp;
    uint8_t *d = c->dst ? c->dst + c->dst_off[i] : NULL;
    uint32_t dsize = c->dst_off[i + 1] - c->dst_off[i];
    uint32_t type = fl_rd32(s);
    if (d) {
        memset(d, 0, dsize);
        memcpy(d, &type, 4);
        memcpy(d + 4, &dsize, 4);
    }
    switch (type & MINI_OBJ_TYPE_MASK) {
        case OBJ_PAIR:
        case OBJ_CLOSURE:
            FL_VAL(FL_HDR, FL_HDR);
            FL_VAL(FL_HDR + sp, FL_HDR + dp);
            break;
        case OBJ_CFUNC:
            break; /* fn and name are rebound by name at load time */
        case OBJ_SYMBOL:
        case OBJ_STRING: {
            uint64_t len = fl_rd(s + FL_HDR, sp);
            if (d) {
                fl_wr(d + FL_HDR, len, dp);
                memcpy(d + FL_HDR + dp, s + FL_HDR + sp, (size_t)len + 1);
            }
            break;
        }
        case OBJ_FLONUM:
            if (d) memcpy(d + 8, s + 8, 8);
            break;
        case OBJ_BIGINT:
            if (d) memcpy(d + 8, s + 8, dsize - 8);
            break;
        case OBJ_VECTOR: {
            uint64_t len = fl_rd(s + FL_HDR, sp);
            if (d) fl_wr(d + FL_HDR, len, dp);
            for (uint64_t k = 0; k < len; k++) FL_VAL(FL_HDR + sp + k * sp, FL_HDR + dp + k * dp);
            break;
        }
        case OBJ_CODE: {
            uint32_t code_len = fl_rd32(s + 8);
            uint16_t nc;
            memcpy(&nc, s + 18, 2);
            if (d) memcpy(d + 8, s + 8, FL_CODE_HDR - 8);
            for (uint32_t k = 0; k < nc; k++) FL_VAL(FL_CODE_HDR + k * sp, FL_CODE_HDR + k * dp);
            if (d) memcpy(d + FL_CODE_HDR + (size_t)nc * dp, s + FL_CODE_HDR + (size_t)nc * sp, code_len);
            break;
        }
        case OBJ_CONTINUATION: {
            uint32_t fc = fl_rd32(s + 8), sl = fl_rd32(s + 12);
            if (d) memcpy(d + 8, s + 8, FL_CONT_HDR - 8);
            for (uint32_t f = 0; f < fc; f++) {
                size_t sf = FL_CONT_HDR + (size_t)f * FL_FRAME(sp), df = FL_CONT_HDR + (size_t)f * FL_FRAME(dp);
                FL_VAL(sf, df);                             /* closure_val */
                FL_VAL(sf + sp, df + dp);                   /* code_val */
                if (d) memcpy(d + df + 2 * dp, s + sf + 2 * sp, 8); /* pc_offset, fp_offset */
                FL_VAL(sf + 2 * sp + 8, df + 2 * dp + 8);   /* env */
            }
            size_t ss = FL_CONT_HDR + (size_t)fc * FL_FRAME(sp), ds = FL_CONT_HDR + (size_t)fc * FL_FRAME(dp);
            for (uint32_t k = 0; k < sl; k++) FL_VAL(ss + (size_t)k * sp, ds + (size_t)k * dp);
            break;
        }
        case OBJ_MACRO:
            if (d) memcpy(d + 8, s + 8, 4); /* is_syntax_rules */
            FL_VAL(16, 16);
            FL_VAL(16 + sp, 16 + dp);
            break;
        default:
            c->error = "FASL image holds an object type that cannot be converted";
            break;
    }
}
#undef FL_VAL

static void fl_conv_free(FaslConv *c) {
    free(c->src_off);
    free(c->dst_off);
    c->src_off = c->dst_off = NULL;
}

/* Indexes the source objects, lays out the output and checks every
 * reference. On success dst_off[count] + extra_bytes is the output size. */
static bool fl_conv_measure(FaslConv *c) {
    size_t n = 0;
    for (size_t off = 0; off < c->src_bytes; ) {
        const uint8_t *s = c->src + off;
        uint32_t size = off + FL_HDR <= c->src_bytes ? fl_rd32(s + 4) : 0;
        size_t need = size >= FL_HDR ? fl_obj_size(s, c->sp, c->sp) : 0;
        if (size < FL_HDR || off + ALIGN8(size) > c->src_bytes || need == 0 || ALIGN8(size) < need) {
            c->error = "corrupted heap object in FASL image";
            return false;
        }
        off += ALIGN8(size);
        n++;
    }
    c->count = n;
    c->src_off = (uint32_t *)malloc((n + 1) * sizeof(uint32_t));
    c->dst_off = (uint32_t *)malloc((n + 1) * sizeof(uint32_t));
    if (!c->src_off || !c->dst_off) {
        fl_conv_free(c);
        c->error = "out of memory converting FASL image";
        return false;
    }
    size_t off = 0, doff = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = c->src + off;
        c->src_off[i] = (uint32_t)off;
        c->dst_off[i] = (uint32_t)doff;
        doff += fl_obj_size(s, c->sp, c->dp);
        off += ALIGN8(fl_rd32(s + 4));
    }
    c->src_off[n] = (uint32_t)off;
    c->dst_off[n] = (uint32_t)doff;
    c->extra_bytes = 0;
    for (size_t i = 0; i < n && !c->error; i++) fl_object(c, i);
    if (c->error) {
        fl_conv_free(c);
        return false;
    }
    return true;
}

/* Writes the measured output to dst; cannot fail after fl_conv_measure */
static void fl_conv_emit(FaslConv *c, uint8_t *dst) {
    c->dst = dst;
    c->extra_at = c->dst_off[c->count];
    for (size_t i = 0; i < c->count; i++) fl_object(c, i);
}

/* Output offset of the source object at src offset off */
static bool fl_map_offset(const FaslConv *c, uint64_t off, uint64_t *out) {
    size_t idx;
    if (!fl_find(c, off, &idx)) return false;
    *out = c->dst_off[idx];
    return true;
}

/* Byte sink for a snapshot: a memory buffer or an open file */
typedef struct {
    uint8_t *buf; /* memory sink, or NULL for fp */
    FILE *fp;
    size_t pos;
} FaslSink;

static bool fasl_write(FaslSink *sink, const void *data, size_t len) {
    if (len == 0) return true;
    if (sink->buf) {
        memcpy(sink->buf + sink->pos, data, len);
    } else if (fwrite(data, 1, len, sink->fp) != len) {
        return false;
    }
    sink->pos += len;
    return true;
}

static bool fasl_write_zeros(FaslSink *sink, size_t len) {
    static const uint8_t zeros[8] = { 0 };
    while (len > 0) {
        size_t n = len < sizeof(zeros) ? len : sizeof(zeros);
        if (!fasl_write(sink, zeros, n)) return false;
        len -= n;
    }
    return true;
}

static size_t cfunc_name_len(const CFuncObj *cf) {
    size_t nlen = cf->name ? strlen(cf->name) : 0;
    return nlen > 65535 ? 65535 : nlen;
}

/* Compacts the heap and sizes the image: header, C relocations, payload */
static bool fasl_dump_prepare(MiniVM *vm, MiniFaslHeader *hdr, size_t *total) {
    if (vm->xrom_start) {
        vm_error(vm, "dump-fasl: not supported while a Code ROM image is attached");
        return false;
    }
    /* Compact all live objects to tenured_start */
    minor_gc(vm, 0);
    major_gc(vm, 0);
    if (vm->has_error) return false;

    char *base = vm->tenured_start;
    uint32_t cfunc_count = 0;
    size_t reloc_table_bytes = 0;
    for (char *scan = base; scan < vm->tenured_hp; scan += ALIGN8(((HeapObj *)scan)->size)) {
        HeapObj *obj = (HeapObj *)scan;
        if ((obj->type & MINI_OBJ_TYPE_MASK) == OBJ_CFUNC) {
            reloc_table_bytes += sizeof(uint64_t) + sizeof(uint16_t) + cfunc_name_len((CFuncObj *)obj);
            cfunc_count++;
        }
    }
    size_t payload_bytes = (size_t)(vm->tenured_hp - base);
    size_t payload_offset = ALIGN8(sizeof(MiniFaslHeader) + reloc_table_bytes);

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic[0] = 'M';
    hdr->magic[1] = 'F';
    hdr->magic[2] = 'A';
    hdr->magic[3] = 'S';
    hdr->version = 1;
    hdr->endian_check = 0x01020304;
    hdr->flags = (uint32_t)sizeof(Value);
    hdr->heap_payload_bytes = (uint64_t)payload_bytes;
    hdr->global_env_offset = (uint64_t)val_to_offset(vm->global_env, base);
    hdr->symbols_offset = (uint64_t)val_to_offset(vm->symbols, base);
    hdr->cfunc_count = cfunc_count;
    hdr->payload_offset = (uint32_t)payload_offset;
    *total = payload_offset + payload_bytes;
    return true;
}

/* Writes the image prepared by fasl_dump_prepare. Allocates nothing, so the
 * heap stays exactly as prepared. */
static bool fasl_dump_emit(MiniVM *vm, const MiniFaslHeader *hdr, FaslSink *sink) {
    char *base = vm->tenured_start;
    if (!fasl_write(sink, hdr, sizeof(*hdr))) return false;

    for (char *scan = base; scan < vm->tenured_hp; scan += ALIGN8(((HeapObj *)scan)->size)) {
        HeapObj *obj = (HeapObj *)scan;
        if ((obj->type & MINI_OBJ_TYPE_MASK) != OBJ_CFUNC) continue;
        CFuncObj *cf = (CFuncObj *)obj;
        uint64_t h_off = (uint64_t)(scan - base);
        uint16_t n_len = (uint16_t)cfunc_name_len(cf);
        if (!fasl_write(sink, &h_off, sizeof(h_off)) || !fasl_write(sink, &n_len, sizeof(n_len)) ||
            !fasl_write(sink, cf->name, n_len)) {
            return false;
        }
    }
    if (!fasl_write_zeros(sink, hdr->payload_offset - sink->pos)) return false;

    for (char *scan = base; scan < vm->tenured_hp; ) {
        HeapObj *obj = (HeapObj *)scan;
        size_t size = obj->size;
        bool ok;
        if ((obj->type & MINI_OBJ_TYPE_MASK) == OBJ_CFUNC) {
            /* C function pointers are rebound by name at load time */
            CFuncObj *cf = (CFuncObj *)obj;
            MiniCFunc fn = cf->fn;
            const char *name = cf->name;
            cf->fn = NULL;
            cf->name = NULL;
            ok = fasl_write(sink, obj, size);
            cf->fn = fn;
            cf->name = name;
        } else {
            fasl_reloc_object(obj, base, true);
            ok = fasl_write(sink, obj, size);
            fasl_reloc_object(obj, base, false);
        }
        if (!ok || !fasl_write_zeros(sink, ALIGN8(size) - size)) return false;
        scan += ALIGN8(size);
    }
    return true;
}

/* Portable dump from a 64-bit build: the heap converted to the 32-bit layout
 * into one buffer (hosts have the RAM; 32-bit targets stream instead) */
static bool fasl_dump_converted(MiniVM *vm, uint8_t **out_buf, size_t *out_size) {
    MiniFaslHeader hdr;
    size_t native_total = 0;
    if (!fasl_dump_prepare(vm, &hdr, &native_total)) return false;
    FaslConv c;
    memset(&c, 0, sizeof(c));
    c.src = (const uint8_t *)vm->tenured_start;
    c.src_bytes = (size_t)(vm->tenured_hp - vm->tenured_start);
    c.sp = (unsigned)sizeof(Value);
    c.dp = 4;
    c.live_base = (uintptr_t)vm->tenured_start;
    if (!fl_conv_measure(&c)) {
        vm_error(vm, "dump-fasl: %s", c.error);
        return false;
    }

    size_t reloc_bytes = 0;
    uint32_t cfunc_count = 0;
    for (size_t i = 0; i < c.count; i++) {
        const HeapObj *obj = (const HeapObj *)(c.src + c.src_off[i]);
        if ((obj->type & MINI_OBJ_TYPE_MASK) != OBJ_CFUNC) continue;
        reloc_bytes += sizeof(uint64_t) + sizeof(uint16_t) + cfunc_name_len((const CFuncObj *)obj);
        cfunc_count++;
    }
    size_t payload_bytes = c.dst_off[c.count] + c.extra_bytes;
    size_t payload_offset = ALIGN8(sizeof(MiniFaslHeader) + reloc_bytes);
    size_t total = payload_offset + payload_bytes;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) {
        fl_conv_free(&c);
        vm_error(vm, "out of memory allocating FASL buffer (%zu bytes)", total);
        return false;
    }

    uint8_t *cur = buf + sizeof(MiniFaslHeader);
    for (size_t i = 0; i < c.count; i++) {
        const HeapObj *obj = (const HeapObj *)(c.src + c.src_off[i]);
        if ((obj->type & MINI_OBJ_TYPE_MASK) != OBJ_CFUNC) continue;
        const CFuncObj *cf = (const CFuncObj *)obj;
        uint64_t h_off = c.dst_off[i];
        uint16_t n_len = (uint16_t)cfunc_name_len(cf);
        memcpy(cur, &h_off, sizeof(h_off));
        memcpy(cur + sizeof(h_off), &n_len, sizeof(n_len));
        if (n_len) memcpy(cur + sizeof(h_off) + sizeof(n_len), cf->name, n_len);
        cur += sizeof(h_off) + sizeof(n_len) + n_len;
    }
    fl_conv_emit(&c, buf + payload_offset);

    hdr.flags = 4;
    hdr.heap_payload_bytes = (uint64_t)payload_bytes;
    hdr.global_env_offset = fl_value(&c, (uint64_t)vm->global_env);
    hdr.symbols_offset = fl_value(&c, (uint64_t)vm->symbols);
    hdr.cfunc_count = cfunc_count;
    hdr.payload_offset = (uint32_t)payload_offset;
    memcpy(buf, &hdr, sizeof(hdr));
    fl_conv_free(&c);
    *out_buf = buf;
    *out_size = total;
    return true;
}

/* A portable dump from a 32-bit build is its native layout */
static bool fasl_dump_is_raw(MiniFaslLayout layout) {
    return layout == MINI_FASL_NATIVE || sizeof(Value) == 4;
}

bool mini_dump_fasl_mem_ex(MiniVM *vm, uint8_t **out_buf, size_t *out_size, MiniFaslLayout layout) {
    if (!vm || !out_buf || !out_size) return false;
    if (!fasl_dump_is_raw(layout)) return fasl_dump_converted(vm, out_buf, out_size);
    MiniFaslHeader hdr;
    size_t total = 0;
    if (!fasl_dump_prepare(vm, &hdr, &total)) return false;
    uint8_t *buf = (uint8_t *)malloc(total > 0 ? total : 1);
    if (!buf) {
        vm_error(vm, "out of memory allocating FASL buffer (%zu bytes)", total);
        return false;
    }
    FaslSink sink = { buf, NULL, 0 };
    fasl_dump_emit(vm, &hdr, &sink); /* a memory sink cannot fail */
    *out_buf = buf;
    *out_size = total;
    return true;
}

bool mini_dump_fasl_mem(MiniVM *vm, uint8_t **out_buf, size_t *out_size) {
    return mini_dump_fasl_mem_ex(vm, out_buf, out_size, MINI_FASL_PORTABLE);
}

/* Sequential FASL source: an in-memory image or an open file. Reading a file
 * this way puts the heap payload straight into the VM heap, so loading never
 * needs a RAM copy of the whole image. */
typedef struct {
    const uint8_t *buf; /* memory source, or NULL for fp */
    FILE *fp;
    size_t size;        /* total image bytes */
    size_t pos;         /* bytes consumed */
} FaslSrc;

static bool fasl_read(FaslSrc *src, void *dst, size_t len) {
    if (len > src->size - src->pos) return false;
    if (src->buf) {
        memcpy(dst, src->buf + src->pos, len);
    } else if (fread(dst, 1, len, src->fp) != len) {
        return false;
    }
    src->pos += len;
    return true;
}

static bool fasl_skip(FaslSrc *src, size_t len) {
    uint8_t tmp[64];
    while (len > 0) {
        size_t n = len < sizeof(tmp) ? len : sizeof(tmp);
        if (!fasl_read(src, tmp, n)) return false;
        len -= n;
    }
    return true;
}

/* Smallest free tenured space left after an image is loaded */
#define FASL_MIN_HEADROOM (8 * 1024)

static bool fasl_load(MiniVM *vm, FaslSrc *src) {
    MiniFaslHeader hdr;
    if (src->size < sizeof(MiniFaslHeader) || !fasl_read(src, &hdr, sizeof(hdr))) {
        vm_error(vm, "FASL data too short (%zu bytes)", src->size);
        return false;
    }

    if (hdr.magic[0] != 'M' || hdr.magic[1] != 'F' || hdr.magic[2] != 'A' || hdr.magic[3] != 'S') {
        vm_error(vm, "invalid FASL magic header");
        return false;
    }
    if (hdr.version != 1) {
        vm_error(vm, "unsupported FASL version %u", (unsigned int)hdr.version);
        return false;
    }
    if (hdr.endian_check != 0x01020304) {
        vm_error(vm, "endianness mismatch in FASL image");
        return false;
    }
    if (hdr.flags != 0 && hdr.flags != 4 && hdr.flags != 8) {
        vm_error(vm, "unsupported FASL heap layout (%u-byte pointers)", (unsigned)hdr.flags);
        return false;
    }
    /* An image of the other pointer width is converted while loading */
    bool convert = hdr.flags != 0 && hdr.flags != sizeof(Value);

    typedef struct {
        uint64_t heap_offset;
        char name[128];
    } LoadedCReloc;

    LoadedCReloc *c_relocs = NULL;
    if (hdr.cfunc_count > 0) {
        if (hdr.cfunc_count > (src->size - src->pos) / (sizeof(uint64_t) + sizeof(uint16_t))) {
            vm_error(vm, "truncated C relocation entry in FASL");
            return false;
        }
        c_relocs = (LoadedCReloc *)malloc(hdr.cfunc_count * sizeof(LoadedCReloc));
        if (!c_relocs) {
            vm_error(vm, "out of memory reading C relocation table");
            return false;
        }
        for (uint32_t i = 0; i < hdr.cfunc_count; i++) {
            uint64_t heap_offset;
            uint16_t name_len;
            if (!fasl_read(src, &heap_offset, sizeof(heap_offset)) ||
                !fasl_read(src, &name_len, sizeof(name_len))) {
                free(c_relocs);
                vm_error(vm, "truncated C relocation entry in FASL");
                return false;
            }
            size_t copy_len = name_len < sizeof(c_relocs[i].name) - 1 ? name_len : sizeof(c_relocs[i].name) - 1;
            if (!fasl_read(src, c_relocs[i].name, copy_len) || !fasl_skip(src, name_len - copy_len)) {
                free(c_relocs);
                vm_error(vm, "truncated C function name in FASL");
                return false;
            }
            c_relocs[i].heap_offset = heap_offset;
            c_relocs[i].name[copy_len] = '\0';
        }
    }

    /* Locate heap payload (support both 8-byte aligned and legacy contiguous formats) */
    if (hdr.payload_offset >= sizeof(MiniFaslHeader) && hdr.payload_offset < src->size) {
        if (hdr.payload_offset < src->pos || !fasl_skip(src, hdr.payload_offset - src->pos)) {
            free(c_relocs);
            vm_error(vm, "corrupted payload offset in FASL image");
            return false;
        }
    }
    size_t payload_bytes = (size_t)hdr.heap_payload_bytes;
    if (hdr.heap_payload_bytes > (uint64_t)(src->size - src->pos)) {
        free(c_relocs);
        vm_error(vm, "truncated heap payload in FASL image");
        return false;
    }

    /* Conversion reads the source payload into a scratch buffer first */
    FaslConv conv;
    memset(&conv, 0, sizeof(conv));
    uint8_t *conv_src = NULL;
    size_t image_bytes = payload_bytes;
    if (convert) {
        conv_src = (uint8_t *)malloc(payload_bytes > 0 ? payload_bytes : 1);
        if (!conv_src || !fasl_read(src, conv_src, payload_bytes)) {
            free(conv_src);
            free(c_relocs);
            vm_error(vm, conv_src ? "failed reading heap payload of FASL image"
                                  : "out of memory converting FASL image");
            return false;
        }
        conv.src = conv_src;
        conv.src_bytes = payload_bytes;
        conv.sp = (unsigned)hdr.flags;
        conv.dp = (unsigned)sizeof(Value);
        bool ok = fl_conv_measure(&conv);
        for (uint32_t i = 0; ok && i < hdr.cfunc_count; i++) {
            ok = fl_map_offset(&conv, c_relocs[i].heap_offset, &c_relocs[i].heap_offset);
            if (!ok) conv.error = "C relocation names no object in FASL image";
        }
        if (!ok) {
            fl_conv_free(&conv);
            free(conv_src);
            free(c_relocs);
            vm_error(vm, "%s", conv.error);
            return false;
        }
        image_bytes = conv.dst_off[conv.count] + conv.extra_bytes;
    }

    /* Size the heap: an image that fits the current heap with some headroom
     * keeps it (device VMs have a fixed heap and no RAM for a second one);
     * otherwise the heap grows to fit the image with working headroom. */
    size_t needed_cap = vm->tenured_size;
    if (image_bytes + FASL_MIN_HEADROOM > vm->tenured_size) {
        needed_cap = 384 * 1024;
        if (needed_cap < ALIGN8(image_bytes + 64 * 1024)) {
            needed_cap = ALIGN8(image_bytes + 64 * 1024);
        }
        if (needed_cap < vm->heap_size) needed_cap = vm->heap_size;
    }

    if (vm->tenured_size < needed_cap) {
        char *new_tenured_raw = NULL;
        char *new_tenured = alloc_aligned8(needed_cap, &new_tenured_raw);
        if (!new_tenured) {
            free(new_tenured_raw);
            fl_conv_free(&conv);
            free(conv_src);
            free(c_relocs);
            vm_error(vm, "out of memory resizing heap for FASL image");
            return false;
        }
        free(vm->tenured_raw);
        vm->tenured_start = new_tenured;
        vm->tenured_raw = new_tenured_raw;
        vm->tenured_size = needed_cap;
        vm->heap_size = needed_cap;
        if (vm->max_memory_bytes > 0 && vm->max_memory_bytes < needed_cap) {
            vm->max_memory_bytes = needed_cap;
        }
    }

    /* Read the heap payload straight into the tenured space (or convert it
     * there). Past this point the old heap is gone, so a failed read leaves
     * an empty VM. */
    if (convert) {
        fl_conv_emit(&conv, (uint8_t *)vm->tenured_start);
        hdr.global_env_offset = fl_value(&conv, hdr.global_env_offset);
        hdr.symbols_offset = fl_value(&conv, hdr.symbols_offset);
        hdr.heap_payload_bytes = (uint64_t)image_bytes;
        fl_conv_free(&conv);
        free(conv_src);
    } else if (payload_bytes > 0 && !fasl_read(src, vm->tenured_start, payload_bytes)) {
        free(c_relocs);
        vm->tenured_hp = vm->tenured_start;
        vm->global_env = mini_nil();
        vm->symbols = mini_nil();
        mini_globals_reset(vm);
        vm_error(vm, "failed reading heap payload of FASL image");
        return false;
    }

    vm->tenured_hp = vm->tenured_start + image_bytes;
    vm->tenured_limit = vm->tenured_start + vm->tenured_size;
    vm->eden_hp = vm->eden_start;
    vm->remset_count = 0;

    /* Legacy compatibility pointers */
    vm->to_space = vm->tenured_start;
    vm->to_space_raw = vm->tenured_raw;
    vm->hp = vm->tenured_hp;
    vm->hp_limit = vm->tenured_limit;

    /* The snapshot replaces the whole heap and never points into a Code ROM
     * (dump-fasl refuses while one is attached), so detach any image: its
     * symbols must not shadow the snapshot's own. */
    vm->xrom_start = NULL;
    vm->xrom_end = NULL;
    vm->xrom_syms = NULL;
    vm->xrom_sym_count = 0;

    char *new_base = vm->tenured_start;

    /* Relocate pointers in the loaded heap */
    char *scan = new_base;
    while (scan < vm->tenured_hp) {
        HeapObj *obj = (HeapObj *)scan;
        if (obj->size < sizeof(HeapObj) || scan + obj->size > vm->tenured_hp) {
            free(c_relocs);
            vm_error(vm, "corrupted heap object size in FASL image");
            return false;
        }

        switch (obj->type & MINI_OBJ_TYPE_MASK) {
            case OBJ_PAIR: {
                PairObj *p = (PairObj *)obj;
                if (is_heap_val(p->car)) p->car = offset_to_val(p->car, new_base);
                if (is_heap_val(p->cdr)) p->cdr = offset_to_val(p->cdr, new_base);
                break;
            }
            case OBJ_CLOSURE: {
                ClosureObj *c = (ClosureObj *)obj;
                if (is_heap_val(c->code)) c->code = offset_to_val(c->code, new_base);
                if (is_heap_val(c->env))  c->env  = offset_to_val(c->env, new_base);
                break;
            }
            case OBJ_CODE: {
                CodeObj *co = (CodeObj *)obj;
                Value *c_arr = CODE_CONSTS(co);
                for (uint16_t i = 0; i < co->num_consts; i++) {
                    if (is_heap_val(c_arr[i])) c_arr[i] = offset_to_val(c_arr[i], new_base);
                }
                break;
            }
            case OBJ_VECTOR: {
                VectorObj *vec = (VectorObj *)obj;
                for (size_t i = 0; i < vec->length; i++) {
                    if (is_heap_val(vec->data[i])) vec->data[i] = offset_to_val(vec->data[i], new_base);
                }
                break;
            }
            case OBJ_CONTINUATION: {
                ContinuationObj *cont = (ContinuationObj *)obj;
                SavedFrame *sf = CONT_FRAMES(cont);
                for (uint32_t i = 0; i < cont->frame_count; i++) {
                    if (is_heap_val(sf[i].closure_val)) sf[i].closure_val = offset_to_val(sf[i].closure_val, new_base);
                    if (is_heap_val(sf[i].code_val))    sf[i].code_val    = offset_to_val(sf[i].code_val, new_base);
                    if (is_heap_val(sf[i].env))         sf[i].env         = offset_to_val(sf[i].env, new_base);
                }
                Value *stk = CONT_STACK(cont);
                for (uint32_t i = 0; i < cont->stack_len; i++) {
                    if (is_heap_val(stk[i])) stk[i] = offset_to_val(stk[i], new_base);
                }
                break;
            }
            case OBJ_MACRO: {
                MacroObj *m = (MacroObj *)obj;
                if (is_heap_val(m->proc_or_literals)) m->proc_or_literals = offset_to_val(m->proc_or_literals, new_base);
                if (is_heap_val(m->rules)) m->rules = offset_to_val(m->rules, new_base);
                break;
            }
            case OBJ_CFUNC:
            case OBJ_SYMBOL:
            case OBJ_STRING:
            case OBJ_FLONUM:
            case OBJ_BIGINT:
                break;
            default:
                break;
        }
        scan += ALIGN8(obj->size);
    }

    /* Relocate and patch C functions */
    for (uint32_t i = 0; i < hdr.cfunc_count; i++) {
        if (c_relocs[i].heap_offset + sizeof(CFuncObj) <= hdr.heap_payload_bytes) {
            CFuncObj *cf = (CFuncObj *)(new_base + c_relocs[i].heap_offset);
            cf->fn = resolve_cfunc(vm, c_relocs[i].name);
            cf->name = vm_persist_name(vm, c_relocs[i].name);
        }
    }
    free(c_relocs);

    /* Restore VM roots */
    vm->global_env = offset_to_val(hdr.global_env_offset, new_base);
    mini_globals_reset(vm);
    vm->symbols    = offset_to_val(hdr.symbols_offset, new_base);

    /* Re-bind fast internal symbol references */
    vm->sym_quote   = mini_make_symbol(vm, "quote");
    vm->sym_if      = mini_make_symbol(vm, "if");
    vm->sym_begin   = mini_make_symbol(vm, "begin");
    vm->sym_lambda  = mini_make_symbol(vm, "lambda");
    vm->sym_define  = mini_make_symbol(vm, "define");
    vm->sym_set     = mini_make_symbol(vm, "set!");
    vm->sym_let     = mini_make_symbol(vm, "let");
    vm->sym_letstar = mini_make_symbol(vm, "let*");
    vm->sym_letrec  = mini_make_symbol(vm, "letrec");

    vm->sym_define_syntax = mini_make_symbol(vm, "define-syntax");
    vm->sym_syntax_rules  = mini_make_symbol(vm, "syntax-rules");
    vm->sym_define_macro  = mini_make_symbol(vm, "define-macro");
    vm->sym_ellipsis      = mini_make_symbol(vm, "...");
    vm->sym_wildcard      = mini_make_symbol(vm, "_");

    /* Primitives missing from the image need no eager registration: global
     * lookups fall back to the VM's registered (lazy) primitive tables. */

    /* Reset evaluation stack and frames */
    vm->sp = vm->stack;
    vm->call_frame_count = 0;
    vm->c_roots_count = 0;
    vm->state_reloaded = true;

    return true;
}

bool mini_load_fasl_mem(MiniVM *vm, const uint8_t *buf, size_t size) {
    if (!vm || !buf) return false;
    FaslSrc src = { buf, NULL, size, 0 };
    return fasl_load(vm, &src);
}

bool mini_dump_fasl_fp_ex(MiniVM *vm, FILE *fp, MiniFaslLayout layout) {
    if (!vm || !fp) return false;
    if (!fasl_dump_is_raw(layout)) {
        uint8_t *buf = NULL;
        size_t size = 0;
        if (!fasl_dump_converted(vm, &buf, &size)) return false;
        bool ok = fwrite(buf, 1, size, fp) == size;
        free(buf);
        if (!ok) vm_error(vm, "failed writing FASL image (%zu bytes)", size);
        return ok;
    }
    MiniFaslHeader hdr;
    size_t total = 0;
    if (!fasl_dump_prepare(vm, &hdr, &total)) return false;
    FaslSink sink = { NULL, fp, 0 };
    if (!fasl_dump_emit(vm, &hdr, &sink)) {
        vm_error(vm, "failed writing FASL image (%zu of %zu bytes)", sink.pos, total);
        return false;
    }
    return true;
}

bool mini_dump_fasl_fp(MiniVM *vm, FILE *fp) {
    return mini_dump_fasl_fp_ex(vm, fp, MINI_FASL_PORTABLE);
}

bool mini_dump_fasl_ex(MiniVM *vm, const char *filepath, MiniFaslLayout layout) {
    if (!vm || !filepath) return false;
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        vm_error(vm, "cannot open file for FASL dump: %s", filepath);
        return false;
    }
    bool ok = mini_dump_fasl_fp_ex(vm, fp, layout);
    if (fclose(fp) != 0 && ok) {
        vm_error(vm, "failed to write entire FASL file: %s", filepath);
        ok = false;
    }
    if (!ok) remove(filepath); /* never leave a partial image behind */
    return ok;
}

bool mini_dump_fasl(MiniVM *vm, const char *filepath) {
    return mini_dump_fasl_ex(vm, filepath, MINI_FASL_PORTABLE);
}

bool mini_load_fasl_fp(MiniVM *vm, FILE *fp) {
    if (!vm || !fp) return false;
    long start = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long end = ftell(fp);
    fseek(fp, start, SEEK_SET);
    if (start < 0 || end < start) {
        vm_error(vm, "cannot size FASL file");
        return false;
    }
    FaslSrc src = { NULL, fp, (size_t)(end - start), 0 };
    return fasl_load(vm, &src);
}

bool mini_load_fasl(MiniVM *vm, const char *filepath) {
    if (!vm || !filepath) return false;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        vm_error(vm, "cannot open file for FASL load: %s", filepath);
        return false;
    }
    bool ok = mini_load_fasl_fp(vm, fp);
    fclose(fp);
    return ok;
}

/* ========================================================================= */
/* Execute-In-Place (XIP) & Demand-Paged FASL Execution Engine               */
/* ========================================================================= */

const char *rom_find_cfunc_name(MiniVM *vm, uint64_t heap_offset) {
    if (!vm->rom_cfunc_table || vm->rom_cfunc_count == 0) return NULL;
    const uint8_t *cur = vm->rom_cfunc_table;
    for (uint32_t i = 0; i < vm->rom_cfunc_count; i++) {
        uint64_t off;
        uint16_t nlen;
        memcpy(&off, cur, sizeof(uint64_t));
        cur += sizeof(uint64_t);
        memcpy(&nlen, cur, sizeof(uint16_t));
        cur += sizeof(uint16_t);
        if (off == heap_offset) {
            char tmp[256];
            size_t copy_len = nlen < sizeof(tmp) - 1 ? nlen : sizeof(tmp) - 1;
            memcpy(tmp, cur, copy_len);
            tmp[copy_len] = '\0';
            return vm_persist_name(vm, tmp);
        }
        cur += nlen;
    }
    return NULL;
}

Value rom_materialize_val(MiniVM *vm, uint64_t raw_val) {
    uintptr_t tag = raw_val & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) {
        return (Value)raw_val;
    }
    uint64_t off = raw_val & ~(uintptr_t)MINI_TAG_MASK;
    if (off >= vm->rom_size) return mini_void();

    const char *ptr = vm->rom_base + off;
    const HeapObj *h = (const HeapObj *)ptr;

    switch (h->type & MINI_OBJ_TYPE_MASK) {
        case OBJ_CODE:
        case OBJ_SYMBOL:
        case OBJ_STRING:
        case OBJ_FLONUM:
        case OBJ_BIGINT:
            /* Zero RAM copy: pointer directly references read-only ROM / Flash */
            return ((uintptr_t)ptr) | tag;

        case OBJ_CLOSURE: {
            const ClosureObj *c = (const ClosureObj *)ptr;
            Value code_val = rom_materialize_val(vm, c->code);
            Value env_val = (c->env == MINI_CONST_NIL) ? mini_nil() : rom_materialize_val(vm, c->env);
            return mini_make_closure(vm, code_val, env_val);
        }
        case OBJ_CFUNC: {
            const char *cname = rom_find_cfunc_name(vm, off);
            MiniCFunc fn = resolve_cfunc(vm, cname ? cname : "");
            return make_cfunc(vm, fn, cname ? cname : "unknown");
        }
        case OBJ_MACRO: {
            const MacroObj *m = (const MacroObj *)ptr;
            Value proc_lit = rom_materialize_val(vm, m->proc_or_literals);
            mini_push_root(vm, &proc_lit);
            Value rules = rom_materialize_val(vm, m->rules);
            mini_pop_root(vm);
            return mini_make_macro(vm, m->is_syntax_rules, proc_lit, rules);
        }
        case OBJ_PAIR: {
            const PairObj *p = (const PairObj *)ptr;
            Value car_val = rom_materialize_val(vm, p->car);
            mini_push_root(vm, &car_val);
            Value cdr_val = rom_materialize_val(vm, p->cdr);
            mini_pop_root(vm);
            return mini_make_cons(vm, car_val, cdr_val);
        }
        case OBJ_VECTOR: {
            const VectorObj *v = (const VectorObj *)ptr;
            Value vec = mini_make_vector(vm, v->length, mini_void());
            mini_push_root(vm, &vec);
            for (size_t i = 0; i < v->length; i++) {
                Value elem = rom_materialize_val(vm, v->data[i]);
                mini_vector_set(vm, vec, i, elem);
            }
            mini_pop_root(vm);
            return vec;
        }
        default:
            return ((uintptr_t)ptr) | tag;
    }
}

Value rom_on_demand_load(MiniVM *vm, Value sym) {
    if (!vm || !vm->rom_base) return mini_void();

    const char *target_name = mini_symbol_name(sym);
    if (!target_name || !*target_name) return mini_void();
    size_t target_len = strlen(target_name);

    uint64_t curr_off = vm->rom_global_env_offset & ~(uintptr_t)MINI_TAG_MASK;
    if (curr_off < vm->rom_size) {
        const PairObj *head = (const PairObj *)(vm->rom_base + curr_off);
        if (head->header.type == OBJ_PAIR && head->car == MINI_CONST_NIL) {
            curr_off = head->cdr & ~(uintptr_t)MINI_TAG_MASK;
        }
    }

    while (curr_off < vm->rom_size) {
        const PairObj *spine = (const PairObj *)(vm->rom_base + curr_off);
        if (spine->header.type != OBJ_PAIR) break;

        uint64_t binding_val = spine->car;
        if ((binding_val & MINI_TAG_MASK) == MINI_TAG_PAIR) {
            uint64_t b_off = binding_val & ~(uintptr_t)MINI_TAG_MASK;
            if (b_off < vm->rom_size) {
                const PairObj *binding = (const PairObj *)(vm->rom_base + b_off);
                uint64_t s_val = binding->car;
                if ((s_val & MINI_TAG_MASK) == MINI_TAG_SYMBOL) {
                    uint64_t s_off = s_val & ~(uintptr_t)MINI_TAG_MASK;
                    if (s_off < vm->rom_size) {
                        const SymbolObj *s_obj = (const SymbolObj *)(vm->rom_base + s_off);
                        if (s_obj->len == target_len && memcmp(s_obj->name, target_name, target_len) == 0) {
                            Value val = rom_materialize_val(vm, binding->cdr);
                            mini_push_root(vm, &val);
                            env_define(vm, vm->global_env, sym, val);
                            mini_pop_root(vm);
                            return val;
                        }
                    }
                }
            }
        }

        uint64_t next_val = spine->cdr;
        if ((next_val & MINI_TAG_MASK) == MINI_TAG_PAIR) {
            curr_off = next_val & ~(uintptr_t)MINI_TAG_MASK;
        } else {
            break;
        }
    }
    return mini_void();
}

bool mini_load_fasl_xip(MiniVM *vm, const uint8_t *rom_buf, size_t rom_size) {
    if (!vm || !rom_buf) return false;

    if (rom_size < sizeof(MiniFaslHeader)) {
        vm_error(vm, "XIP FASL data too short (%zu bytes)", rom_size);
        return false;
    }

    MiniFaslHeader hdr;
    memcpy(&hdr, rom_buf, sizeof(MiniFaslHeader));

    if (hdr.magic[0] != 'M' || hdr.magic[1] != 'F' || hdr.magic[2] != 'A' || hdr.magic[3] != 'S') {
        vm_error(vm, "invalid FASL magic header in XIP image");
        return false;
    }
    if (hdr.version != 1) {
        vm_error(vm, "unsupported FASL version %u", (unsigned int)hdr.version);
        return false;
    }
    if (hdr.endian_check != 0x01020304) {
        vm_error(vm, "endianness mismatch in XIP FASL image");
        return false;
    }
    if (hdr.flags != 0 && hdr.flags != sizeof(Value)) {
        vm_error(vm, "pointer size mismatch in XIP FASL image: image is %u-bit, VM is %u-bit "
                 "(XIP needs (dump-fasl path 'native) from a %u-bit build)",
                 (unsigned)(hdr.flags * 8), (unsigned)(sizeof(Value) * 8), (unsigned)(sizeof(Value) * 8));
        return false;
    }

    const uint8_t *cur = rom_buf + sizeof(MiniFaslHeader);
    const uint8_t *cfunc_table = cur;
    uint32_t cfunc_count = hdr.cfunc_count;

    for (uint32_t i = 0; i < cfunc_count; i++) {
        if (cur + sizeof(uint64_t) + sizeof(uint16_t) > rom_buf + rom_size) {
            vm_error(vm, "truncated C relocations in XIP FASL image");
            return false;
        }
        cur += sizeof(uint64_t);
        uint16_t name_len;
        memcpy(&name_len, cur, sizeof(uint16_t));
        cur += sizeof(uint16_t) + name_len;
    }

    const uint8_t *payload_ptr = cur;
    if (hdr.payload_offset >= sizeof(MiniFaslHeader) && hdr.payload_offset < rom_size) {
        payload_ptr = rom_buf + hdr.payload_offset;
    }

    if (payload_ptr + hdr.heap_payload_bytes > rom_buf + rom_size) {
        vm_error(vm, "truncated heap payload in XIP FASL image");
        return false;
    }

    /* Set up XIP ROM execution context: zero-copy in-place if 8-byte aligned */
    if (((uintptr_t)payload_ptr % 8) == 0) {
        vm->rom_base = (const char *)payload_ptr;
        vm->rom_owned_buffer = NULL;
    } else {
        /* Safety fallback for unaligned legacy FASL: allocate aligned copy */
        char *aligned_copy = (char *)malloc((size_t)hdr.heap_payload_bytes);
        if (!aligned_copy) {
            vm_error(vm, "out of memory allocating aligned XIP buffer");
            return false;
        }
        memcpy(aligned_copy, payload_ptr, (size_t)hdr.heap_payload_bytes);
        vm->rom_base = aligned_copy;
        vm->rom_owned_buffer = (uint8_t *)aligned_copy;
    }

    vm->rom_size = (size_t)hdr.heap_payload_bytes;
    vm->rom_end  = vm->rom_base + vm->rom_size;
    vm->rom_cfunc_table = cfunc_table;
    vm->rom_cfunc_count = cfunc_count;
    vm->rom_global_env_offset = hdr.global_env_offset & ~(uint64_t)MINI_TAG_MASK;
    vm->rom_symbols_offset = hdr.symbols_offset & ~(uint64_t)MINI_TAG_MASK;

    /* Reset symbols and global env so all bindings use unified ROM symbol pointers */
    vm->symbols = mini_nil();
    vm->global_env = mini_make_cons(vm, mini_nil(), mini_nil());
    mini_globals_reset(vm);

    /* Re-bind fast internal symbol references from ROM */
    vm->sym_quote   = mini_make_symbol(vm, "quote");
    vm->sym_if      = mini_make_symbol(vm, "if");
    vm->sym_begin   = mini_make_symbol(vm, "begin");
    vm->sym_lambda  = mini_make_symbol(vm, "lambda");
    vm->sym_define  = mini_make_symbol(vm, "define");
    vm->sym_set     = mini_make_symbol(vm, "set!");
    vm->sym_let     = mini_make_symbol(vm, "let");
    vm->sym_letstar = mini_make_symbol(vm, "let*");
    vm->sym_letrec  = mini_make_symbol(vm, "letrec");

    vm->sym_define_syntax = mini_make_symbol(vm, "define-syntax");
    vm->sym_syntax_rules  = mini_make_symbol(vm, "syntax-rules");
    vm->sym_define_macro  = mini_make_symbol(vm, "define-macro");
    vm->sym_ellipsis      = mini_make_symbol(vm, "...");
    vm->sym_wildcard      = mini_make_symbol(vm, "_");

    /* Primitives need no re-registration: the static tables stay registered
     * and are consulted after the ROM image (see global_miss) */

    /* Reset evaluation stack and frames */
    vm->sp = vm->stack;
    vm->call_frame_count = 0;
    vm->c_roots_count = 0;
    vm->state_reloaded = true;

    return true;
}

bool mini_load_fasl_file_xip(MiniVM *vm, const char *filepath) {
    if (!vm || !filepath) return false;
    char path_buf[512];
    strncpy(path_buf, filepath, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    FILE *fp = fopen(path_buf, "rb");
    if (!fp) {
        vm_error(vm, "cannot open file for XIP FASL load: %s", path_buf);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < (long)sizeof(MiniFaslHeader)) {
        fclose(fp);
        vm_error(vm, "XIP FASL file is too short or empty: %s", path_buf);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        vm_error(vm, "out of memory reading XIP FASL file: %s", path_buf);
        return false;
    }

    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        vm_error(vm, "failed reading XIP FASL file: %s", path_buf);
        return false;
    }
    fclose(fp);

    if (vm->rom_owned_buffer) {
        free(vm->rom_owned_buffer);
        vm->rom_owned_buffer = NULL;
    }

    bool ok = mini_load_fasl_xip(vm, buf, (size_t)sz);
    if (ok) {
        vm->rom_owned_buffer = buf;
    } else {
        free(buf);
    }
    return ok;
}

bool mini_is_xip_active(const MiniVM *vm) {
    return vm && vm->rom_base != NULL && vm->rom_size > 0;
}

Value prim_load_fasl_xip(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "load-fasl-xip")) return mini_void();
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "load-fasl-xip requires 1 string argument (filepath)");
        return mini_void();
    }
    char path[512];
    size_t len = mini_string_len(argv[0]);
    if (len >= sizeof(path)) len = sizeof(path) - 1;
    memcpy(path, mini_string_data(argv[0]), len);
    path[len] = '\0';
    if (!mini_load_fasl_file_xip(vm, path)) {
        return mini_false();
    }
    return mini_true();
}

Value prim_xip_active_p(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_bool(mini_is_xip_active(vm));
}

Value prim_xip_stats(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!mini_is_xip_active(vm)) return mini_nil();

    Value res = mini_nil();
    mini_push_root(vm, &res);

    Value p1 = mini_make_cons(vm, mini_make_symbol(vm, "rom-base"), mini_make_fixnum((int64_t)(uintptr_t)vm->rom_base));
    res = mini_make_cons(vm, p1, res);

    Value p2 = mini_make_cons(vm, mini_make_symbol(vm, "rom-size"), mini_make_fixnum((int64_t)vm->rom_size));
    res = mini_make_cons(vm, p2, res);

    Value p3 = mini_make_cons(vm, mini_make_symbol(vm, "cfuncs"), mini_make_fixnum((int64_t)vm->rom_cfunc_count));
    res = mini_make_cons(vm, p3, res);

    mini_pop_root(vm);
    return res;
}

Value prim_dump_fasl(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "dump-fasl")) return mini_void();
    if (argc < 1 || argc > 2 || !mini_is_string(argv[0])) {
        vm_error(vm, "dump-fasl requires a string argument (filepath) and an optional layout symbol");
        return mini_void();
    }
    MiniFaslLayout layout = MINI_FASL_PORTABLE;
    if (argc == 2) {
        const char *name = mini_is_symbol(argv[1]) ? mini_symbol_name(argv[1]) : "";
        if (strcmp(name, "native") == 0) {
            layout = MINI_FASL_NATIVE;
        } else if (strcmp(name, "portable") != 0) {
            vm_error(vm, "dump-fasl: layout must be 'portable or 'native");
            return mini_void();
        }
    }
    char path[512];
    size_t len = mini_string_len(argv[0]);
    if (len >= sizeof(path)) len = sizeof(path) - 1;
    memcpy(path, mini_string_data(argv[0]), len);
    path[len] = '\0';
    if (!mini_dump_fasl_ex(vm, path, layout)) {
        return mini_false();
    }
    return mini_true();
}

Value prim_load_fasl(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "load-fasl")) return mini_void();
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "load-fasl requires 1 string argument (filepath)");
        return mini_void();
    }
    char path[512];
    size_t len = mini_string_len(argv[0]);
    if (len >= sizeof(path)) len = sizeof(path) - 1;
    memcpy(path, mini_string_data(argv[0]), len);
    path[len] = '\0';
    if (!mini_load_fasl(vm, path)) {
        return mini_false();
    }
    return mini_true();
}

Value prim_fasl_to_c(MiniVM *vm, int argc, Value *argv) {
    if (argc < 4 || !mini_is_string(argv[0]) || !mini_is_string(argv[1]) ||
        !mini_is_string(argv[2]) || !mini_is_bool(argv[3])) {
        vm_error(vm, "fasl-to-c: expected (fasl-path out-path sym-name is-exe)");
        return mini_false();
    }
    const char *fasl_path = mini_string_data(argv[0]);
    const char *out_path  = mini_string_data(argv[1]);
    const char *sym_name  = mini_string_data(argv[2]);
    bool is_exe           = mini_is_true(argv[3]);

    FILE *fp = fopen(fasl_path, "rb");
    if (!fp) {
        vm_error(vm, "fasl-to-c: cannot open '%s' for reading", fasl_path);
        return mini_false();
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return mini_false(); }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return mini_false(); }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return mini_false();
    }
    fclose(fp);

    FILE *out = fopen(out_path, "w");
    if (!out) {
        free(buf);
        vm_error(vm, "fasl-to-c: cannot create '%s'", out_path);
        return mini_false();
    }

    char guard[256];
    size_t g_idx = 0;
    for (size_t i = 0; out_path[i] && g_idx < sizeof(guard) - 1; i++) {
        char c = out_path[i];
        guard[g_idx++] = isalnum((unsigned char)c) ? (char)toupper((unsigned char)c) : '_';
    }
    guard[g_idx] = '\0';

    fprintf(out, "/* Auto-generated by minischemec from '%s'. DO NOT EDIT. */\n", fasl_path);
    if (!is_exe) {
        fprintf(out, "#ifndef %s\n#define %s\n\n", guard, guard);
    }
    fprintf(out, "#include <stdint.h>\n#include <stddef.h>\n\n");
    fprintf(out, "#if defined(__GNUC__) || defined(__clang__)\n");
    fprintf(out, "#define FASL_ALIGNED __attribute__((aligned(8)))\n");
    fprintf(out, "#elif defined(_MSC_VER)\n");
    fprintf(out, "#define FASL_ALIGNED __declspec(align(8))\n");
    fprintf(out, "#else\n#define FASL_ALIGNED\n#endif\n\n");
    if (!is_exe) {
        fprintf(out, "#if defined(__AVR__) || defined(ESP8266) || defined(ESP32)\n");
        fprintf(out, "#include <pgmspace.h>\n#define FASL_STORAGE PROGMEM\n");
        fprintf(out, "#else\n#define FASL_STORAGE\n#endif\n\n");
        fprintf(out, "static const uint8_t %s[] FASL_ALIGNED FASL_STORAGE = {\n", sym_name);
    } else {
        fprintf(out, "static const uint8_t %s[] FASL_ALIGNED = {\n", sym_name);
    }

    for (size_t i = 0; i < (size_t)sz; i++) {
        if (i % 16 == 0) fprintf(out, "    ");
        fprintf(out, "0x%02x", buf[i]);
        if (i + 1 < (size_t)sz) fprintf(out, ", ");
        if ((i + 1) % 16 == 0 || i + 1 == (size_t)sz) fprintf(out, "\n");
    }
    fprintf(out, "};\n\nstatic const size_t %s_len = %zu;\n\n", sym_name, (size_t)sz);

    if (is_exe) {
        fprintf(out, "#include \"minischeme.h\"\n#include \"minischeme_lib.h\"\n");
        fprintf(out, "#include <stdio.h>\n#include <stdlib.h>\n\n");
        fprintf(out, "int main(int argc, char **argv) {\n");
        fprintf(out, "    MiniVM *vm = mini_create(0);\n");
        fprintf(out, "    if (!vm) {\n        fprintf(stderr, \"Error: failed to initialize MiniScheme VM\\n\");\n        return 1;\n    }\n");
        fprintf(out, "    mini_register_hardware_lib(vm);\n");
        fprintf(out, "    if (!mini_load_fasl_mem(vm, %s, %s_len)) {\n", sym_name, sym_name);
        fprintf(out, "        fprintf(stderr, \"MiniScheme Fatal: failed to load embedded FASL: %%s\\n\",\n");
        fprintf(out, "                mini_get_error(vm) ? mini_get_error(vm) : \"unknown\");\n");
        fprintf(out, "        mini_destroy(vm);\n        return 1;\n    }\n");
        fprintf(out, "    int exit_code = 0;\n    Value main_fn = mini_get_global(vm, \"main\");\n");
        fprintf(out, "    if (mini_is_closure(main_fn) || mini_is_cfunc(main_fn)) {\n");
        fprintf(out, "        Value arg_list = mini_nil();\n        mini_push_root(vm, &arg_list);\n");
        fprintf(out, "        for (int i = argc - 1; i >= 1; i--) {\n");
        fprintf(out, "            Value s = mini_make_string(vm, argv[i]);\n");
        fprintf(out, "            arg_list = mini_make_cons(vm, s, arg_list);\n        }\n");
        fprintf(out, "        Value res = mini_call(vm, main_fn, 1, &arg_list);\n");
        fprintf(out, "        mini_pop_root(vm);\n");
        fprintf(out, "        if (mini_get_error(vm)) {\n");
        fprintf(out, "            fprintf(stderr, \"Runtime Error in main: %%s\\n\", mini_get_error(vm));\n");
        fprintf(out, "            exit_code = 1;\n");
        fprintf(out, "        } else if (mini_is_fixnum(res)) {\n");
        fprintf(out, "            exit_code = (int)mini_to_fixnum(res);\n        }\n    }\n");
        fprintf(out, "    mini_console_history_free();\n    mini_destroy(vm);\n    return exit_code;\n}\n");
    } else {
        fprintf(out, "#endif /* %s */\n", guard);
    }

    fclose(out);
    free(buf);
    return mini_true();
}

/* ========================================================================= */
/* Code ROM: XIP application image                                          */
/* ========================================================================= */
/* An MSBC top-level code object and every literal it references (nested    */
/* code, symbols, strings, numbers, quoted lists and vectors) are written    */
/* to Flash pre-linked at their final address, so the VM runs them in place  */
/* with no translation. Mutable state (globals, closures, frames) stays in   */
/* RAM; the GC never touches the image because it lies outside the heap.     */

#define MROM_VERSION 2   /* 2: symbols carry their global slot */

typedef struct {
    char     magic[4];     /* "MROM" */
    uint32_t version;
    uint32_t ptr_size;     /* sizeof(Value) of the VM that built it */
    uint32_t key;          /* caller-defined validity key (app + firmware) */
    uint64_t base;         /* address the image is linked to run at */
    uint32_t total_bytes;  /* header + objects + symbol table */
    uint32_t root_off;     /* top-level code object */
    uint32_t symtab_off;   /* Value[sym_count], sorted by symbol name */
    uint32_t sym_count;
    uint32_t checksum;     /* FNV-1a over [header end, total_bytes) */
    uint32_t reserved;
} MiniRomHeader;

#define MROM_HDR_BYTES ALIGN8(sizeof(MiniRomHeader))

static uint32_t fnv1a(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int sym_cmp(const SymbolObj *s, const char *name, size_t len) {
    size_t n = s->len < len ? s->len : len;
    int c = memcmp(s->name, name, n);
    if (c) return c;
    return (s->len > len) - (s->len < len);
}

Value xrom_find_symbol(const MiniVM *vm, const char *name, size_t len) {
    size_t lo = 0, hi = vm->xrom_sym_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const SymbolObj *s = (const SymbolObj *)UNTAG_PTR(vm->xrom_syms[mid]);
        int c = sym_cmp(s, name, len);
        if (c == 0) return vm->xrom_syms[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

bool mini_rom_contains(const MiniVM *vm, Value v) {
    uintptr_t tag = v & MINI_TAG_MASK;
    if (!vm || !vm->xrom_start || tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) return false;
    const char *p = (const char *)UNTAG_PTR(v);
    return p >= vm->xrom_start && p < vm->xrom_end;
}

size_t mini_rom_size(const MiniVM *vm) {
    return (vm && vm->xrom_start) ? (size_t)(vm->xrom_end - vm->xrom_start) : 0;
}

static bool is_ptr_val(Value v) {
    uintptr_t tag = v & MINI_TAG_MASK;
    return tag != MINI_TAG_FIXNUM && tag != MINI_TAG_CONST && UNTAG_PTR(v) != NULL;
}

typedef struct {
    MiniVM *vm;
    HeapObj **stack;
    size_t count, cap;
    bool failed;
} RomMark;

static void rom_mark_push(RomMark *m, Value v) {
    if (m->failed || !is_ptr_val(v)) return;
    HeapObj *obj = (HeapObj *)UNTAG_PTR(v);
    MiniVM *vm = m->vm;
    if ((char *)obj < vm->tenured_start || (char *)obj >= vm->tenured_hp) {
        vm_error(vm, "rom: literal outside the heap");
        m->failed = true;
        return;
    }
    if (obj->type & MINI_GC_MARK_BIT) return;
    switch (obj->type & MINI_GC_TYPE_MASK) {
        case OBJ_PAIR: case OBJ_CODE: case OBJ_VECTOR: case OBJ_SYMBOL:
        case OBJ_STRING: case OBJ_FLONUM: case OBJ_BIGINT:
            break;
        default:
            vm_error(vm, "rom: unsupported literal type %u", (unsigned)(obj->type & MINI_GC_TYPE_MASK));
            m->failed = true;
            return;
    }
    obj->type |= MINI_GC_MARK_BIT;
    if (m->count >= m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        HeapObj **s = (HeapObj **)realloc(m->stack, cap * sizeof(HeapObj *));
        if (!s) {
            vm_error(vm, "rom: out of memory");
            m->failed = true;
            return;
        }
        m->stack = s;
        m->cap = cap;
    }
    m->stack[m->count++] = obj;
}

typedef struct {
    HeapObj **objs;  /* image objects in heap address order */
    uint32_t *offs;  /* their offsets in the image */
    size_t n;
    uintptr_t base;
} RomMap;

static Value rom_reloc(const RomMap *map, Value v, bool *ok) {
    if (!is_ptr_val(v)) return v;
    HeapObj *obj = (HeapObj *)UNTAG_PTR(v);
    size_t lo = 0, hi = map->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (map->objs[mid] == obj) {
            return (Value)(map->base + map->offs[mid]) | (v & MINI_TAG_MASK);
        }
        if (map->objs[mid] < obj) lo = mid + 1; else hi = mid;
    }
    *ok = false;
    return v;
}

size_t mini_rom_build(MiniVM *vm, Value root, uintptr_t base, uint32_t key,
                      MiniRomWriteFn write, void *user) {
    if (!vm || !write || !mini_is_code(root)) return 0;
    if (vm->xrom_start) {
        vm_error(vm, "rom: cannot build while an image is attached");
        return 0;
    }
    mini_push_root(vm, &root);
    mini_gc(vm); /* compacts every live object into tenured */
    mini_pop_root(vm); /* nothing below allocates on the heap */
    if (vm->has_error || vm->eden_hp != vm->eden_start) return 0;

    /* 1. Mark the literal graph reachable from root */
    RomMark m = { vm, NULL, 0, 0, false };
    rom_mark_push(&m, root);
    while (!m.failed && m.count > 0) {
        HeapObj *obj = m.stack[--m.count];
        switch (obj->type & MINI_GC_TYPE_MASK) {
            case OBJ_PAIR:
                rom_mark_push(&m, ((PairObj *)obj)->car);
                rom_mark_push(&m, ((PairObj *)obj)->cdr);
                break;
            case OBJ_CODE: {
                CodeObj *co = (CodeObj *)obj;
                for (uint16_t i = 0; i < co->num_consts; i++) rom_mark_push(&m, CODE_CONSTS(co)[i]);
                break;
            }
            case OBJ_VECTOR: {
                VectorObj *vec = (VectorObj *)obj;
                for (size_t i = 0; i < vec->length; i++) rom_mark_push(&m, vec->data[i]);
                break;
            }
            default:
                break;
        }
    }
    free(m.stack);

    /* 2. Lay out marked objects in address order */
    RomMap map = { NULL, NULL, 0, base };
    size_t n = 0, nsym = 0, max_obj = 0;
    for (char *p = vm->tenured_start; p < vm->tenured_hp; p += ALIGN8(((HeapObj *)p)->size)) {
        HeapObj *obj = (HeapObj *)p;
        if (obj->type & MINI_GC_MARK_BIT) {
            n++;
            if ((obj->type & MINI_GC_TYPE_MASK) == OBJ_SYMBOL) nsym++;
            if (obj->size > max_obj) max_obj = obj->size;
        }
    }
    map.objs = (HeapObj **)malloc((n ? n : 1) * sizeof(HeapObj *));
    map.offs = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    uint32_t *sym_idx = (uint32_t *)malloc((nsym ? nsym : 1) * sizeof(uint32_t));
    uint8_t *tmp = (uint8_t *)malloc(max_obj ? max_obj : 8);
    bool ok = !m.failed && map.objs && map.offs && sym_idx && tmp;
    size_t cursor = MROM_HDR_BYTES, k = 0, ks = 0;
    if (ok) {
        for (char *p = vm->tenured_start; p < vm->tenured_hp; p += ALIGN8(((HeapObj *)p)->size)) {
            HeapObj *obj = (HeapObj *)p;
            if (!(obj->type & MINI_GC_MARK_BIT)) continue;
            if ((obj->type & MINI_GC_TYPE_MASK) == OBJ_SYMBOL) sym_idx[ks++] = (uint32_t)k;
            map.objs[k] = obj;
            map.offs[k] = (uint32_t)cursor;
            cursor += ALIGN8(obj->size);
            k++;
        }
        map.n = k;
    } else if (!vm->has_error) {
        vm_error(vm, "rom: out of memory");
    }

    /* 3. Emit relocated objects, then the sorted symbol table, then header */
    uint32_t sum = 2166136261u;
    uint32_t sym_slot = 0;
    for (size_t i = 0; ok && i < map.n; i++) {
        HeapObj *obj = map.objs[i];
        size_t sz = ALIGN8(obj->size);
        memcpy(tmp, obj, obj->size);
        memset(tmp + obj->size, 0, sz - obj->size);
        HeapObj *c = (HeapObj *)tmp;
        c->type &= MINI_GC_TYPE_MASK;
        switch (c->type) {
            case OBJ_PAIR:
                ((PairObj *)c)->car = rom_reloc(&map, ((PairObj *)c)->car, &ok);
                ((PairObj *)c)->cdr = rom_reloc(&map, ((PairObj *)c)->cdr, &ok);
                break;
            case OBJ_CODE: {
                CodeObj *co = (CodeObj *)c;
                for (uint16_t j = 0; j < co->num_consts; j++) CODE_CONSTS(co)[j] = rom_reloc(&map, CODE_CONSTS(co)[j], &ok);
                break;
            }
            case OBJ_VECTOR: {
                VectorObj *vec = (VectorObj *)c;
                for (size_t j = 0; j < vec->length; j++) vec->data[j] = rom_reloc(&map, vec->data[j], &ok);
                break;
            }
            default:
                break;
        }
        if (c->type == OBJ_SYMBOL) c->type |= ++sym_slot << MINI_SYM_SLOT_SHIFT;
        sum = fnv1a(sum, tmp, sz);
        if (ok && !write(map.offs[i], tmp, sz, user)) ok = false;
    }
    if (ok) {
        /* insertion sort by name; symbol counts are small */
        for (size_t i = 1; i < nsym; i++) {
            uint32_t v = sym_idx[i];
            const SymbolObj *sv = (const SymbolObj *)map.objs[v];
            size_t j = i;
            while (j > 0 && sym_cmp((const SymbolObj *)map.objs[sym_idx[j - 1]], sv->name, sv->len) > 0) {
                sym_idx[j] = sym_idx[j - 1];
                j--;
            }
            sym_idx[j] = v;
        }
        for (size_t i = 0; ok && i < nsym; i++) {
            Value sv = (Value)(base + map.offs[sym_idx[i]]) | MINI_TAG_SYMBOL;
            sum = fnv1a(sum, &sv, sizeof(sv));
            if (!write(cursor + i * sizeof(Value), &sv, sizeof(sv), user)) ok = false;
        }
    }
    size_t total = cursor + nsym * sizeof(Value);
    if (ok) {
        MiniRomHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, "MROM", 4);
        hdr.version = MROM_VERSION;
        hdr.ptr_size = (uint32_t)sizeof(Value);
        hdr.key = key;
        hdr.base = (uint64_t)base;
        hdr.total_bytes = (uint32_t)total;
        hdr.root_off = (uint32_t)((rom_reloc(&map, root, &ok) & ~(uintptr_t)MINI_TAG_MASK) - base);
        hdr.symtab_off = (uint32_t)cursor;
        hdr.sym_count = (uint32_t)nsym;
        hdr.checksum = sum;
        if (ok && !write(0, &hdr, sizeof(hdr), user)) ok = false;
    }
    if (!ok && !vm->has_error) vm_error(vm, "rom: image write failed");

    /* 4. Restore heap headers */
    for (char *p = vm->tenured_start; p < vm->tenured_hp; p += ALIGN8(((HeapObj *)p)->size)) {
        ((HeapObj *)p)->type &= ~MINI_GC_MARK_BIT;
    }
    free(map.objs);
    free(map.offs);
    free(sym_idx);
    free(tmp);
    return ok ? total : 0;
}

/* Re-points RAM references at the image's symbols so each name keeps a
 * single identity (eq?). Run on a fresh VM, before any user code. */
static void xrom_reintern(MiniVM *vm) {
    Value *fixed[] = {
        &vm->sym_quote, &vm->sym_if, &vm->sym_begin, &vm->sym_lambda, &vm->sym_define,
        &vm->sym_set, &vm->sym_let, &vm->sym_letstar, &vm->sym_letrec,
        &vm->sym_define_syntax, &vm->sym_syntax_rules, &vm->sym_define_macro,
        &vm->sym_ellipsis, &vm->sym_wildcard,
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (!mini_is_symbol(*fixed[i])) continue;
        const SymbolObj *s = (const SymbolObj *)UNTAG_PTR(*fixed[i]);
        Value r = xrom_find_symbol(vm, s->name, s->len);
        if (r) *fixed[i] = r;
    }
    for (Value curr = mini_cdr(vm->global_env); mini_is_pair(curr); curr = mini_cdr(curr)) {
        Value b = mini_car(curr);
        if (!mini_is_pair(b) || !mini_is_symbol(mini_car(b))) continue;
        const SymbolObj *s = (const SymbolObj *)UNTAG_PTR(mini_car(b));
        Value r = xrom_find_symbol(vm, s->name, s->len);
        if (r) mini_set_car(b, r);
    }
    mini_globals_reset(vm); /* binding keys changed */
    /* drop RAM twins from the intern list (relink in place, no allocation) */
    Value prev = mini_nil();
    for (Value curr = vm->symbols; mini_is_pair(curr); curr = mini_cdr(curr)) {
        const SymbolObj *s = (const SymbolObj *)UNTAG_PTR(mini_car(curr));
        if (xrom_find_symbol(vm, s->name, s->len)) {
            if (mini_is_nil(prev)) vm->symbols = mini_cdr(curr);
            else mini_set_cdr(prev, mini_cdr(curr));
        } else {
            prev = curr;
        }
    }
}

/* ========================================================================= */
/* Code ROM from an MSBC stream, without a VM                                */
/* ========================================================================= */
/* The MSBC is a tree (symbols by name are the only sharing), so each object
 * can be written the moment it is read: its slots for what it refers to are
 * left erased, the children follow it in the image, and each slot is filled
 * in once its child has an address (flash bits go from 1 to 0, so a slot
 * left erased can be written later). A small window keeps the most recent
 * bytes in RAM, where most fills land; an object larger than the window is
 * written straight through. Memory stays a few KB whatever the app's size. */

#define RS_WIN       2048   /* bytes of the image kept in RAM before they go out */
#define RS_IN        512    /* input buffer */
#define RS_MAX_DEPTH 256    /* nested code / data levels */

/* MSBC constant tags (msbc.h) */
enum { RS_FIXNUM = 0, RS_FLONUM, RS_BOOL, RS_STRING, RS_SYMBOL, RS_NIL, RS_VOID, RS_CODE, RS_PAIR, RS_VECTOR, RS_LSTRING,
       RS_BIGINT, RS_SYMREF };

typedef struct {
    MiniRomReadFn read;
    void *read_user;
    uint8_t in[RS_IN];
    size_t in_pos, in_len;
    MiniRomWriteFn write;
    void *write_user;
    uintptr_t base;
    const uint8_t *img;            /* the image as written, for reading it back */
    uint8_t win[RS_WIN];           /* image bytes [win_off, cursor) not yet written out */
    size_t win_off, cursor;
    uint32_t *sym_off;             /* symbol index: open addressing, offset + 1 (0 = free) */
    uint32_t *sym_hash;
    size_t sym_n, sym_cap;
    char *name;                    /* the symbol being read */
    size_t name_cap;
    Value *symtab;                 /* MSBC version 2: the image's symbols by table index */
    size_t symtab_n;
    uint16_t msbc_version;
    size_t mem, peak;
    int depth;
    const char *err;
} RomStream;

static void rs_fail(RomStream *s, const char *why) {
    if (!s->err) s->err = why;
}

static void rs_mem(RomStream *s, ptrdiff_t delta) {
    s->mem = (size_t)((ptrdiff_t)s->mem + delta);
    if (s->mem > s->peak) s->peak = s->mem;
}

/* ---- input ---------------------------------------------------------------------------- */

static bool rs_in(RomStream *s, void *out, size_t n) {
    uint8_t *o = (uint8_t *)out;
    while (n > 0) {
        if (s->in_pos == s->in_len) {
            s->in_len = s->err ? 0 : s->read(s->in, RS_IN, s->read_user);
            s->in_pos = 0;
            if (s->in_len == 0) {
                rs_fail(s, "rom: truncated bytecode");
                memset(o, 0, n);
                return false;
            }
        }
        size_t k = s->in_len - s->in_pos < n ? s->in_len - s->in_pos : n;
        memcpy(o, s->in + s->in_pos, k);
        s->in_pos += k;
        o += k;
        n -= k;
    }
    return true;
}

static uint8_t rs_u8(RomStream *s) { uint8_t v = 0; rs_in(s, &v, 1); return v; }
static uint16_t rs_u16(RomStream *s) { uint16_t v = 0; rs_in(s, &v, 2); return v; }
static uint32_t rs_u32(RomStream *s) { uint32_t v = 0; rs_in(s, &v, 4); return v; }

/* A LEB128 varint of at most 5 bytes (MSBC version 2) */
static uint32_t rs_varint(RomStream *s) {
    uint32_t v = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        uint8_t b = rs_u8(s);
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    rs_fail(s, "rom: bad varint in bytecode");
    return 0;
}

/* ---- output --------------------------------------------------------------------------- */

static void rs_flush(RomStream *s) {
    if (s->cursor > s->win_off && !s->err &&
        !s->write(s->win_off, s->win, s->cursor - s->win_off, s->write_user)) {
        rs_fail(s, "rom: image write failed");
    }
    s->win_off = s->cursor;
}

/* Bytes at image offset off: into the window when they fall there, else
 * written out (a slot filled in, or a large object streamed through) */
static void rs_put(RomStream *s, size_t off, const void *data, size_t len) {
    if (s->err || len == 0) return;
    if (off >= s->win_off && off + len <= s->win_off + RS_WIN) {
        memcpy(s->win + (off - s->win_off), data, len);
    } else if (!s->write(off, data, len, s->write_user)) {
        rs_fail(s, "rom: image write failed");
    }
}

/* Room for an object of `size` bytes (8-aligned): its offset. Small ones
 * go into the window (erased-looking: 0xFF), large ones past it. */
static size_t rs_alloc(RomStream *s, size_t size, bool *large) {
    if (s->cursor + size > s->win_off + RS_WIN) rs_flush(s);
    size_t off = s->cursor;
    *large = size > RS_WIN;
    if (*large) {
        s->cursor += size;
        s->win_off = s->cursor;   /* the window starts after it */
    } else {
        memset(s->win + (off - s->win_off), 0xFF, size);
        s->cursor += size;
    }
    if (s->cursor > 0xFFFFFFF0u) rs_fail(s, "rom: image too large");
    return off;
}

/* Bytes of the image written so far, from the window or read back */
static void rs_peek(RomStream *s, size_t off, void *out, size_t len) {
    if (off >= s->win_off) memcpy(out, s->win + (off - s->win_off), len);
    else memcpy(out, s->img + off, len);
}

static void rs_zero(RomStream *s, size_t off, size_t len) {
    static const uint8_t zeros[16] = { 0 };
    while (len > 0) {
        size_t k = len < sizeof(zeros) ? len : sizeof(zeros);
        rs_put(s, off, zeros, k);
        off += k;
        len -= k;
    }
}

/* n input bytes copied to image offset off, in pieces */
static void rs_copy_in(RomStream *s, size_t off, size_t n) {
    uint8_t buf[128];
    while (n > 0 && !s->err) {
        size_t k = n < sizeof(buf) ? n : sizeof(buf);
        if (!rs_in(s, buf, k)) return;
        rs_put(s, off, buf, k);
        off += k;
        n -= k;
    }
}

static void rs_header(RomStream *s, size_t off, uint32_t type, size_t size) {
    HeapObj h;
    h.type = type;
    h.size = (uint32_t)size;
    rs_put(s, off, &h, sizeof(h));
}

static Value rs_ptr(RomStream *s, size_t off, uintptr_t tag) { return (Value)(s->base + off) | tag; }

/* ---- symbols -------------------------------------------------------------------------- */

static uint32_t rs_hash(const char *name, size_t len) {
    uint32_t h = fnv1a(2166136261u, name, len);
    return h ? h : 1;
}

static bool rs_sym_grow(RomStream *s) {
    size_t cap = s->sym_cap ? s->sym_cap * 2 : 256;
    uint32_t *off = (uint32_t *)calloc(cap, sizeof(uint32_t));
    uint32_t *hash = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!off || !hash) {
        free(off);
        free(hash);
        rs_fail(s, "rom: out of memory");
        return false;
    }
    for (size_t i = 0; i < s->sym_cap; i++) {
        if (!s->sym_off[i]) continue;
        size_t j = s->sym_hash[i] & (cap - 1);
        while (off[j]) j = (j + 1) & (cap - 1);
        off[j] = s->sym_off[i];
        hash[j] = s->sym_hash[i];
    }
    rs_mem(s, (ptrdiff_t)(cap * 8) - (ptrdiff_t)(s->sym_cap * 8));
    free(s->sym_off);
    free(s->sym_hash);
    s->sym_off = off;
    s->sym_hash = hash;
    s->sym_cap = cap;
    return true;
}

/* Is the symbol at image offset off named name? */
static bool rs_sym_is(RomStream *s, size_t off, const char *name, size_t len) {
    size_t have = 0;
    rs_peek(s, off + offsetof(SymbolObj, len), &have, sizeof(have));
    if (have != len) return false;
    char buf[64];
    for (size_t i = 0; i < len; i += sizeof(buf)) {
        size_t k = len - i < sizeof(buf) ? len - i : sizeof(buf);
        rs_peek(s, off + offsetof(SymbolObj, name) + i, buf, k);
        if (memcmp(buf, name + i, k) != 0) return false;
    }
    return true;
}

static Value rs_symbol(RomStream *s) {
    size_t len = rs_u16(s);
    if (len + 1 > s->name_cap) {
        char *n = (char *)realloc(s->name, len + 1);
        if (!n) { rs_fail(s, "rom: out of memory"); return mini_void(); }
        rs_mem(s, (ptrdiff_t)(len + 1) - (ptrdiff_t)s->name_cap);
        s->name = n;
        s->name_cap = len + 1;
    }
    if (!rs_in(s, s->name, len)) return mini_void();
    uint32_t h = rs_hash(s->name, len);
    if ((s->sym_n + 1) * 5 > s->sym_cap * 3 && !rs_sym_grow(s)) return mini_void();
    size_t j = h & (s->sym_cap - 1);
    for (; s->sym_off[j]; j = (j + 1) & (s->sym_cap - 1)) {
        if (s->sym_hash[j] == h && rs_sym_is(s, s->sym_off[j] - 1, s->name, len)) {
            return rs_ptr(s, s->sym_off[j] - 1, MINI_TAG_SYMBOL);
        }
    }
    size_t size = ALIGN8(sizeof(SymbolObj) + len);
    bool large;
    size_t off = rs_alloc(s, size, &large);
    rs_header(s, off, OBJ_SYMBOL | ((uint32_t)(s->sym_n + 1) << MINI_SYM_SLOT_SHIFT), size);
    rs_put(s, off + offsetof(SymbolObj, len), &len, sizeof(len));
    rs_put(s, off + offsetof(SymbolObj, name), s->name, len);
    rs_zero(s, off + offsetof(SymbolObj, name) + len, size - offsetof(SymbolObj, name) - len);
    s->sym_off[j] = (uint32_t)off + 1;
    s->sym_hash[j] = h;
    s->sym_n++;
    return rs_ptr(s, off, MINI_TAG_SYMBOL);
}

/* ---- objects -------------------------------------------------------------------------- */

static Value rs_value(RomStream *s, uint8_t tag);

static Value rs_next(RomStream *s) { return rs_value(s, rs_u8(s)); }

static Value rs_string(RomStream *s, size_t len) {
    size_t size = ALIGN8(sizeof(StringObj) + len);
    bool large;
    size_t off = rs_alloc(s, size, &large);
    rs_header(s, off, OBJ_STRING, size);
    rs_put(s, off + offsetof(StringObj, len), &len, sizeof(len));
    rs_copy_in(s, off + offsetof(StringObj, data), len);
    rs_zero(s, off + offsetof(StringObj, data) + len, size - offsetof(StringObj, data) - len);
    return rs_ptr(s, off, MINI_TAG_STRING);
}

static Value rs_code(RomStream *s) {
    CodeObj co;
    memset(&co, 0, sizeof(co));
    if (s->msbc_version >= 2) {   /* the header as varints */
        co.code_len = rs_varint(s);
        uint32_t np = rs_varint(s), ms = rs_varint(s);
        uint8_t flags = rs_u8(s);
        uint32_t nc = rs_varint(s);
        if (np > 0xFFFF || ms > 0xFFFF || nc > 0xFFFF) rs_fail(s, "rom: bad code header");
        co.num_params = (uint16_t)np;
        co.max_stack = (uint16_t)ms;
        co.is_variadic = flags & 1;
        co.has_inner_lambdas = (flags >> 1) & 1;
        co.num_consts = (uint16_t)nc;
    } else {
        co.code_len = rs_u32(s);
        co.num_params = rs_u16(s);
        co.max_stack = rs_u16(s);
        co.is_variadic = rs_u8(s);
        co.has_inner_lambdas = rs_u8(s);
        co.num_consts = rs_u16(s);
    }
    if (s->err) return mini_void();
    size_t consts = sizeof(CodeObj), bytes = consts + (size_t)co.num_consts * sizeof(Value);
    size_t size = ALIGN8(bytes + co.code_len);
    bool large;
    size_t off = rs_alloc(s, size, &large);
    co.header.type = OBJ_CODE;
    co.header.size = (uint32_t)size;
    rs_put(s, off, &co, sizeof(co));
    rs_copy_in(s, off + bytes, co.code_len);
    rs_zero(s, off + bytes + co.code_len, size - bytes - co.code_len);
    for (uint16_t i = 0; i < co.num_consts && !s->err; i++) {
        Value v = rs_next(s);
        rs_put(s, off + consts + i * sizeof(Value), &v, sizeof(v));
    }
    return rs_ptr(s, off, MINI_TAG_CODE);
}

/* A list: the cdr chain is followed in a loop, not by recursion */
static Value rs_pair(RomStream *s) {
    bool large;
    size_t off = rs_alloc(s, sizeof(PairObj), &large);
    Value head = rs_ptr(s, off, MINI_TAG_PAIR);
    for (;;) {
        rs_header(s, off, OBJ_PAIR, sizeof(PairObj));
        Value car = rs_next(s);
        rs_put(s, off + offsetof(PairObj, car), &car, sizeof(car));
        uint8_t tag = rs_u8(s);
        if (s->err) return head;
        if (tag != RS_PAIR) {
            Value cdr = rs_value(s, tag);
            rs_put(s, off + offsetof(PairObj, cdr), &cdr, sizeof(cdr));
            return head;
        }
        size_t next = rs_alloc(s, sizeof(PairObj), &large);
        Value link = rs_ptr(s, next, MINI_TAG_PAIR);
        rs_put(s, off + offsetof(PairObj, cdr), &link, sizeof(link));
        off = next;
    }
}

static Value rs_vector(RomStream *s) {
    size_t len = rs_u16(s);
    size_t size = ALIGN8(sizeof(VectorObj) + (len > 1 ? (len - 1) * sizeof(Value) : 0));
    bool large;
    size_t off = rs_alloc(s, size, &large);
    rs_header(s, off, OBJ_VECTOR, size);
    rs_put(s, off + offsetof(VectorObj, length), &len, sizeof(len));
    size_t data = offsetof(VectorObj, data);
    rs_zero(s, off + data + len * sizeof(Value), size - data - len * sizeof(Value));
    for (size_t i = 0; i < len && !s->err; i++) {
        Value v = rs_next(s);
        rs_put(s, off + data + i * sizeof(Value), &v, sizeof(v));
    }
    return rs_ptr(s, off, MINI_TAG_RECORD);
}

/* An integer outside the fixnums: a BigIntObj in the image (read-only, as
 * every bignum operation makes a new one); sign and limbs as MSBC holds them */
static Value rs_bigint(RomStream *s, int sign, const uint32_t *limbs, uint32_t n) {
    while (n > 0 && limbs && limbs[n - 1] == 0) n--;
    if (n == 0) return mini_make_fixnum(0);
    if (n <= 2) {                       /* a fixnum after all on this target? */
        uint64_t mag = limbs[0] | (n > 1 ? (uint64_t)limbs[1] << 32 : 0);
        if (mag <= (uint64_t)MINI_FIXNUM_MAX + (sign < 0 ? 1u : 0u)) {
            return mini_make_fixnum(sign < 0 ? -(int64_t)mag : (int64_t)mag);
        }
    }
    size_t size = ALIGN8(sizeof(BigIntObj) + (n > 1 ? (size_t)(n - 1) * sizeof(uint32_t) : 0));
    bool large;
    size_t off = rs_alloc(s, size, &large);
    rs_header(s, off, OBJ_BIGINT, size);
    int32_t sg = sign < 0 ? -1 : 1;
    rs_put(s, off + offsetof(BigIntObj, sign), &sg, sizeof(sg));
    rs_put(s, off + offsetof(BigIntObj, length), &n, sizeof(n));
    rs_zero(s, off + offsetof(BigIntObj, digits), size - offsetof(BigIntObj, digits));
    rs_put(s, off + offsetof(BigIntObj, digits), limbs, n * sizeof(uint32_t));
    return rs_ptr(s, off, MINI_TAG_RECORD);
}

static Value rs_value(RomStream *s, uint8_t tag) {
    if (s->err) return mini_void();
    if (++s->depth > RS_MAX_DEPTH) {
        rs_fail(s, "rom: nested too deep");
        return mini_void();
    }
    Value v = mini_void();
    switch (tag) {
        case RS_FIXNUM: {
            uint64_t u = 0;
            rs_in(s, &u, sizeof(u));
            int64_t iv = (int64_t)u;
            if (iv >= MINI_FIXNUM_MIN && iv <= MINI_FIXNUM_MAX) {
                v = mini_make_fixnum(iv);
            } else {                    /* an older image's wide fixnum */
                uint64_t mag = iv < 0 ? (uint64_t)0 - u : u;
                uint32_t limbs[2] = { (uint32_t)mag, (uint32_t)(mag >> 32) };
                v = rs_bigint(s, iv < 0 ? -1 : 1, limbs, 2);
            }
            break;
        }
        case RS_BIGINT: {
            int8_t sign = (int8_t)rs_u8(s);
            uint32_t n = rs_u16(s);
            uint32_t limbs[64];         /* up to 2048-bit constants */
            if (n > 64) {
                rs_fail(s, "rom: integer constant too large");
                break;
            }
            rs_in(s, limbs, n * sizeof(uint32_t));
            v = rs_bigint(s, sign, limbs, n);
            break;
        }
        case RS_FLONUM: {
            FlonumObj f;
            memset(&f, 0, sizeof(f));
            rs_in(s, &f.val, sizeof(double));
            bool large;
            size_t off = rs_alloc(s, sizeof(FlonumObj), &large);
            f.header.type = OBJ_FLONUM;
            f.header.size = (uint32_t)ALIGN8(sizeof(FlonumObj));
            rs_put(s, off, &f, sizeof(f));
            v = rs_ptr(s, off, MINI_TAG_RECORD);
            break;
        }
        case RS_BOOL: v = rs_u8(s) ? mini_true() : mini_false(); break;
        case RS_STRING: v = rs_string(s, rs_u16(s)); break;
        case RS_LSTRING: v = rs_string(s, rs_u32(s)); break;
        case RS_SYMBOL: v = rs_symbol(s); break;
        case RS_SYMREF: {
            size_t idx = rs_varint(s);
            if (idx < s->symtab_n) v = s->symtab[idx];
            else rs_fail(s, "rom: symbol index out of range");
            break;
        }
        case RS_NIL: v = mini_nil(); break;
        case RS_VOID: v = mini_void(); break;
        case RS_CODE: v = rs_code(s); break;
        case RS_PAIR: v = rs_pair(s); break;
        case RS_VECTOR: v = rs_vector(s); break;
        default: rs_fail(s, "rom: unknown constant in bytecode"); break;
    }
    s->depth--;
    return v;
}

/* Symbol image offsets sorted by name (insertion sort: a few hundred) */
static void rs_sort_symbols(RomStream *s, uint32_t *offs, size_t n) {
    for (size_t i = 1; i < n; i++) {
        uint32_t v = offs[i];
        const SymbolObj *sv = (const SymbolObj *)(const void *)(s->img + v);
        size_t j = i;
        while (j > 0 && sym_cmp((const SymbolObj *)(const void *)(s->img + offs[j - 1]), sv->name, sv->len) > 0) {
            offs[j] = offs[j - 1];
            j--;
        }
        offs[j] = v;
    }
}

size_t mini_rom_build_stream(MiniRomReadFn read, void *read_user, uintptr_t base, const void *image,
                             uint32_t key, MiniRomWriteFn write, void *write_user,
                             size_t *peak_mem, const char **err) {
    if (err) *err = NULL;
    if (peak_mem) *peak_mem = 0;
    if (!read || !write) return 0;
    RomStream *s = (RomStream *)calloc(1, sizeof(RomStream));
    if (!s) {
        if (err) *err = "rom: out of memory";
        return 0;
    }
    s->read = read;
    s->read_user = read_user;
    s->write = write;
    s->write_user = write_user;
    s->base = base;
    s->img = image ? (const uint8_t *)image : (const uint8_t *)base;
    s->win_off = s->cursor = MROM_HDR_BYTES;
    rs_mem(s, (ptrdiff_t)sizeof(RomStream));

    /* MSBC header, (version 2) its symbol table, then the top-level code object */
    uint32_t magic = rs_u32(s);
    uint16_t version = rs_u16(s);
    (void)rs_u16(s);
    if (!s->err && (magic != 0x4342534Du || version < 1 || version > 2)) rs_fail(s, "rom: not MSBC bytecode");
    s->msbc_version = version;
    if (!s->err && version == 2) {
        size_t n = rs_u16(s);
        s->symtab = (Value *)malloc((n ? n : 1) * sizeof(Value));
        if (!s->symtab) rs_fail(s, "rom: out of memory");
        else rs_mem(s, (ptrdiff_t)(n * sizeof(Value)));
        for (size_t i = 0; i < n && !s->err; i++) s->symtab[i] = rs_symbol(s);
        s->symtab_n = s->err ? 0 : n;
    }
    Value root = s->err ? mini_void() : rs_code(s);  /* no tag byte before the top level */
    rs_flush(s);

    /* the symbol table, sorted by name, then the checksum and the header */
    size_t total = 0;
    uint32_t *offs = NULL;
    if (!s->err) {
        offs = (uint32_t *)malloc((s->sym_n ? s->sym_n : 1) * sizeof(uint32_t));
        if (!offs) rs_fail(s, "rom: out of memory");
        else rs_mem(s, (ptrdiff_t)(s->sym_n * sizeof(uint32_t)));
    }
    if (!s->err) {
        size_t k = 0;
        for (size_t i = 0; i < s->sym_cap; i++) if (s->sym_off[i]) offs[k++] = s->sym_off[i] - 1;
        rs_sort_symbols(s, offs, k);
        for (size_t i = 0; i < k && !s->err; i++) {
            Value sv = rs_ptr(s, offs[i], MINI_TAG_SYMBOL);
            if (!s->write(s->cursor + i * sizeof(Value), &sv, sizeof(sv), s->write_user)) rs_fail(s, "rom: image write failed");
        }
        total = s->cursor + k * sizeof(Value);
    }
    if (!s->err) {
        MiniRomHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, "MROM", 4);
        hdr.version = MROM_VERSION;
        hdr.ptr_size = (uint32_t)sizeof(Value);
        hdr.key = key;
        hdr.base = (uint64_t)base;
        hdr.total_bytes = (uint32_t)total;
        hdr.root_off = (uint32_t)((root & ~(uintptr_t)MINI_TAG_MASK) - base);
        hdr.symtab_off = (uint32_t)s->cursor;
        hdr.sym_count = (uint32_t)s->sym_n;
        hdr.checksum = fnv1a(2166136261u, s->img + MROM_HDR_BYTES, total - MROM_HDR_BYTES);
        if (!s->write(0, &hdr, sizeof(hdr), s->write_user)) rs_fail(s, "rom: image write failed");
    }
    if (err) *err = s->err;
    if (peak_mem) *peak_mem = s->peak;
    size_t result = s->err ? 0 : total;
    free(offs);
    free(s->sym_off);
    free(s->sym_hash);
    free(s->name);
    free(s->symtab);
    free(s);
    return result;
}

bool mini_rom_check(const void *image, size_t size, uint32_t key) {
    if (!image || size < MROM_HDR_BYTES) return false;
    MiniRomHeader hdr;
    memcpy(&hdr, image, sizeof(hdr));
    if (memcmp(hdr.magic, "MROM", 4) != 0 || hdr.version != MROM_VERSION ||
        hdr.ptr_size != sizeof(Value) || hdr.key != key ||
        hdr.base != (uint64_t)(uintptr_t)image || hdr.total_bytes > size ||
        hdr.total_bytes < MROM_HDR_BYTES || hdr.root_off >= hdr.total_bytes ||
        hdr.symtab_off + (uint64_t)hdr.sym_count * sizeof(Value) != hdr.total_bytes ||
        (hdr.symtab_off & 7) != 0) {
        return false;
    }
    const char *img = (const char *)image;
    return fnv1a(2166136261u, img + MROM_HDR_BYTES, hdr.total_bytes - MROM_HDR_BYTES) == hdr.checksum;
}

Value mini_rom_attach(MiniVM *vm, const void *image, size_t size, uint32_t key) {
    if (!vm || !mini_rom_check(image, size, key)) return mini_void();
    MiniRomHeader hdr;
    memcpy(&hdr, image, sizeof(hdr));
    const char *img = (const char *)image;
    vm->xrom_start = img;
    vm->xrom_end = img + hdr.total_bytes;
    vm->xrom_syms = (const Value *)(const void *)(img + hdr.symtab_off);
    vm->xrom_sym_count = hdr.sym_count;
    /* a global slot per image symbol (without the memory: the global cache alone) */
    free(vm->gslots);
    vm->gslot_count = 0;
    vm->gslots = hdr.sym_count ? (Value *)mini_word_alloc(hdr.sym_count * sizeof(Value)) : NULL;
    if (vm->gslots) vm->gslot_count = hdr.sym_count;
    xrom_reintern(vm);
    return (Value)(uintptr_t)(img + hdr.root_off) | MINI_TAG_CODE;
}
