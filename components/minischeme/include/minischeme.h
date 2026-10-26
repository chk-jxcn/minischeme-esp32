/**
 * @file minischeme.h
 * @brief MiniScheme: A high-performance, lightweight, embeddable Scheme interpreter.
 * 
 * Inspired by Chez Scheme's architectural philosophies:
 *  - 64-bit tagged pointer representation (immediate fixnums, booleans, nil, void).
 *  - Dynamic 2-space Cheney copying GC with automatic geometric expansion (unbounded heap).
 *  - Proper tail-call optimization (TCO) for O(1) space loops and recursion.
 *  - Unbounded re-entrant C FFI (C calls Scheme -> Scheme calls C -> C calls Scheme...).
 */

#ifndef MINISCHEME_H
#define MINISCHEME_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Value and Tag Definitions                                                 */
/* ========================================================================= */

/**
 * All Scheme values are represented as a 64-bit unsigned integer.
 * Primary type is encoded in the lowest 3 bits (8-byte pointer alignment):
 *   TAG_FIXNUM  (0b000): 63-bit signed integer immediate (val >> 1)
 *   TAG_PAIR    (0b001): Heap-allocated Pair (car, cdr)
 *   TAG_CLOSURE (0b010): Heap-allocated Closure (params, body, env)
 *   TAG_SYMBOL  (0b011): Interned Symbol
 *   TAG_STRING  (0b100): Heap-allocated String
 *   TAG_CFUNC   (0b101): Registered C Function pointer
 *   TAG_CONST   (0b111): Immediate constants (NIL, TRUE, FALSE, VOID, EOF)
 */
typedef uintptr_t Value;

/* Tag bitmasks */
#define MINI_TAG_MASK     0x7
#define MINI_TAG_FIXNUM   0x0
#define MINI_TAG_PAIR     0x1
#define MINI_TAG_CLOSURE  0x2
#define MINI_TAG_SYMBOL   0x3
#define MINI_TAG_STRING   0x4
#define MINI_TAG_CFUNC    0x5
#define MINI_TAG_RECORD   0x6
#define MINI_TAG_CODE     MINI_TAG_RECORD
#define MINI_TAG_CONST    0x7

/* Immediate Constant Identifiers */
#define MINI_CONST_NIL    ((0 << 3) | MINI_TAG_CONST)
#define MINI_CONST_TRUE   ((1 << 3) | MINI_TAG_CONST)
#define MINI_CONST_FALSE  ((2 << 3) | MINI_TAG_CONST)
#define MINI_CONST_VOID   ((3 << 3) | MINI_TAG_CONST)
#define MINI_CONST_EOF    ((4 << 3) | MINI_TAG_CONST)

/* Record / Heap Object Subtypes */
typedef enum {
    MINI_OBJ_PAIR = 1,
    MINI_OBJ_CLOSURE,
    MINI_OBJ_SYMBOL,
    MINI_OBJ_STRING,
    MINI_OBJ_CFUNC,
    MINI_OBJ_CODE,
    MINI_OBJ_VECTOR,
    MINI_OBJ_FLONUM,
    MINI_OBJ_FORWARDED,
    MINI_OBJ_CONTINUATION,
    MINI_OBJ_MACRO,
    MINI_OBJ_BIGINT
} MiniObjType;

#define MINI_OBJ_TYPE_MASK 0x000000FFU

typedef struct MiniHeapObj {
    uint32_t type;
    uint32_t size;
} MiniHeapObj;

/* Opaque VM State */
typedef struct MiniVM MiniVM;

/* Function pointer prototype for C functions callable from Scheme */
typedef Value (*MiniCFunc)(MiniVM *vm, int argc, Value *argv);

/* ========================================================================= */
/* Configuration and Memory Profiles                                         */
/* ========================================================================= */

typedef enum {
    MINI_PROFILE_DEFAULT = 0,     /* Recommended for 1MB+ devices (ESP32-S3 / PSRAM / PC): 64KB initial Tenured, 16KB Eden */
    MINI_PROFILE_EMBEDDED_200K,   /* Compact profile for 200KB SRAM microcontrollers (e.g. classic ESP32): 16KB initial, 64KB cap, compact stack */
    MINI_PROFILE_CUSTOM           /* Custom configuration parameters */
} MiniProfile;

