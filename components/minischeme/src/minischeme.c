/**
 * @file minischeme.c
 * @brief MiniScheme Public API, Reference Help System, and Autocompletion Engine.
 *
 * Implements:
 *  - VM lifecycle management (mini_create, mini_create_ext, mini_destroy).
 *  - Public evaluation and execution entry points (mini_eval_string, mini_call, etc.).
 *  - Interactive printer supporting all Scheme types and BigInts (mini_print_value).
 *  - Built-in documentation reference and help command (mini_print_help, help).
 *  - Dynamic REPL autocompletion engine (mini_get_completions).
 */

#include "minischeme_internal.h"
#include "minischeme_gui.h"
#include "minischeme_cfasl.h"
#include "minischeme_help.h"

#ifdef MINISCHEME_EMBED_INIT
#if UINTPTR_MAX == 0xffffffff
#include "init_fasl32.h"
#else
#include "init_fasl.h"
#endif
#else
static const uint8_t init_fasl_data[] = {0};
static const size_t init_fasl_data_len = 0;
#endif

/* ========================================================================= */
/* Error Handling & Diagnostics                                              */
/* ========================================================================= */

char *vm_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *res = (char *)malloc(len + 1);
    if (!res) return NULL;
    memcpy(res, s, len + 1);
    return res;
}

void vm_error(MiniVM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_buf, MAX_ERROR_LEN, fmt, ap);
    va_end(ap);
    vm->has_error = true;
}

const char *mini_get_error(const MiniVM *vm) {
    return vm->has_error ? vm->error_buf : NULL;
}

void mini_clear_error(MiniVM *vm) {
    vm->has_error = false;
    vm->error_buf[0] = '\0';
    vm->sp = vm->stack;
    vm->call_frame_count = 0;
}

/* ========================================================================= */
/* Global Environment & C Function Registration                             */
/* ========================================================================= */

Value mini_get_global(MiniVM *vm, const char *name) {
    Value sym = mini_make_symbol(vm, name);
    Value val = env_lookup(vm, vm->global_env, sym);
    if (mini_is_void(val)) {
        val = global_miss(vm, sym);
    }
    if (mini_is_void(val)) {
        vm_error(vm, "unbound global variable: %s", name);
    }
    return val;
}

void mini_set_global(MiniVM *vm, const char *name, Value val) {
    *vm->sp++ = val;
    Value sym = mini_make_symbol(vm, name);
    val = *(--vm->sp);
    env_define(vm, vm->global_env, sym, val);
}

Value make_cfunc(MiniVM *vm, MiniCFunc fn, const char *name) {
    CFuncObj *cf = (CFuncObj *)gc_alloc(vm, sizeof(CFuncObj), OBJ_CFUNC);
    if (!cf) return mini_void();
    cf->fn = fn;
    cf->name = name;
    return ((uintptr_t)cf) | MINI_TAG_CFUNC;
}

void mini_register_prim_table(MiniVM *vm, const MiniPrimEntry *table, size_t count) {
    if (!vm || !table) return;
    size_t n = 0;
    while (n < count && table[n].name) n++;
    for (int i = 0; i < vm->prim_table_count; i++) {
        if (vm->prim_tables[i] == table) return;
    }
    if (vm->prim_table_count >= MINI_MAX_PRIM_TABLES) {
        /* Out of table slots: bind eagerly instead */
        for (size_t i = 0; i < n; i++) mini_register_c_func(vm, table[i].name, table[i].fn);
        return;
    }
    vm->prim_tables[vm->prim_table_count] = table;
    vm->prim_table_sizes[vm->prim_table_count] = n;
    vm->prim_table_count++;

    /* Last registration wins: rebind names that are already bound (e.g. by
     * a FASL snapshot loaded earlier). The env is empty on a fresh VM. */
    Value curr = mini_cdr(vm->global_env);
    mini_push_root(vm, &curr);
    while (mini_is_pair(curr)) {
        Value binding = mini_car(curr);
        if (mini_is_pair(binding) && mini_is_symbol(mini_car(binding))) {
            const char *bname = mini_symbol_name(mini_car(binding));
            for (size_t i = n; i-- > 0;) {
                if (strcmp(table[i].name, bname) != 0) continue;
                mini_push_root(vm, &binding);
                Value cf = make_cfunc(vm, table[i].fn, table[i].name);
                mini_pop_root(vm);
                mini_set_cdr(binding, cf);
                mini_write_barrier(vm, binding, cf);
                break;
            }
        }
        curr = mini_cdr(curr);
    }
    mini_pop_root(vm);
}

