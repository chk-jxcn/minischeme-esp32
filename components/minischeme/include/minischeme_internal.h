/**
 * @file minischeme_internal.h
 * @brief MiniScheme Internal Types, Virtual Machine Structure, and Engine Declarations.
 */

#ifndef MINISCHEME_INTERNAL_H
#define MINISCHEME_INTERNAL_H

#include "minischeme.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#define DEFAULT_INITIAL_HEAP (128 * 1024)
#define DEFAULT_STACK_SIZE   2048
#define MAX_ERROR_LEN        512
#define MAX_CALL_FRAMES      2048

/* Fixnum bounds: 61-bit for 64-bit systems, 29-bit for 32-bit systems (ESP32, WASM, i686) */
#if defined(UINTPTR_MAX) && (UINTPTR_MAX > 0xFFFFFFFFULL)
#define MINI_FIXNUM_MAX (((int64_t)1 << 60) - 1)
#define MINI_FIXNUM_MIN (-((int64_t)1 << 60))
#else
#define MINI_FIXNUM_MAX (((int64_t)1 << 28) - 1)
#define MINI_FIXNUM_MIN (-((int64_t)1 << 28))
#endif

typedef MiniObjType ObjType;
typedef MiniHeapObj HeapObj;
#define OBJ_PAIR         MINI_OBJ_PAIR
#define OBJ_CLOSURE      MINI_OBJ_CLOSURE
#define OBJ_SYMBOL       MINI_OBJ_SYMBOL
#define OBJ_STRING       MINI_OBJ_STRING
#define OBJ_CFUNC        MINI_OBJ_CFUNC
#define OBJ_CODE         MINI_OBJ_CODE
#define OBJ_VECTOR       MINI_OBJ_VECTOR
#define OBJ_FLONUM       MINI_OBJ_FLONUM
#define OBJ_FORWARDED    MINI_OBJ_FORWARDED
#define OBJ_CONTINUATION MINI_OBJ_CONTINUATION
#define OBJ_MACRO        MINI_OBJ_MACRO
#define OBJ_BIGINT       MINI_OBJ_BIGINT

/* Generational GC Object Header Bit Allocation (in MiniHeapObj->type) */
#define MINI_GC_TYPE_MASK   0x000000FFU /* Bits 0..7: Object type tag (1..12) */
#define MINI_GC_REMSET_BIT  0x00000100U /* Bit 8: In-remset flag for Tenured objects */
#define MINI_GC_FWD_SHIFT   9           /* Bits 9..30: 22-bit forwarding offset >> 3 (up to 32MB heap) */
#define MINI_GC_FWD_MASK    0x003FFFFFU
#define MINI_GC_MARK_BIT    0x80000000U /* Bit 31: Mark bit for Major GC Mark-Compact */

/* A Code ROM symbol's header carries its global slot (1 + its number in the image) in bits
 * 9..30: the GC never touches the image, so they stay; heap objects use them for forwarding */
#define MINI_SYM_SLOT_SHIFT 9

typedef struct ForwardedObj {
    HeapObj header;
    Value new_val;
} ForwardedObj;

typedef struct PairObj {
    HeapObj header;
    Value car;
    Value cdr;
} PairObj;

typedef struct SymbolObj {
    HeapObj header;
    size_t len;
    char name[1];
} SymbolObj;

typedef struct StringObj {
    HeapObj header;
    size_t len;
    char data[1];
} StringObj;

typedef struct CFuncObj {
    HeapObj header;
    MiniCFunc fn;
    const char *name;
} CFuncObj;

typedef struct CodeObj {
    HeapObj header;
    uint32_t code_len;
    uint16_t num_params;
    uint16_t max_stack;
    uint8_t  is_variadic;
    uint8_t  has_inner_lambdas;
    uint16_t num_consts;
} CodeObj;

#define CODE_CONSTS(co) ((Value *)((char *)(co) + sizeof(CodeObj)))
#define CODE_BYTES(co)  ((uint8_t *)((char *)(co) + sizeof(CodeObj) + (co)->num_consts * sizeof(Value)))

typedef struct FlonumObj {
    HeapObj header;
    double val;
} FlonumObj;

typedef struct VectorObj {
    HeapObj header;
    size_t length;
    Value data[1];
} VectorObj;