typedef struct MiniConfig {
    size_t initial_heap_bytes;    /* Initial Tenured heap size in bytes (default 64KB / compact 16KB) */
    size_t max_heap_bytes;        /* Maximum Tenured heap limit in bytes (0 for unlimited, compact 64KB) */
    size_t stack_size;            /* Evaluation stack capacity (slots, default 1024 / compact 512) */
    size_t frame_capacity;        /* Call frame capacity (default 256 / compact 128) */
    bool skip_init_fasl;          /* If true, do not automatically load embedded init_fasl upon initialization */
    size_t eden_bytes;            /* Nursery (Eden) generation size in bytes (0 for default 16KB) */
    bool enable_stack_trace;      /* If true, capture stack trace on errors (default false on hardware, true in WASM) */
} MiniConfig;

/* ========================================================================= */
/* VM Lifecycle                                                              */
/* ========================================================================= */

/**
 * @brief Create and initialize a new MiniScheme virtual machine instance.
 * @param initial_heap_bytes Initial heap size in bytes (e.g., 64 * 1024). Pass 0 for default (64 KB).
 * @return Pointer to the newly allocated MiniVM.
 */
MiniVM *mini_create(size_t initial_heap_bytes);

/**
 * @brief Create MiniScheme VM with a specific fine-grained configuration.
 */
MiniVM *mini_create_ext(const MiniConfig *config);

/**
 * @brief Create MiniScheme VM using a preset hardware profile.
 */
MiniVM *mini_create_profile(MiniProfile profile);

/**
 * @brief Destroy a MiniScheme VM and release all allocated resources.
 * @param vm VM instance to destroy.
 */
void mini_destroy(MiniVM *vm);

/**
 * @brief Set an optional maximum memory limit for the VM heap (0 = unbounded).
 * @param vm VM instance.
 * @param max_bytes Maximum heap size in bytes, or 0 for unlimited.
 */
void mini_set_memory_limit(MiniVM *vm, size_t max_bytes);

/**
 * @brief Get the current total heap capacity in bytes.
 */
size_t mini_get_heap_size(const MiniVM *vm);

/**
 * @brief Get the current allocated bytes in the active heap space.
 */
size_t mini_get_allocated_bytes(const MiniVM *vm);

/**
 * @brief Manually trigger a garbage collection cycle.
 */
void mini_gc(MiniVM *vm);

/* ========================================================================= */
/* Evaluation and Execution                                                  */
/* ========================================================================= */

/**
 * @brief Evaluate a null-terminated string of Scheme code.
 * @param vm VM instance.
 * @param str Scheme source string.
 * @return Result value of the last evaluated expression.
 */
Value mini_eval_string(MiniVM *vm, const char *str);

/**
 * @brief Evaluate a Scheme script file.
 * @param vm VM instance.
 * @param filepath Path to the script file.
 * @return Result value of the last evaluated expression in the file.
 */
Value mini_eval_file(MiniVM *vm, const char *filepath);

/**
 * @brief Load an initialization script (e.g., init.scm).
 * @param vm VM instance.
 * @param init_path Path to the initialization script.
 * @return true on success, false if the file could not be read or evaluated.
 */
bool mini_load_init(MiniVM *vm, const char *init_path);

/**
 * @brief Load Scheme code from an in-memory string as initialization script.
 * Useful for embedded environments without file systems.
 * @param vm VM instance.
 * @param scm_code Source code string.
 * @return true on success, false on error.
 */
bool mini_load_init_string(MiniVM *vm, const char *scm_code);

/* ========================================================================= */
/* C and Scheme Interoperability (FFI)                                       */
/* ========================================================================= */

/**
 * @brief Register a C function as a global procedure in the Scheme environment.
 * @param vm VM instance.
 * @param name Function name in Scheme (e.g., "my-add", "log-message").
 * @param fn C function pointer matching MiniCFunc.
 */
void mini_register_c_func(MiniVM *vm, const char *name, MiniCFunc fn);

/** @brief One entry of a static primitive table. */
typedef struct MiniPrimEntry {
    const char *name;
    MiniCFunc fn;
} MiniPrimEntry;

/**
 * @brief Register a static table of C primitives without using heap memory.
 *
 * Only the table pointer is stored; a primitive is bound into the global
 * environment the first time it is referenced. The table (names included)
 * must outlive the VM, e.g. `static const` data kept in Flash. Bindings made
 * with mini_register_c_func / define take precedence; among tables the one
 * registered last wins. Registering the same table twice is a no-op.
 * @param count Number of entries; a NULL name also ends the table.
 */