void mini_register_c_func(MiniVM *vm, const char *name, MiniCFunc fn) {
    Value cf = make_cfunc(vm, fn, name);
    mini_set_global(vm, name, cf);

    /* Record in host C functions registry for FASL relocation */
    for (size_t i = 0; i < vm->host_cfuncs_count; i++) {
        if (strcmp(vm->host_cfuncs[i].name, name) == 0) {
            vm->host_cfuncs[i].fn = fn;
            return;
        }
    }
    if (vm->host_cfuncs_count >= vm->host_cfuncs_cap) {
        size_t new_cap = vm->host_cfuncs_cap ? vm->host_cfuncs_cap * 2 : 16;
        HostCFuncEntry *new_arr = (HostCFuncEntry *)realloc(vm->host_cfuncs, new_cap * sizeof(HostCFuncEntry));
        if (new_arr) {
            vm->host_cfuncs = new_arr;
            vm->host_cfuncs_cap = new_cap;
        }
    }
    if (vm->host_cfuncs_count < vm->host_cfuncs_cap) {
        vm->host_cfuncs[vm->host_cfuncs_count].name = vm_strdup(name);
        vm->host_cfuncs[vm->host_cfuncs_count].fn = fn;
        vm->host_cfuncs_count++;
    }
}



/* ========================================================================= */
/* Public Lifecycle API                                                      */
/* ========================================================================= */

MiniVM *mini_create_ext(const MiniConfig *config) {
    size_t initial_heap = (config && config->initial_heap_bytes > 0) ?
                          config->initial_heap_bytes : (64 * 1024);
    initial_heap = ALIGN8(initial_heap);

    MiniVM *vm = (MiniVM *)calloc(1, sizeof(MiniVM));
    if (!vm) return NULL;

    size_t eden_sz = (config && config->eden_bytes > 0) ? config->eden_bytes : 0;
    if (eden_sz == 0) {
        if (initial_heap <= 16 * 1024) {
            eden_sz = 8 * 1024;
        } else {
            eden_sz = 16 * 1024;
        }
    }
    eden_sz = ALIGN8(eden_sz);

    size_t tenured_sz = initial_heap;
    if (tenured_sz < 16 * 1024) tenured_sz = 16 * 1024;
    tenured_sz = ALIGN8(tenured_sz);

    vm->eden_size = eden_sz;
    vm->tenured_size = tenured_sz;
    vm->heap_size = tenured_sz;
    vm->max_memory_bytes = config ? config->max_heap_bytes : 0;

    /* Largest block first, before small allocations can split it */
    vm->tenured_start = alloc_aligned8(vm->tenured_size, &vm->tenured_raw);
    vm->eden_start = alloc_aligned8(vm->eden_size, &vm->eden_raw);
    if (!vm->eden_start || !vm->tenured_start) {
        free(vm->eden_raw);
        free(vm->tenured_raw);
        free(vm);
        return NULL;
    }

    vm->eden_hp = vm->eden_start;
    vm->eden_limit = vm->eden_start + vm->eden_size;

    vm->tenured_hp = vm->tenured_start;
    vm->tenured_limit = vm->tenured_start + vm->tenured_size;

    /* Legacy compatibility fields */
    vm->from_space_raw = vm->eden_raw;
    vm->to_space_raw   = vm->tenured_raw;
    vm->from_space     = vm->eden_start;
    vm->to_space       = vm->tenured_start;
    vm->hp             = vm->tenured_hp;
    vm->hp_limit       = vm->tenured_limit;

    vm->remset_cap = 64;
    vm->remset = (Value *)mini_word_alloc(vm->remset_cap * sizeof(Value));
    vm->remset_count = 0;

    vm->mark_stack_cap = 512;
    vm->mark_stack = (HeapObj **)mini_word_alloc(vm->mark_stack_cap * sizeof(HeapObj *));
    vm->mark_stack_count = 0;

    mini_set_active_vm(vm);

    vm->stack_cap = (config && config->stack_size > 0) ? config->stack_size : 65536;
    vm->stack = (Value *)mini_word_alloc(vm->stack_cap * sizeof(Value));
    if (!vm->stack) {
        free(vm->eden_raw);
        free(vm->tenured_raw);
        free(vm->remset);
        free(vm->mark_stack);
        free(vm);
        return NULL;
    }
    vm->sp = vm->stack;

    size_t frame_cap = (config && config->frame_capacity > 0) ? config->frame_capacity : 4096;
    vm->call_frames = (CallFrame *)mini_word_alloc(frame_cap * sizeof(CallFrame));
    if (!vm->call_frames) {
        free(vm->stack);
        free(vm->eden_raw);
        free(vm->tenured_raw);
        free(vm->remset);
        free(vm->mark_stack);
        free(vm);
        return NULL;
    }
    vm->call_frame_count = 0;
    vm->frame_cap = frame_cap;

    vm->gcache = (MiniGlobalCacheEntry *)mini_word_alloc(MINI_GCACHE_SIZE * sizeof(MiniGlobalCacheEntry));
    if (!vm->gcache) {
        free(vm->call_frames);
        free(vm->stack);
        free(vm->eden_raw);
        free(vm->tenured_raw);
        free(vm->remset);
        free(vm->mark_stack);
        free(vm);
        return NULL;
    }
    mini_words_fill(vm->gcache, 0, MINI_GCACHE_SIZE * sizeof(MiniGlobalCacheEntry));

    vm->c_roots_cap = 32;
    vm->c_roots = (Value **)malloc(vm->c_roots_cap * sizeof(Value *));
    vm->c_roots_count = 0;

    /* Initialize ROM / Flash XIP context */
    vm->rom_base = NULL;
    vm->rom_size = 0;
    vm->rom_end = NULL;
    vm->rom_cfunc_table = NULL;
    vm->rom_cfunc_count = 0;
    vm->rom_global_env_offset = 0;
    vm->rom_symbols_offset = 0;
    vm->rom_owned_buffer = NULL;

    vm->global_env = mini_nil();
    vm->symbols    = mini_nil();

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
    vm->gensym_counter    = 0;

    vm->global_env = mini_make_cons(vm, mini_nil(), mini_nil());

    /* Register built-in primitives and peripheral / CFASL / Help libraries */
    mini_register_primitives(vm);
    mini_register_gui_lib(vm);
    mini_register_cfasl_lib(vm);
    mini_register_help_lib(vm);

    if (!config || !config->skip_init_fasl) {
        if (init_fasl_data_len > 0) {
            mini_load_fasl_mem(vm, init_fasl_data, init_fasl_data_len);
        }
    }

    vm->baseline_env = mini_cdr(vm->global_env);

    return vm;
}