typedef struct ClosureObj {
    HeapObj header;
    Value code;
    Value env;
} ClosureObj;

typedef struct SavedFrame {
    Value closure_val;
    Value code_val;
    uint32_t pc_offset;
    uint32_t fp_offset;
    Value env;
#if UINTPTR_MAX == 0xffffffff
    uint32_t pad;
#endif
} SavedFrame;

typedef struct ContinuationObj {
    HeapObj header;
    uint32_t frame_count;
    uint32_t stack_len;
    uint32_t base_frame;
    uint32_t reserved;
} ContinuationObj;

#define CONT_FRAMES(cont) ((SavedFrame *)((char *)(cont) + sizeof(ContinuationObj)))
#define CONT_STACK(cont)  ((Value *)((char *)(cont) + sizeof(ContinuationObj) + (cont)->frame_count * sizeof(SavedFrame)))

typedef struct MacroObj {
    HeapObj header;
    uint32_t is_syntax_rules;
#if UINTPTR_MAX == 0xffffffff
    uint32_t pad;
#endif
    Value proc_or_literals;
    Value rules;
} MacroObj;

typedef struct BigIntObj {
    HeapObj header;
    int32_t sign;       /* +1 or -1; 0 is normalized to fixnum 0 */
    uint32_t length;    /* number of 32-bit limbs in digits[] */
    uint32_t digits[1]; /* little-endian limbs */
} BigIntObj;

#define ALIGN8(n) (((n) + 7) & ~(size_t)7)

/* Memory the VM reads and writes only as aligned 32-bit words: its value
 * stack, call frames, GC mark stack and remembered set. On the ESP32 it
 * comes from the IRAM-only heap (word access only, otherwise unused), so the
 * same amount of byte-addressable RAM stays for the VM heap and the system;
 * elsewhere, and when that heap is full, from malloc. Never copy it with
 * memcpy/memmove (they may move single bytes): use mini_words_copy. Never
 * read a byte or halfword of a word there (a cast to uint8_t/uint16_t of a
 * loaded word lets the compiler load just that part, l8ui/l16ui, which
 * faults): pack sub-word fields and read whole words through volatile. */
void  *mini_word_alloc(size_t bytes);
void  *mini_word_grow(void *old, size_t old_bytes, size_t new_bytes);
void   mini_words_copy(void *dst, const void *src, size_t bytes);
void   mini_words_fill(void *dst, uint32_t value, size_t bytes);
/* Bytes of VM structures placed in IRAM so far (0 off the ESP32) */
size_t mini_word_iram_bytes(void);

/* Memory a module holds, for the heap map (GET /api/heapmap): fn gets each
 * block as malloc returned it, with a short label */
typedef void (*MiniMemOwnerFn)(const void *block, const char *what, void *user);
void mini_vm_mem_owners(MiniVM *vm, MiniMemOwnerFn fn, void *user);
void mini_gui_mem_owners(MiniMemOwnerFn fn, void *user);
void mini_json_feed_mem_owners(MiniMemOwnerFn fn, void *user);
void mini_font_mem_owners(MiniMemOwnerFn fn, void *user);

/* Allocation attribution for benchmarks (tools/bench_device.c); compiled
 * out unless MINI_ALLOC_PROFILE is defined */
#ifdef MINI_ALLOC_PROFILE
extern const char *g_mini_prof_site;
extern const void *g_mini_prof_code; /* code object behind <env-frame>/<closure> */
void mini_prof_alloc(size_t bytes, unsigned type);
#define PROF_SAVE(var)     const char *var = g_mini_prof_site
#define PROF_SITE(s)       (g_mini_prof_site = (s))
#define PROF_CODE(c)       (g_mini_prof_code = (const void *)(c))
#else
#define PROF_SAVE(var)     ((void)0)
#define PROF_SITE(s)       ((void)0)
#define PROF_CODE(c)       ((void)0)
#endif
#define UNTAG_PTR(v) ((void *)((v) & ~(uintptr_t)MINI_TAG_MASK))