void mini_register_prim_table(MiniVM *vm, const MiniPrimEntry *table, size_t count);

/* ========================================================================= */
/* Code ROM: run an application's bytecode and literals in place (XIP)      */
/* ========================================================================= */

/** @brief Sink for mini_rom_build: store len bytes at offset of the image. */
typedef bool (*MiniRomWriteFn)(size_t offset, const void *data, size_t len, void *user);

/**
 * @brief Build a Code ROM image from an MSBC top-level code object.
 *
 * The code object and every literal it references (nested code, symbols,
 * strings, numbers, quoted lists/vectors) are emitted pre-linked to run at
 * address `base`. Writes are sequential except the header, which comes last
 * (offset 0), so the image can stream straight to erased Flash.
 * The VM must not have an image attached; `root` stays valid in RAM.
 * @return Image size in bytes, or 0 on failure (see mini_get_error).
 */
size_t mini_rom_build(MiniVM *vm, Value root, uintptr_t base, uint32_t key,
                      MiniRomWriteFn write, void *user);

/** @brief Source for mini_rom_build_stream: up to n bytes of the MSBC; 0 at the end. */
typedef size_t (*MiniRomReadFn)(void *buf, size_t n, void *user);

/**
 * @brief Build a Code ROM image straight from an MSBC stream, without a VM.
 *
 * Same image format as mini_rom_build, but each object is written as it is
 * read and the references to it are filled in afterwards, so memory stays a
 * few KB whatever the app's size (a 2 KB window of recent bytes, the symbol
 * index). The sink sees writes out of order: a slot written once more after
 * the bytes around it (into erased flash that is fine, bits only go 1 -> 0).
 * `image` reads back what has been written, for symbol names and the
 * checksum (the mapped image; NULL: `base` itself).
 * @return Image size in bytes, or 0 (*err says why). *peak_mem: the most
 *         memory it held at once.
 */
size_t mini_rom_build_stream(MiniRomReadFn read, void *read_user, uintptr_t base, const void *image,
                             uint32_t key, MiniRomWriteFn write, void *write_user,
                             size_t *peak_mem, const char **err);

/**
 * @brief Attach a Code ROM image mapped at `image` (its link address).
 *
 * Validates header, key and checksum; on success the image's symbols become
 * the interned ones. Call on a fresh VM before running any code. The image
 * must stay mapped for the VM's lifetime and is never written.
 * @return The top-level code object to run, or void if the image is invalid.
 */
Value mini_rom_attach(MiniVM *vm, const void *image, size_t size, uint32_t key);

/** @brief Validate an image (header, link address, key, checksum) without a VM. */
bool mini_rom_check(const void *image, size_t size, uint32_t key);

/** @brief True if v is an object inside the attached Code ROM image. */
bool mini_rom_contains(const MiniVM *vm, Value v);

/** @brief Bytes of the attached Code ROM image (0 if none). */
size_t mini_rom_size(const MiniVM *vm);

/* ========================================================================= */
/* Runtime statistics (cheap counters, always on)                           */
/* ========================================================================= */

typedef struct MiniStats {
    uint64_t alloc_bytes;     /* bytes allocated since VM creation */
    uint32_t minor_gcs;
    uint32_t major_gcs;
    size_t   live_bytes;      /* tenured bytes live after the last major GC */
    size_t   live_peak;       /* highest tenured occupancy seen by a GC */
    uint32_t frame_peak;      /* deepest call frame count */
    uint32_t stack_peak;      /* deepest value stack (slots) at a call */
    size_t   heap_bytes;      /* tenured capacity */
    size_t   eden_bytes;
    size_t   stack_slots;
    size_t   frame_slots;
    size_t   rom_bytes;
} MiniStats;

/** @brief Fill *out with the VM's counters. */
void mini_get_stats(const MiniVM *vm, MiniStats *out);

/**
 * @brief Snapshot current global environment as the baseline.
 * Subsequent definitions will be treated as user-defined.
 */
void mini_set_baseline_env(MiniVM *vm);

/**
 * @brief Look up a global variable/procedure by name.
 * @param vm VM instance.
 * @param name Identifier name.
 * @return The bound Value, or MINI_CONST_VOID if unbound.
 */
Value mini_get_global(MiniVM *vm, const char *name);