void mini_set_baseline_env(MiniVM *vm) {
    if (vm) {
        vm->baseline_env = mini_cdr(vm->global_env);
    }
}

MiniVM *mini_create_profile(MiniProfile profile) {
    MiniConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    switch (profile) {
        case MINI_PROFILE_EMBEDDED_200K:
            cfg.initial_heap_bytes = 48 * 1024;
            cfg.max_heap_bytes     = 64 * 1024;
            cfg.eden_bytes         = 16 * 1024;
            cfg.stack_size         = 12288;
            cfg.frame_capacity     = 2048;
            cfg.skip_init_fasl     = true;
            break;
        case MINI_PROFILE_DEFAULT:
        default:
            cfg.initial_heap_bytes = 64 * 1024;
            cfg.max_heap_bytes     = 0;
            cfg.eden_bytes         = 16 * 1024;
            cfg.stack_size         = 65536;
            cfg.frame_capacity     = 4096;
            cfg.skip_init_fasl     = true;
            break;
    }
    return mini_create_ext(&cfg);
}

MiniVM *mini_create(size_t initial_heap_bytes) {
    MiniConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.initial_heap_bytes = initial_heap_bytes > 0 ? initial_heap_bytes : (64 * 1024);
    cfg.max_heap_bytes     = 0;
    cfg.eden_bytes         = 16 * 1024;
    cfg.stack_size         = 65536;
    cfg.frame_capacity     = 4096;
#ifdef MINISCHEME_EMBED_INIT
    cfg.skip_init_fasl     = false;
#else
    cfg.skip_init_fasl     = true;
#endif
    return mini_create_ext(&cfg);
}