typedef enum {
    OP_HALT = 0,
    OP_CONST,
    OP_INT8,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_VOID,

    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_DEF_GLOBAL,

    OP_POP,
    OP_JUMP,
    OP_JUMP_IF_FALSE,

    OP_CLOSURE,
    OP_CALL,
    OP_TAIL_CALL,
    OP_RETURN,

    /* API 1.14: (+ a b) (- a b) (* a b) (< a b) (> a b) (<= a b) (>= a b) (= a b) of the
     * global procedures, inline: two fixnums are computed here, anything else (flonums,
     * bigints, an overflow, a wrong type) goes to the primitive, as the call would */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_NUM_EQ,

    /* API 1.15: (f a ...) of an API procedure by its permanent number (minischeme_api_ids.h):
     * operands u16 number, u8 argc; the procedure is found once per VM (its global binding if
     * the name is bound, else the firmware's C function) */
    OP_CALL_PRIM,
    /* API 1.15: (car p) (cdr p) (not x) (null? x) (pair? x) (eq? a b) (vector-ref v k) inline */
    OP_CAR,
    OP_CDR,
    OP_NOT,
    OP_NULLP,
    OP_PAIRP,
    OP_EQ,
    OP_VECTOR_REF,
    OP_COUNT
} OpCode;

typedef struct CallFrame {
    ClosureObj *closure;
    CodeObj *code;
    uint8_t *pc;
    Value *fp;
    Value env;
} CallFrame;

/* The global cache: MINI_GCACHE_SIZE entries in sets of MINI_GCACHE_WAYS (vm.c global_binding) */
#define MINI_GCACHE_SIZE 128
#define MINI_GCACHE_WAYS 4
#define MINI_GCACHE_SETS (MINI_GCACHE_SIZE / MINI_GCACHE_WAYS)
typedef struct {
    Value sym;
    Value binding;
    uint32_t gen;
} MiniGlobalCacheEntry;

typedef struct HostCFuncEntry {
    char *name;
    MiniCFunc fn;
} HostCFuncEntry;

struct MiniVM {
    /* Eden Generation (Nursery) */
    char *eden_raw;
    char *eden_start;
    char *eden_hp;
    char *eden_limit;
    size_t eden_size;

    /* Tenured Generation (Old) */
    char *tenured_raw;
    char *tenured_start;
    char *tenured_hp;
    char *tenured_limit;
    size_t tenured_size;

    size_t heap_size;
    size_t max_memory_bytes;

    /* Remembered Set (Tenured -> Eden pointers) */
    Value *remset;
    size_t remset_count;
    size_t remset_cap;

    /* Mark Stack for Major GC Mark Phase */
    HeapObj **mark_stack;
    size_t mark_stack_count;
    size_t mark_stack_cap;

    /* Legacy compatibility fields */
    char *from_space_raw;
    char *to_space_raw;
    char *from_space;
    char *to_space;
    char *hp;
    char *hp_limit;

    Value *stack;
    Value *sp;
    size_t stack_cap;

    CallFrame *call_frames;
    int call_frame_count;
    size_t frame_cap;

    Value **c_roots;
    size_t c_roots_count;
    size_t c_roots_cap;

    Value global_env;
    Value baseline_env;
    Value symbols;

    Value sym_quote;
    Value sym_if;
    Value sym_begin;
    Value sym_lambda;
    Value sym_define;
    Value sym_set;
    Value sym_let;
    Value sym_letstar;
    Value sym_letrec;

    Value sym_define_syntax;
    Value sym_syntax_rules;
    Value sym_define_macro;
    Value sym_ellipsis;
    Value sym_wildcard;
    uint32_t gensym_counter;

    /* Host C function registry and dynamic resolver */
    HostCFuncEntry *host_cfuncs;
    /* Static primitive tables, bound lazily on first reference */
#define MINI_MAX_PRIM_TABLES 16
    const MiniPrimEntry *prim_tables[MINI_MAX_PRIM_TABLES];
    size_t prim_table_sizes[MINI_MAX_PRIM_TABLES];
    int prim_table_count;
    size_t host_cfuncs_count;
    size_t host_cfuncs_cap;

    MiniCResolver c_resolver;
    void *c_resolver_data;

    /* Dynamically persisted C function names */
    char **cfunc_names;
    size_t cfunc_names_count;
    size_t cfunc_names_cap;

    bool state_reloaded;
    int continuation_base_frame;