/**
 * @brief Set a global variable.
 * @param vm VM instance.
 * @param name Identifier name.
 * @param val Value to bind.
 */
void mini_set_global(MiniVM *vm, const char *name, Value val);

/**
 * @brief Call a Scheme procedure from C.
 * Supports arbitrary recursion depth and re-entrancy (C -> Scheme -> C -> Scheme).
 * @param vm VM instance.
 * @param func Callable Value (Closure or CFunc).
 * @param argc Number of arguments.
 * @param argv Array of argument Values.
 * @return Return value of the procedure.
 */
Value mini_call(MiniVM *vm, Value func, int argc, Value *argv);

/**
 * @brief Call a Scheme procedure by its global name from C.
 * @param vm VM instance.
 * @param func_name Global identifier of the procedure.
 * @param argc Number of arguments.
 * @param argv Array of argument Values.
 * @return Return value of the procedure.
 */
Value mini_call_named(MiniVM *vm, const char *func_name, int argc, Value *argv);

/**
 * @brief Invoke a captured Scheme first-class continuation directly from C.
 * @param vm VM instance.
 * @param cont Captured Continuation object.
 * @param arg Value passed as the continuation's result.
 * @return Result value after resuming execution.
 */
Value mini_continuation_call(MiniVM *vm, Value cont, Value arg);

/* ========================================================================= */
/* Value Constructors and Type Checks                                        */
/* ========================================================================= */

/* Immediate Constants */
static inline Value mini_nil(void)   { return MINI_CONST_NIL; }
static inline Value mini_true(void)  { return MINI_CONST_TRUE; }
static inline Value mini_false(void) { return MINI_CONST_FALSE; }
static inline Value mini_void(void)  { return MINI_CONST_VOID; }
static inline Value mini_eof(void)   { return MINI_CONST_EOF; }

/* Type Check Predicates */
static inline bool mini_is_nil(Value v)     { return v == MINI_CONST_NIL; }
static inline bool mini_is_true(Value v)    { return v == MINI_CONST_TRUE; }
static inline bool mini_is_false(Value v)   { return v == MINI_CONST_FALSE; }
static inline bool mini_is_bool(Value v)    { return v == MINI_CONST_TRUE || v == MINI_CONST_FALSE; }
static inline bool mini_is_void(Value v)    { return v == MINI_CONST_VOID; }
static inline bool mini_is_eof(Value v)     { return v == MINI_CONST_EOF; }
static inline bool mini_is_fixnum(Value v)  { return (v & MINI_TAG_MASK) == MINI_TAG_FIXNUM; }
static inline bool mini_is_pair(Value v)    { return (v & MINI_TAG_MASK) == MINI_TAG_PAIR; }
static inline bool mini_is_closure(Value v) { return (v & MINI_TAG_MASK) == MINI_TAG_CLOSURE; }
static inline bool mini_is_symbol(Value v)  { return (v & MINI_TAG_MASK) == MINI_TAG_SYMBOL; }
static inline bool mini_is_string(Value v)  { return (v & MINI_TAG_MASK) == MINI_TAG_STRING; }
static inline bool mini_is_cfunc(Value v)   { return (v & MINI_TAG_MASK) == MINI_TAG_CFUNC; }
static inline bool mini_is_record(Value v)  { return (v & MINI_TAG_MASK) == MINI_TAG_RECORD; }

static inline bool mini_is_code(Value v) {
    return mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_CODE;
}
static inline bool mini_is_vector(Value v) {
    return mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_VECTOR;
}
static inline bool mini_is_flonum(Value v) {
    return mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_FLONUM;
}
static inline bool mini_is_continuation(Value v) {
    return mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_CONTINUATION;
}
static inline bool mini_is_macro(Value v) {
    return mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_MACRO;
}
static inline bool mini_is_bigint(Value v) {
    return mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_BIGINT;
}
static inline bool mini_is_integer(Value v) {
    return mini_is_fixnum(v) || mini_is_bigint(v);
}
static inline bool mini_is_number(Value v) {
    return mini_is_fixnum(v) || mini_is_flonum(v) || mini_is_bigint(v);
}

/* Fixnum conversions (signed integer immediate, 3-bit tag = 0b000) */
static inline Value mini_make_fixnum(int64_t n) {
    return (Value)(((uintptr_t)(intptr_t)n << 3) | MINI_TAG_FIXNUM);
}

static inline int64_t mini_to_fixnum(Value v) {
    return (int64_t)((intptr_t)v >> 3);
}