void mini_destroy(MiniVM *vm) {
    if (!vm) return;
    if (vm->host_cfuncs) {
        for (size_t i = 0; i < vm->host_cfuncs_count; i++) {
            free(vm->host_cfuncs[i].name);
        }
        free(vm->host_cfuncs);
    }
    if (vm->cfunc_names) {
        for (size_t i = 0; i < vm->cfunc_names_count; i++) {
            free(vm->cfunc_names[i]);
        }
        free(vm->cfunc_names);
    }
    free(vm->eden_raw);
    free(vm->tenured_raw);
    free(vm->remset);
    free(vm->mark_stack);
    free(vm->stack);
    free(vm->call_frames);
    free(vm->gcache);
    free(vm->c_roots);
    if (vm->rom_owned_buffer) {
        free(vm->rom_owned_buffer);
        vm->rom_owned_buffer = NULL;
    }
    if (mini_get_active_vm() == vm) {
        mini_set_active_vm(NULL);
    }
    free(vm);
}

/* ========================================================================= */
/* Evaluation and Execution                                                  */
/* ========================================================================= */

Value mini_continuation_call(MiniVM *vm, Value cont_val, Value arg) {
    if (!mini_is_continuation(cont_val)) {
        vm_error(vm, "mini_continuation_call: target is not a continuation");
        return mini_void();
    }
    ContinuationObj *cont = (ContinuationObj *)UNTAG_PTR(cont_val);
    int base_frame = vm->call_frame_count;
    if (cont->base_frame != (uint32_t)base_frame) {
        vm_error(vm, "cannot invoke continuation across foreign C frame boundary");
        return mini_void();
    }
    if (cont->frame_count == (uint32_t)base_frame) {
        vm->sp = vm->stack + cont->stack_len;
        *vm->sp++ = arg;
        return arg;
    }

    int frame_count = (int)cont->frame_count;
    vm->call_frame_count = frame_count;
    SavedFrame *sf = CONT_FRAMES(cont);
    for (int i = 0; i < frame_count; i++) {
        vm->call_frames[i].closure = sf[i].closure_val ? (ClosureObj *)UNTAG_PTR(sf[i].closure_val) : NULL;
        vm->call_frames[i].code    = sf[i].code_val ? (CodeObj *)UNTAG_PTR(sf[i].code_val) : NULL;
        vm->call_frames[i].pc      = vm->call_frames[i].code ? (CODE_BYTES(vm->call_frames[i].code) + sf[i].pc_offset) : NULL;
        vm->call_frames[i].fp      = vm->stack + sf[i].fp_offset;
        vm->call_frames[i].env     = sf[i].env;
    }

    Value *stk = CONT_STACK(cont);
    if (cont->stack_len > 0) {
        mini_words_copy(vm->stack, stk, cont->stack_len * sizeof(Value));
    }
    vm->sp = vm->stack + cont->stack_len;
    *vm->sp++ = arg;

    vm->continuation_base_frame = (int)cont->base_frame;
    return mini_vm_run(vm, 0, 0, NULL);
}

Value mini_call(MiniVM *vm, Value func, int argc, Value *argv) {
    if (!vm) return mini_void();
    mini_set_active_vm(vm);
    if (!mini_is_closure(func) && !mini_is_cfunc(func) && !mini_is_continuation(func)) {
        vm_error(vm, "mini_call: target is not callable");
        return mini_void();
    }
    if (mini_is_cfunc(func)) {
        /* The caller's argv may be a C array the collector cannot see (apply's
         * flattened list, map's and fold's argument slots): a primitive that
         * allocates and then reads its arguments again (append walks one list
         * after another) would read moved objects. Rooted for the call. */
        CFuncObj *cf = (CFuncObj *)UNTAG_PTR(func);
        for (int i = 0; i < argc; i++) mini_push_root(vm, &argv[i]);
        Value res = cf->fn(vm, argc, argv);
        for (int i = 0; i < argc; i++) mini_pop_root(vm);
        return res;
    }
    if (mini_is_closure(func)) {
        return mini_vm_run(vm, func, argc, argv);
    }
    if (mini_is_continuation(func)) {
        if (argc != 1) {
            vm_error(vm, "continuation: expected 1 argument, got %d", argc);
            return mini_void();
        }
        return mini_continuation_call(vm, func, argv[0]);
    }
    vm_error(vm, "mini_call: target is not callable");
    return mini_void();
}

Value mini_call_named(MiniVM *vm, const char *func_name, int argc, Value *argv) {
    Value func = mini_get_global(vm, func_name);
    if (vm->has_error) return mini_void();
    return mini_call(vm, func, argc, argv);
}