    /* ROM / Flash XIP Execution Context */
    const char *rom_base;              /* Base address of heap payload in ROM */
    size_t      rom_size;              /* Total bytes in ROM payload */
    const char *rom_end;               /* rom_base + rom_size */
    const uint8_t *rom_cfunc_table;    /* Pointer to C relocation table in ROM */
    uint32_t    rom_cfunc_count;       /* Number of C relocations in ROM */
    uint64_t    rom_global_env_offset; /* Offset of global_env in ROM payload */
    uint64_t    rom_symbols_offset;    /* Offset of symbols in ROM payload */
    uint8_t    *rom_owned_buffer;      /* If FASL was loaded from file via XIP, non-NULL to free upon destroy */

    /* Global binding cache: a global has exactly one binding pair, so a hit
     * stays valid until a GC moves objects or the environment is replaced;
     * both bump gcache_gen, which retires every entry at once */
    MiniGlobalCacheEntry *gcache;   /* [MINI_GCACHE_SIZE], word memory (IRAM on the ESP32) */
    uint32_t gcache_gen;
    /* One global binding per Code ROM symbol, by its slot (MINI_SYM_SLOT_SHIFT): 0 until looked
     * up. A GC updates the entries (they are roots); a new global environment clears them. */
    Value *gslots;
    uint32_t gslot_count;
    /* OP_CALL_PRIM: the procedure of each API number, found at its first call (NULL until then) */
    MiniCFunc *prim_fns;

    /* Runtime statistics (see mini_get_stats) */
    uint64_t stat_alloc_bytes;
    uint32_t stat_minor_gcs;
    uint32_t stat_major_gcs;
    size_t   stat_live_bytes;
    size_t   stat_live_peak;
    uint32_t stat_frame_peak;
    uint32_t stat_stack_peak;

    /* Code ROM image (mini_rom_attach): pre-linked literals run in place */
    const char  *xrom_start;
    const char  *xrom_end;
    const Value *xrom_syms;            /* symbols sorted by name */
    uint32_t     xrom_sym_count;

    char error_buf[MAX_ERROR_LEN];
    char *trace_buf;
    bool has_error;
    bool stack_trace_enabled;
};

/* Internal VM helpers */
char  *vm_strdup(const char *s);
void   vm_error(MiniVM *vm, const char *fmt, ...);
const char *mini_get_error_trace(const MiniVM *vm);
void   mini_set_stack_trace_enabled(MiniVM *vm, bool enabled);
bool   mini_is_stack_trace_enabled(const MiniVM *vm);
size_t mini_capture_stack_trace(MiniVM *vm, char *buf, size_t max_len);

/* Memory and Generational Garbage Collection */
char *alloc_aligned8(size_t size, char **raw_ptr);
void *gc_alloc(MiniVM *vm, size_t size, MiniObjType type);
void  minor_gc(MiniVM *vm, size_t needed_bytes);
void  major_gc(MiniVM *vm, size_t needed_bytes);
void  cheney_gc(MiniVM *vm, size_t needed_bytes);
void  mini_remset_add(MiniVM *vm, Value parent);
void  mini_set_active_vm(MiniVM *vm);
MiniVM *mini_get_active_vm(void);

/* Generational Write Barrier (Tenured -> Eden reference tracking) */
static inline void mini_write_barrier(MiniVM *vm, Value parent, Value child) {
    if (!vm) return;
    uintptr_t child_tag = child & MINI_TAG_MASK;
    if (child_tag == MINI_TAG_FIXNUM || child_tag == MINI_TAG_CONST) return;

    uintptr_t parent_tag = parent & MINI_TAG_MASK;
    if (parent_tag == MINI_TAG_FIXNUM || parent_tag == MINI_TAG_CONST) return;

    void *parent_ptr = UNTAG_PTR(parent);
    void *child_ptr  = UNTAG_PTR(child);
    if (!parent_ptr || !child_ptr) return;

    if ((char *)child_ptr >= vm->eden_start && (char *)child_ptr < vm->eden_limit) {
        if ((char *)parent_ptr >= vm->tenured_start && (char *)parent_ptr < vm->tenured_limit) {
            HeapObj *obj = (HeapObj *)parent_ptr;
            if (!(obj->type & MINI_GC_REMSET_BIT)) {
                obj->type |= MINI_GC_REMSET_BIT;
                mini_remset_add(vm, parent);
            }
        }
    }
}