static inline Value mini_make_bool(bool b) {
    return b ? MINI_CONST_TRUE : MINI_CONST_FALSE;
}

static inline bool mini_to_bool(Value v) {
    return v != MINI_CONST_FALSE; /* In Scheme, everything except #f is truthy */
}

/* Flonum (IEEE-754 64-bit double float) */
Value  mini_make_flonum(MiniVM *vm, double num);
double mini_to_flonum(Value v);

/* BigInt (Arbitrary-precision integer) */
Value  mini_make_bigint_from_int64(MiniVM *vm, int64_t n);
Value  mini_make_bigint_from_str(MiniVM *vm, const char *str, int radix);
char  *mini_bigint_to_str(Value v, int radix);
bool   mini_bigint_to_int64(Value v, int64_t *out);

/* Vector */
Value  mini_make_vector(MiniVM *vm, size_t length, Value fill);
size_t mini_vector_length(Value v);
Value  mini_vector_ref(MiniVM *vm, Value v, size_t idx);
void   mini_vector_set(MiniVM *vm, Value v, size_t idx, Value val);

/* Heap Object Constructors */
Value mini_make_cons(MiniVM *vm, Value car, Value cdr);
Value mini_make_symbol(MiniVM *vm, const char *name);
Value mini_make_string(MiniVM *vm, const char *str);
Value mini_make_string_len(MiniVM *vm, const char *str, size_t len);

/* Accessors. car and cdr are inline: every global lookup and list walk takes them (as calls
 * into another file they cost a call each, 12 million in a 3-million-step loop). A pair is a
 * heap header followed by car and cdr (minischeme_internal.h PairObj, checked there). */
typedef struct MiniPairCells {
    MiniHeapObj header;
    Value car;
    Value cdr;
} MiniPairCells;

static inline Value mini_car(Value pair) {
    return mini_is_pair(pair) ? ((const MiniPairCells *)(pair & ~(Value)MINI_TAG_MASK))->car : MINI_CONST_VOID;
}

static inline Value mini_cdr(Value pair) {
    return mini_is_pair(pair) ? ((const MiniPairCells *)(pair & ~(Value)MINI_TAG_MASK))->cdr : MINI_CONST_VOID;
}

void  mini_set_car(Value pair, Value val);
void  mini_set_cdr(Value pair, Value val);

const char *mini_symbol_name(Value sym);
const char *mini_string_data(Value str);
size_t      mini_string_len(Value str);

/* Equality */
bool mini_eq(Value a, Value b);
bool mini_equal(Value a, Value b);

/* ========================================================================= */
/* GC Root Protection (For C Extension Writers)                              */
/* ========================================================================= */

/**
 * @brief Register a pointer to a Value as a GC root across evaluations/allocations.
 * Useful when holding intermediate heap-allocated Values in host C variables.
 */
void mini_push_root(MiniVM *vm, Value *root_ptr);

/**
 * @brief Unregister the most recently pushed GC root.
 */
void mini_pop_root(MiniVM *vm);

/* ========================================================================= */
/* Printing and Debugging                                                    */
/* ========================================================================= */

/**
 * @brief Print a Scheme value to a file stream (stdout, stderr, etc.).
 * @param vm VM instance.
 * @param v Value to print.
 * @param stream Output file stream.
 * @param quoted If true, print strings with quotes and escapes (write mode).
 */
void mini_print_value(MiniVM *vm, Value v, FILE *stream, bool quoted);

/** @brief A string as `write` shows it: in double quotes, with \" \\ \n \t \r
 *  and \xHH; for other control bytes (UTF-8 text is kept). Writes at most cap
 *  bytes (NUL-terminated when cap > 0) and returns the length it needs. */
size_t mini_format_string_literal(char *buf, size_t cap, Value str);

/**
 * @brief Retrieve the last error message, if an evaluation failed.
 */
const char *mini_get_error(const MiniVM *vm);

/**
 * @brief Retrieve the call stack trace of the last error, if available.
 */
const char *mini_get_error_trace(const MiniVM *vm);

/**
 * @brief Enable or disable stack trace capture upon errors.
 */
void mini_set_stack_trace_enabled(MiniVM *vm, bool enabled);

/**
 * @brief Query whether stack trace capture is currently enabled.
 */
bool mini_is_stack_trace_enabled(const MiniVM *vm);