Value mini_eval_string(MiniVM *vm, const char *str) {
    if (!str || !vm) return mini_void();
    mini_set_active_vm(vm);

    Value eval_str_fn = env_lookup_name(vm, "eval-string");
    if (!mini_is_void(eval_str_fn) && !vm->has_error) {
        Value arg = mini_make_string(vm, str);
        mini_push_root(vm, &arg);
        eval_str_fn = env_lookup_name(vm, "eval-string");
        Value res = mini_call(vm, eval_str_fn, 1, &arg);
        mini_pop_root(vm);
        return res;
    }

    Value read_fn = env_lookup_name(vm, "read-from-string");
    Value compile_fn = env_lookup_name(vm, "compile-one");
    Value eval_fn = env_lookup_name(vm, "eval");

    if (!mini_is_void(read_fn) &&
        (!mini_is_void(compile_fn) || !mini_is_void(eval_fn)) &&
        !vm->has_error) {

        Value arg = mini_make_string(vm, str);
        mini_push_root(vm, &arg);
        read_fn = env_lookup_name(vm, "read-from-string");
        Value exprs = mini_call(vm, read_fn, 1, &arg);
        mini_pop_root(vm);
        if (vm->has_error) return mini_void();

        Value res = mini_void();
        mini_push_root(vm, &exprs);
        mini_push_root(vm, &res);

        while (mini_is_pair(exprs)) {
            Value expr = mini_car(exprs);
            mini_push_root(vm, &expr);

            compile_fn = env_lookup_name(vm, "compile-one");
            if (!mini_is_void(compile_fn)) {
                Value clo = mini_call(vm, compile_fn, 1, &expr);
                if (vm->has_error) {
                    mini_pop_root(vm);
                    break;
                }
                res = mini_call(vm, clo, 0, NULL);
                if (vm->has_error) {
                    mini_pop_root(vm);
                    break;
                }
            } else {
                eval_fn = env_lookup_name(vm, "eval");
                if (!mini_is_void(eval_fn)) {
                    res = mini_call(vm, eval_fn, 1, &expr);
                    if (vm->has_error) {
                        mini_pop_root(vm);
                        break;
                    }
                }
            }
            mini_pop_root(vm);
            exprs = mini_cdr(exprs);
        }
        mini_pop_root(vm);
        mini_pop_root(vm);
        return res;
    }
    vm_error(vm, "mini_eval_string: compiler not available; ensure init_fasl is loaded");
    return mini_void();
}

Value mini_eval_file(MiniVM *vm, const char *filepath) {
    if (!filepath) return mini_void();
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        vm_error(vm, "mini_eval_file: cannot open file: %s", filepath);
        return mini_void();
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return mini_void(); }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(fp); return mini_void(); }
    size_t read_bytes = fread(buf, 1, sz, fp);
    buf[read_bytes] = '\0';
    fclose(fp);
    Value res = mini_eval_string(vm, buf);
    free(buf);
    return res;
}

bool mini_load_init(MiniVM *vm, const char *init_path) {
    if (!vm || !init_path) return false;
    mini_clear_error(vm);
    mini_eval_file(vm, init_path);
    return !vm->has_error;
}

bool mini_load_init_string(MiniVM *vm, const char *scm_code) {
    if (!vm || !scm_code) return false;
    mini_clear_error(vm);
    mini_eval_string(vm, scm_code);
    return !vm->has_error;
}

size_t mini_format_string_literal(char *buf, size_t cap, Value str) {
    const char *s = mini_is_string(str) ? mini_string_data(str) : "";
    size_t n = mini_is_string(str) ? mini_string_len(str) : 0, out = 0;
    char esc[8];
#define LIT_PUT(c) do { if (out + 1 < cap) buf[out] = (c); out++; } while (0)
    LIT_PUT('"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *e = NULL;
        if (c == '"') e = "\\\"";
        else if (c == '\\') e = "\\\\";
        else if (c == '\n') e = "\\n";
        else if (c == '\t') e = "\\t";
        else if (c == '\r') e = "\\r";
        else if (c < 0x20 || c == 0x7F) {
            snprintf(esc, sizeof(esc), "\\x%X;", c);
            e = esc;
        }
        if (e) {
            for (; *e; e++) LIT_PUT(*e);
        } else {
            LIT_PUT((char)c);
        }
    }
    LIT_PUT('"');
#undef LIT_PUT
    if (cap > 0) buf[out < cap ? out : cap - 1] = '\0';
    return out;
}