/* Internal Constructors */
Value mini_make_code(MiniVM *vm, uint32_t code_len, const uint8_t *code_bytes,
                     uint16_t num_params, uint16_t max_stack, uint8_t is_variadic,
                     uint8_t has_inner_lambdas, uint16_t num_consts, const Value *consts);
Value mini_make_closure(MiniVM *vm, Value code, Value env);
Value mini_make_continuation(MiniVM *vm, uint32_t frame_count, const CallFrame *frames,
                             uint32_t stack_len, const Value *stack_vals, uint32_t base_frame);
Value mini_make_macro(MiniVM *vm, uint32_t is_syntax_rules, Value proc_or_literals, Value rules);

/* BigInt Internal API */
Value  bigint_alloc(MiniVM *vm, int32_t sign, uint32_t length);
Value  bigint_normalize(MiniVM *vm, Value v);
Value  bigint_from_int64(MiniVM *vm, int64_t n);
/* An integer from 32-bit limbs (least significant first), as a fixnum when it fits */
Value  bigint_from_limbs(MiniVM *vm, int32_t sign, const uint32_t *limbs, uint32_t n);
Value  bigint_from_str(MiniVM *vm, const char *str, int radix);
char  *bigint_to_str(Value v, int radix);
double bigint_to_double(Value v);
bool   bigint_to_int64(Value v, int64_t *out);

Value bigint_add(MiniVM *vm, Value a, Value b);
Value bigint_sub(MiniVM *vm, Value a, Value b);
Value bigint_mul(MiniVM *vm, Value a, Value b);
bool  bigint_div_rem(MiniVM *vm, Value a, Value b, Value *out_quot, Value *out_rem);
int   bigint_cmp(Value a, Value b);
Value bigint_negate(MiniVM *vm, Value v);
Value bigint_abs(MiniVM *vm, Value v);

Value bigint_bitwise_and(MiniVM *vm, Value a, Value b);
Value bigint_bitwise_ior(MiniVM *vm, Value a, Value b);
Value bigint_bitwise_xor(MiniVM *vm, Value a, Value b);
Value bigint_bitwise_not(MiniVM *vm, Value a);
Value bigint_shift(MiniVM *vm, Value a, int64_t shift);

/* VM Execution Engine */
Value mini_execute(MiniVM *vm, ClosureObj *entry_closure, int argc, Value *argv);
Value mini_vm_run(MiniVM *vm, Value initial_closure, int argc, Value *argv);

/* Environment Operations */
Value env_lookup(MiniVM *vm, Value env, Value sym);
Value env_lookup_name(MiniVM *vm, const char *name);
void  env_define(MiniVM *vm, Value env, Value sym, Value val);
void  env_set(MiniVM *vm, Value env, Value sym, Value val);
Value make_cfunc(MiniVM *vm, MiniCFunc fn, const char *name);
Value prim_callcc(MiniVM *vm, int argc, Value *argv);
Value prim_car(MiniVM *vm, int argc, Value *argv);
Value prim_cdr(MiniVM *vm, int argc, Value *argv);
Value prim_vector_ref(MiniVM *vm, int argc, Value *argv);
/* (%api-id name): the API procedure's permanent number, or #f (the compiler's OP_CALL_PRIM) */
Value prim_api_id(MiniVM *vm, int argc, Value *argv);

/* Built-in Primitives Registration & Table */
typedef MiniPrimEntry BuiltinCFuncEntry;

extern const BuiltinCFuncEntry BUILTIN_CFUNCS[];
void mini_register_primitives(MiniVM *vm);
#include "minischeme_help.h"