/**
 * @brief Clear any pending error state.
 */
void mini_clear_error(MiniVM *vm);

/* ========================================================================= */
/* Help and Autocompletion Reference APIs                                    */
/* ========================================================================= */

typedef struct MiniHelpEntry {
    const char *name;         /* Symbol identifier, e.g. "map", "+", "when" */
    const char *syntax;       /* Usage syntax, e.g. "(map proc list ...)" */
    const char *category;     /* Classification, e.g. "Pairs & Lists", "Math" */
    const char *description;  /* Brief summary of behavior */
} MiniHelpEntry;

/**
 * @brief Look up the help metadata for a given symbol name.
 * @param name Function, macro, or primitive name.
 * @return Pointer to MiniHelpEntry, or NULL if not in standard reference table.
 */
const MiniHelpEntry *mini_help_lookup(const char *name);

/**
 * @brief Print reference help to a file stream.
 * @param vm VM instance.
 * @param symbol Optional symbol name (NULL or empty to list all functions).
 * @param stream Output file stream (stdout, etc.).
 */
void mini_print_help(MiniVM *vm, const char *symbol, FILE *stream);

/**
 * @brief Find all symbol completions starting with a given prefix.
 * Searches built-ins, standard library, special forms, macros, and global_env.
 * @param vm VM instance.
 * @param prefix Prefix string to match.
 * @param out_matches Receives dynamically allocated array of matching strings (free with mini_free_completions).
 * @return Number of matching candidates found.
 */
int mini_get_completions(MiniVM *vm, const char *prefix, char ***out_matches);

/**
 * @brief Free the completion matches array returned by mini_get_completions.
 */
void mini_free_completions(char **matches, int count);

/* ========================================================================= */
/* FASL (Fast Loading / Machine State Snapshot & Restoration)                */
/* ========================================================================= */

/**
 * @brief Dynamic C function resolver callback prototype.
 * Invoked during FASL load when a C function in the snapshot is not found in
 * built-in primitives or host pre-registered functions.
 * @param name Symbolic identifier of the C function.
 * @param user_data Host-provided context pointer.
 * @return Resolved MiniCFunc pointer, or NULL if unresolved.
 */
typedef MiniCFunc (*MiniCResolver)(const char *name, void *user_data);

/**
 * @brief Set a dynamic C function resolver callback for FASL loading.
 * @param vm VM instance.
 * @param resolver Callback function.
 * @param user_data Host-provided context passed to resolver.
 */
void mini_set_c_resolver(MiniVM *vm, MiniCResolver resolver, void *user_data);

/**
 * @brief Dump the entire VM machine state to an in-memory buffer.
 * Performs a compaction GC before dumping. All pointers are converted to
 * relative heap offsets.
 * @param vm VM instance.
 * @param out_buf Pointer to receive the allocated buffer (caller frees via free()).
 * @param out_size Pointer to receive the total byte length.
 * @return true on success, false on error.
 */
bool mini_dump_fasl_mem(MiniVM *vm, uint8_t **out_buf, size_t *out_size);

/**
 * @brief Heap layout of a dumped FASL image.
 * All targets are little-endian with an 8-byte object header, so every 32-bit
 * build (ESP32, WASM, i386) shares one layout. That is the portable layout: a
 * 64-bit host converts its heap to it when dumping, a 32-bit target loads it
 * with a plain copy, and a 64-bit host converts it back when loading. Fixnums
 * too wide for 32 bits become bigints in a portable image.
 */
typedef enum {
    MINI_FASL_PORTABLE = 0, /**< 32-bit layout: loads on every target (default) */
    MINI_FASL_NATIVE   = 1  /**< this build's layout; required by mini_load_fasl_xip */
} MiniFaslLayout;

/** @brief mini_dump_fasl_mem with an explicit layout. */
bool mini_dump_fasl_mem_ex(MiniVM *vm, uint8_t **out_buf, size_t *out_size, MiniFaslLayout layout);

/**
 * @brief Restore the entire VM machine state from an in-memory FASL buffer.
 * Images of either pointer width load: the other width is converted.
 * @param vm VM instance.
 * @param buf Pointer to FASL byte array (can reside in Flash / PROGMEM).
 * @param size Length of FASL data in bytes.
 * @return true on success, false on error.
 */
bool mini_load_fasl_mem(MiniVM *vm, const uint8_t *buf, size_t size);