void mini_print_value(MiniVM *vm, Value v, FILE *stream, bool quoted) {
    (void)vm;
    if (mini_is_fixnum(v)) {
        fprintf(stream, "%lld", (long long)mini_to_fixnum(v));
    } else if (mini_is_bigint(v)) {
        char *str = bigint_to_str(v, 10);
        if (str) {
            fprintf(stream, "%s", str);
            free(str);
        }
    } else if (v == MINI_CONST_NIL) {
        fprintf(stream, "()");
    } else if (v == MINI_CONST_TRUE) {
        fprintf(stream, "#t");
    } else if (v == MINI_CONST_FALSE) {
        fprintf(stream, "#f");
    } else if (v == MINI_CONST_VOID) {
        /* void value: silent */
    } else if (v == MINI_CONST_EOF) {
        fprintf(stream, "#<eof>");
    } else if (mini_is_symbol(v)) {
        fprintf(stream, "%s", mini_symbol_name(v));
    } else if (mini_is_string(v)) {
        if (quoted) {
            char small[256];
            size_t need = mini_format_string_literal(small, sizeof(small), v);
            char *lit = need < sizeof(small) ? small : (char *)malloc(need + 1);
            if (lit && lit != small) mini_format_string_literal(lit, need + 1, v);
            fputs(lit ? lit : "\"\"", stream);
            if (lit && lit != small) free(lit);
        } else {
            fprintf(stream, "%s", mini_string_data(v));
        }
    } else if (mini_is_closure(v) || mini_is_cfunc(v)) {
        fprintf(stream, "#<procedure>");
    } else if (mini_is_continuation(v)) {
        fprintf(stream, "#<continuation>");
    } else if (mini_is_flonum(v)) {
        double d = mini_to_flonum(v);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", d);
        if (strchr(buf, '.') == NULL && strchr(buf, 'e') == NULL && strchr(buf, 'n') == NULL) {
            fprintf(stream, "%s.0", buf);
        } else {
            fprintf(stream, "%s", buf);
        }
    } else if (mini_is_vector(v)) {
        fprintf(stream, "#(");
        size_t len = mini_vector_length(v);
        for (size_t i = 0; i < len; i++) {
            if (i > 0) fprintf(stream, " ");
            mini_print_value(vm, mini_vector_ref(vm, v, i), stream, quoted);
        }
        fprintf(stream, ")");
    } else if (mini_is_code(v)) {
        fprintf(stream, "#<bytecode>");
    } else if (mini_is_pair(v)) {
        fprintf(stream, "(");
        Value curr = v;
        bool first = true;
        while (mini_is_pair(curr)) {
            if (!first) fprintf(stream, " ");
            first = false;
            mini_print_value(vm, mini_car(curr), stream, quoted);
            curr = mini_cdr(curr);
        }
        if (!mini_is_nil(curr)) {
            fprintf(stream, " . ");
            mini_print_value(vm, curr, stream, quoted);
        }
        fprintf(stream, ")");
    } else {
        fprintf(stream, "#<unknown 0x%llx>", (unsigned long long)v);
    }
}

/* ------------------------------------------------------------------------- */
/* Access policy (minischeme.h)                                              */
/* ------------------------------------------------------------------------- */

static const MiniAccessPolicy *s_access = NULL;

void mini_set_access_policy(const MiniAccessPolicy *policy) { s_access = policy; }

bool mini_access_is_privileged(void) {
    return !s_access || !s_access->privileged || s_access->privileged();
}

bool mini_access_file(MiniVM *vm, const char *who, const char *path, bool write, char *out, size_t cap) {
    char why[96] = "";
    if (cap == 0) return false;
    if (!s_access || !s_access->file) {
        if (strlen(path) >= cap) {
            vm_error(vm, "%s: path too long", who);
            return false;
        }
        memcpy(out, path, strlen(path) + 1);
        return true;
    }
    if (!s_access->file(path, write, out, cap, why, sizeof(why))) {
        vm_error(vm, "%s: %s", who, why[0] ? why : "not allowed");
        return false;
    }
    return true;
}

bool mini_access_privileged(MiniVM *vm, const char *who) {
    if (mini_access_is_privileged()) return true;
    vm_error(vm, "%s: not allowed for apps (system settings and the REPL only)", who);
    return false;
}