/* Math Primitives */
Value prim_add(MiniVM *vm, int argc, Value *argv);
Value prim_sub(MiniVM *vm, int argc, Value *argv);
Value prim_mul(MiniVM *vm, int argc, Value *argv);
Value prim_div(MiniVM *vm, int argc, Value *argv);
Value prim_quotient(MiniVM *vm, int argc, Value *argv);
Value prim_remainder(MiniVM *vm, int argc, Value *argv);
Value prim_modulo(MiniVM *vm, int argc, Value *argv);
Value prim_lt(MiniVM *vm, int argc, Value *argv);
Value prim_gt(MiniVM *vm, int argc, Value *argv);
Value prim_le(MiniVM *vm, int argc, Value *argv);
Value prim_ge(MiniVM *vm, int argc, Value *argv);
Value prim_num_eq(MiniVM *vm, int argc, Value *argv);
Value prim_sqrt(MiniVM *vm, int argc, Value *argv);
Value prim_sin(MiniVM *vm, int argc, Value *argv);
Value prim_cos(MiniVM *vm, int argc, Value *argv);
Value prim_tan(MiniVM *vm, int argc, Value *argv);
Value prim_asin(MiniVM *vm, int argc, Value *argv);
Value prim_acos(MiniVM *vm, int argc, Value *argv);
Value prim_atan(MiniVM *vm, int argc, Value *argv);
Value prim_exp(MiniVM *vm, int argc, Value *argv);
Value prim_log(MiniVM *vm, int argc, Value *argv);
Value prim_expt(MiniVM *vm, int argc, Value *argv);
Value prim_abs(MiniVM *vm, int argc, Value *argv);
Value prim_floor(MiniVM *vm, int argc, Value *argv);
Value prim_ceiling(MiniVM *vm, int argc, Value *argv);
Value prim_round(MiniVM *vm, int argc, Value *argv);
Value prim_truncate(MiniVM *vm, int argc, Value *argv);
Value prim_exact_to_inexact(MiniVM *vm, int argc, Value *argv);
Value prim_inexact_to_exact(MiniVM *vm, int argc, Value *argv);
Value prim_integer_p(MiniVM *vm, int argc, Value *argv);
Value prim_flonum_p(MiniVM *vm, int argc, Value *argv);
Value prim_exact_p(MiniVM *vm, int argc, Value *argv);
Value prim_inexact_p(MiniVM *vm, int argc, Value *argv);
Value prim_number_p(MiniVM *vm, int argc, Value *argv);
Value prim_min(MiniVM *vm, int argc, Value *argv);
Value prim_max(MiniVM *vm, int argc, Value *argv);

/* Helper for double conversion */
static inline double to_double_val(Value v) {
    if (mini_is_fixnum(v)) return (double)mini_to_fixnum(v);
    if (mini_is_flonum(v)) return mini_to_flonum(v);
    if (mini_is_bigint(v)) return bigint_to_double(v);
    return 0.0;
}

Value prim_system(MiniVM *vm, int argc, Value *argv);
Value prim_file_size(MiniVM *vm, int argc, Value *argv);
Value prim_file_remove(MiniVM *vm, int argc, Value *argv);
Value prim_write_file_string(MiniVM *vm, int argc, Value *argv);
Value prim_fasl_to_c(MiniVM *vm, int argc, Value *argv);
Value prim_load_fasl_xip(MiniVM *vm, int argc, Value *argv);
Value prim_xip_active_p(MiniVM *vm, int argc, Value *argv);
Value prim_xip_stats(MiniVM *vm, int argc, Value *argv);

/* FASL & XIP Execution Engine Helpers */
Value rom_materialize_val(MiniVM *vm, uint64_t raw_val);
Value rom_on_demand_load(MiniVM *vm, Value sym);
/* Global not bound in RAM: try the XIP image, then the primitive tables.
 * Binds and returns the value, or void. May allocate (sym is rooted). */
Value global_miss(MiniVM *vm, Value sym);

/* Set by mini_register_api_lib (minischeme_api.c), NULL before: the i-th
 * other name of an API procedure (its older or current name), and a remark
 * for an unbound name (" (reserved API ...)") or NULL */
extern const char *(*mini_api_alias_hook)(const char *name, size_t i);
extern const char *(*mini_api_unbound_hook)(const char *name);
/* Interned symbol of the attached Code ROM, or 0 */
Value xrom_find_symbol(const MiniVM *vm, const char *name, size_t len);
/* The global environment was replaced or its keys changed: drop what lookups remembered */
void mini_globals_reset(MiniVM *vm);
const MiniPrimEntry *prim_table_find(const MiniVM *vm, const char *name);
const char *rom_find_cfunc_name(MiniVM *vm, uint64_t heap_offset);

#endif /* MINISCHEME_INTERNAL_H */