/**
 * @brief Dump the entire VM machine state to a binary FASL image file.
 * Performs a compaction GC before dumping. All pointers are converted to
 * relative heap offsets. C functions are serialized symbolically.
 * @param vm VM instance.
 * @param filepath Path to output FASL file (e.g., "snapshot.fasl").
 * @return true on success, false on I/O or serialization error.
 */
bool mini_dump_fasl(MiniVM *vm, const char *filepath);

/** @brief mini_dump_fasl with an explicit layout. */
bool mini_dump_fasl_ex(MiniVM *vm, const char *filepath, MiniFaslLayout layout);

/**
 * @brief Dump the entire VM machine state as a FASL image to an open file.
 * Streams the compacted heap object by object, so no RAM copy of the image
 * is needed (works on devices whose free RAM is smaller than the heap).
 * @param vm VM instance.
 * @param fp File opened in binary write mode.
 * @return true on success, false on I/O or serialization error.
 */
bool mini_dump_fasl_fp(MiniVM *vm, FILE *fp);

/** @brief mini_dump_fasl_fp with an explicit layout. */
bool mini_dump_fasl_fp_ex(MiniVM *vm, FILE *fp, MiniFaslLayout layout);

/**
 * @brief Restore the entire VM machine state from a binary FASL image file.
 * Restores all global variables, compiled bytecodes, closures, symbols, and
 * complex data structures. Reconnects C functions via 3-tier dynamic relocation.
 * @param vm VM instance.
 * @param filepath Path to FASL file.
 * @return true on success, false on error (check mini_get_error(vm)).
 */
bool mini_load_fasl(MiniVM *vm, const char *filepath);

/**
 * @brief Restore the VM machine state from a FASL image read from an open file.
 * Reads from the current position to the end of the file and streams the heap
 * payload straight into the VM heap (no RAM copy of the image). An image that
 * fits the current heap keeps it; a larger one grows the heap as
 * mini_load_fasl_mem does.
 * @param vm VM instance.
 * @param fp File opened in binary mode, positioned at the image.
 * @return true on success, false on error (check mini_get_error(vm)).
 */
bool mini_load_fasl_fp(MiniVM *vm, FILE *fp);

/**
 * @brief Load and execute a FASL image directly in-place from read-only memory (ROM/Flash).
 *        Bytecodes and constants stay in ROM (0 bytes RAM copied). Functions are loaded
 *        on-demand only when executed.
 * @param vm VM instance.
 * @param rom_buf Pointer to read-only FASL binary buffer in Flash/ROM or .rodata.
 * @param rom_size Size of rom_buf in bytes.
 * @return true on success, false on error.
 */
bool mini_load_fasl_xip(MiniVM *vm, const uint8_t *rom_buf, size_t rom_size);

/**
 * @brief Load and execute a FASL file using XIP (Execute-In-Place).
 * @param vm VM instance.
 * @param filepath Path to FASL file.
 * @return true on success, false on error.
 */
bool mini_load_fasl_file_xip(MiniVM *vm, const char *filepath);

/**
 * @brief Check if XIP execution from ROM/Flash is currently active.
 */
bool mini_is_xip_active(const MiniVM *vm);

/* ------------------------------------------------------------------------- */
/* Access policy                                                             */
/* ------------------------------------------------------------------------- */

/**
 * What the running code may touch: the device layer installs an untrusted
 * app's sandbox (minischeme_device.h, mini_app_sandbox_set); without a
 * policy, or with NULL hooks, everything is allowed (the PC REPL, tools).
 */
typedef struct {
    /** Resolves path for the running code into out; false with why when it
     *  may not read it (write: create, change or remove it). */
    bool (*file)(const char *path, bool write, char *out, size_t cap, char *why, size_t why_cap);
    /** Whether the running code may use privileged procedures (system
     *  settings, the REPL), not just an app. */
    bool (*privileged)(void);
} MiniAccessPolicy;

void mini_set_access_policy(const MiniAccessPolicy *policy);

/** For primitives: path resolved into out, or false after raising "who: why". */
bool mini_access_file(MiniVM *vm, const char *who, const char *path, bool write, char *out, size_t cap);

/** For primitives: true when allowed, else false after raising
 *  "who: not allowed for apps (system settings and the REPL only)". */
bool mini_access_privileged(MiniVM *vm, const char *who);

/** Whether the running code may use privileged procedures (no error raised). */
bool mini_access_is_privileged(void);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_H */
