/**
 * @file minischeme_vm.c
 * @brief MiniScheme Bytecode Virtual Machine Execution Engine.
 *
 * Implements:
 *  - High-performance register/stack hybrid bytecode interpreter.
 *  - GCC/Clang computed-goto direct threading (with ANSI switch fallback).
 *  - Guaranteed O(1) space Tail-Call Optimization (TCO).
 *  - First-class re-entrant continuations (call/cc) with cross-boundary checking.
 *  - Static lexical scoping and mutable upvalue heap environment synchronization.
 */

#include "minischeme_internal.h"

/* Tagged fixnum arithmetic: a and b are fixnums (value << 3, tag 0); true and the tagged
 * result in *r when it is a fixnum too, false on an overflow (the caller then takes the
 * primitive, which makes a bigint) */
#if defined(__GNUC__) || defined(__clang__)
static inline bool fixnum_add_ok(Value a, Value b, intptr_t *r) {
    return !__builtin_add_overflow((intptr_t)a, (intptr_t)b, r);
}
static inline bool fixnum_sub_ok(Value a, Value b, intptr_t *r) {
    return !__builtin_sub_overflow((intptr_t)a, (intptr_t)b, r);
}
static inline bool fixnum_mul_ok(Value a, Value b, intptr_t *r) {
    return !__builtin_mul_overflow((intptr_t)a >> 3, (intptr_t)b, r);
}
#else
static bool fixnum_result(int64_t n, intptr_t *r) {
    if (n < MINI_FIXNUM_MIN || n > MINI_FIXNUM_MAX) return false;
    *r = (intptr_t)mini_make_fixnum(n);
    return true;
}
static bool fixnum_add_ok(Value a, Value b, intptr_t *r) {
    return fixnum_result(mini_to_fixnum(a) + mini_to_fixnum(b), r);
}
static bool fixnum_sub_ok(Value a, Value b, intptr_t *r) {
    return fixnum_result(mini_to_fixnum(a) - mini_to_fixnum(b), r);
}
static bool fixnum_mul_ok(Value a, Value b, intptr_t *r) {
    int64_t x = mini_to_fixnum(a), y = mini_to_fixnum(b);
    /* |x|, |y| <= 2^60: decide on the magnitudes before multiplying */
    uint64_t ux = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    uint64_t uy = y < 0 ? (uint64_t)0 - (uint64_t)y : (uint64_t)y;
    if (ux != 0 && uy > (uint64_t)MINI_FIXNUM_MAX / ux) return false;
    return fixnum_result(x * y, r);
}
#endif

Value prim_callcc(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_void();
}

/* ========================================================================= */
/* Environment Operations                                                    */
/* ========================================================================= */

/* Entry a of a cache set takes entry b's place (field by field: the cache is word memory,
 * IRAM on the ESP32, which takes 32-bit accesses only) */
static inline void gcache_move(MiniGlobalCacheEntry *a, const MiniGlobalCacheEntry *b) {
    a->sym = b->sym;
    a->binding = b->binding;
    a->gen = b->gen;
}

/* Binding pair of a global, or 0. The cache holds MINI_GCACHE_SIZE recent lookups in sets of
 * MINI_GCACHE_WAYS, the most recent first: a hit moves to its set's front, a miss walks the
 * global list and puts what it found there (the set's least recent entry leaves). A
 * direct-mapped cache let two hot globals of one slot evict each other on every access, each
 * time a walk of the whole list (a matrix loop: 1.7 s instead of 0.17 s). Entries of an older
 * gcache_gen (a GC moved the bindings) count as empty. */
static Value global_binding(MiniVM *vm, Value sym) {
    uintptr_t h = (((uintptr_t)sym >> 3) * 2654435761u) >> 8;
    MiniGlobalCacheEntry *set = &vm->gcache[(h & (MINI_GCACHE_SETS - 1)) * MINI_GCACHE_WAYS];
    uint32_t gen = vm->gcache_gen;
    if (set[0].sym == sym && set[0].gen == gen) return set[0].binding;
    for (int w = 1; w < MINI_GCACHE_WAYS; w++) {
        if (set[w].sym == sym && set[w].gen == gen) {
            MiniGlobalCacheEntry hit;
            gcache_move(&hit, &set[w]);
            for (int k = w; k > 0; k--) gcache_move(&set[k], &set[k - 1]);
            gcache_move(&set[0], &hit);
            return hit.binding;
        }
    }
    for (Value curr = mini_cdr(vm->global_env); mini_is_pair(curr); curr = mini_cdr(curr)) {
        Value b = mini_car(curr);
        if (mini_is_pair(b) && mini_car(b) == sym) {
            for (int k = MINI_GCACHE_WAYS - 1; k > 0; k--) gcache_move(&set[k], &set[k - 1]);
            set[0].sym = sym;
            set[0].binding = b;
            set[0].gen = gen;
            return b;
        }
    }
    return 0;
}

Value env_lookup(MiniVM *vm, Value env, Value sym) {
    if (vm && env == vm->global_env) {
        Value b = global_binding(vm, sym);
        return b ? mini_cdr(b) : mini_void();
    }
    Value curr = env;
    while (mini_is_pair(curr)) {
        Value binding = mini_car(curr);
        if (mini_is_pair(binding) && mini_car(binding) == sym) {
            return mini_cdr(binding);
        }
        curr = mini_cdr(curr);
    }
    return mini_void();
}

Value env_lookup_name(MiniVM *vm, const char *name) {
    Value sym = mini_make_symbol(vm, name);
    Value val = env_lookup(vm, vm->global_env, sym);
    if (mini_is_void(val)) {
        val = global_miss(vm, sym);
    }
    return val;
}

const MiniPrimEntry *prim_table_find(const MiniVM *vm, const char *name) {
    if (!vm || !name) return NULL;
    for (int t = vm->prim_table_count - 1; t >= 0; t--) {
        const MiniPrimEntry *tab = vm->prim_tables[t];
        for (size_t i = vm->prim_table_sizes[t]; i-- > 0;) {
            if (strcmp(tab[i].name, name) == 0) return &tab[i];
        }
    }
    return NULL;
}

const char *(*mini_api_alias_hook)(const char *name, size_t i) = NULL;
const char *(*mini_api_unbound_hook)(const char *name) = NULL;

/* A C procedure registered under `name`: a primitive table entry or a host
 * function; its name is static (the table's or the registry's copy) */
static MiniCFunc cfunc_named(MiniVM *vm, const char *name, const char **static_name) {
    const MiniPrimEntry *e = prim_table_find(vm, name);
    if (e) {
        *static_name = e->name;
        return e->fn;
    }
    for (size_t i = 0; i < vm->host_cfuncs_count; i++) {
        if (strcmp(vm->host_cfuncs[i].name, name) == 0) {
            *static_name = vm->host_cfuncs[i].name;
            return vm->host_cfuncs[i].fn;
        }
    }
    return NULL;
}

Value global_miss(MiniVM *vm, Value sym) {
    if (!mini_is_symbol(sym)) return mini_void();
    Value val = mini_void();
    mini_push_root(vm, &sym);
    if (vm->rom_base) {
        val = rom_on_demand_load(vm, sym);
    }
    if (mini_is_void(val)) {
        /* a primitive table entry, or a host function dropped by a FASL
         * snapshot that never bound it */
        const char *name = NULL;
        MiniCFunc fn = cfunc_named(vm, mini_symbol_name(sym), &name);
        /* an API procedure registered under its older (or newer) name */
        const char *other;
        for (size_t k = 0; !fn && mini_api_alias_hook && (other = mini_api_alias_hook(mini_symbol_name(sym), k)) != NULL; k++) {
            fn = cfunc_named(vm, other, &name);
        }
        if (fn) {
            val = make_cfunc(vm, fn, name);
            mini_push_root(vm, &val);
            env_define(vm, vm->global_env, sym, val);
            mini_pop_root(vm);
        }
    }
    mini_pop_root(vm);
    return val;
}

void env_define(MiniVM *vm, Value env, Value sym, Value val) {
    if (env == vm->global_env) {
        Value b = global_binding(vm, sym);
        if (b) {
            mini_set_cdr(b, val);
            mini_write_barrier(vm, b, val);
            return;
        }
    }
    Value curr = (env == vm->global_env) ? mini_nil() : env; /* global miss: just add */
    while (mini_is_pair(curr)) {
        Value binding = mini_car(curr);
        if (mini_is_pair(binding) && mini_car(binding) == sym) {
            mini_set_cdr(binding, val);
            mini_write_barrier(vm, binding, val);
            return;
        }
        curr = mini_cdr(curr);
    }
    *vm->sp++ = sym;
    *vm->sp++ = val;
    Value binding = mini_make_cons(vm, sym, val);
    *vm->sp++ = binding;
    Value new_pair = mini_make_cons(vm, binding, mini_cdr(vm->global_env));
    vm->sp -= 3;
    mini_set_cdr(vm->global_env, new_pair);
    mini_write_barrier(vm, vm->global_env, new_pair);
}

void env_set(MiniVM *vm, Value env, Value sym, Value val) {
    if (env == vm->global_env) {
        Value b = global_binding(vm, sym);
        if (b) {
            mini_set_cdr(b, val);
            mini_write_barrier(vm, b, val);
            return;
        }
    }
    Value curr = (env == vm->global_env) ? mini_nil() : env;
    while (mini_is_pair(curr)) {
        Value binding = mini_car(curr);
        if (mini_is_pair(binding) && mini_car(binding) == sym) {
            mini_set_cdr(binding, val);
            mini_write_barrier(vm, binding, val);
            return;
        }
        curr = mini_cdr(curr);
    }
    mini_push_root(vm, &sym);
    mini_push_root(vm, &val);
    Value loaded = global_miss(vm, sym);
    mini_pop_root(vm);
    mini_pop_root(vm);
    if (!mini_is_void(loaded)) {
        env_set(vm, vm->global_env, sym, val);
        return;
    }
    vm_error(vm, "set!: unbound variable: %s", mini_symbol_name(sym));
}

Value make_continuation(MiniVM *vm, int frame_count, size_t stack_len, int base_frame) {
    return mini_make_continuation(vm, (uint32_t)frame_count, vm->call_frames,
                                  (uint32_t)stack_len, vm->stack, (uint32_t)base_frame);
}

/* ========================================================================= */
/* Virtual Machine Execution Engine                                          */
/* ========================================================================= */

static inline int get_code_num_locals(CodeObj *c) {
    int n = (c->max_stack >= 64) ? (c->max_stack - 64) : c->max_stack;
    return (n < c->num_params) ? c->num_params : n;
}

static inline Value vm_fetch_const(MiniVM *vm, CodeObj *code, const Value *consts, uint16_t idx) {
    Value v = consts[idx];
    if (vm->rom_base && (const char *)code >= vm->rom_base && (const char *)code < vm->rom_end) {
        return rom_materialize_val(vm, v);
    }
    return v;
}

Value mini_vm_run(MiniVM *vm, Value initial_closure, int argc, Value *argv) {
    if (!vm || vm->has_error) return mini_void();
    mini_set_active_vm(vm);
    vm->state_reloaded = false;

    int base_frame;
    CallFrame *frames = vm->call_frames;
    int frame_count;
    Value *sp = vm->sp;
    Value *entry_sp = sp;
    Value *fp;
    CodeObj *code;
    uint8_t *pc;
    Value *consts;
    uint8_t call_argc = 0;
    Value callee = 0;

    if (initial_closure == 0) {
        base_frame = vm->continuation_base_frame;
        frame_count = vm->call_frame_count;
        if (frame_count == 0) return mini_void();
        CallFrame *top_f = &frames[frame_count - 1];
        fp = top_f->fp;
        code = top_f->code;
        pc = top_f->pc;
        consts = CODE_CONSTS(code);
    } else {
        base_frame = vm->call_frame_count;
        if ((size_t)base_frame >= vm->frame_cap) {
            vm_error(vm, "call stack overflow");
            return mini_void();
        }
        frame_count = base_frame;

        *sp++ = initial_closure;
        for (int i = 0; i < argc; i++) {
            *sp++ = argv[i];
        }
        vm->sp = sp;

        ClosureObj *cl = (ClosureObj *)UNTAG_PTR(initial_closure);
        code = (CodeObj *)UNTAG_PTR(cl->code);
        fp = sp - argc;

        frames[frame_count].closure = cl;
        frames[frame_count].code = code;
        frames[frame_count].pc = CODE_BYTES(code);
        frames[frame_count].fp = fp;
        frames[frame_count].env = cl->env;
        frame_count++;
        vm->call_frame_count = frame_count;
        if ((uint32_t)frame_count > vm->stat_frame_peak) vm->stat_frame_peak = (uint32_t)frame_count;

        if (code->is_variadic) {
            int fixed = code->num_params - 1;
            Value rest = mini_nil();
            mini_push_root(vm, &rest);
            for (int i = argc - 1; i >= fixed; i--) {
                vm->sp = sp;
                rest = mini_make_cons(vm, fp[i], rest);
            }
            fp[fixed] = rest;
            mini_pop_root(vm);
            sp = fp + fixed + 1;
            vm->sp = sp;
            code = frames[frame_count - 1].code;
        }

        int needed = get_code_num_locals(code);
        if (fp + needed + 64 >= vm->stack + vm->stack_cap) {
            vm_error(vm, "stack overflow");
            vm->sp = entry_sp;
            vm->call_frame_count = base_frame;
            return mini_void();
        }
        while (sp < fp + needed) *sp++ = MINI_CONST_VOID;
        vm->sp = sp;

        if (code->has_inner_lambdas) {
            PROF_SAVE(prof_prev);
            PROF_SITE("<env-frame>");
            PROF_CODE(code);
            Value fvals = mini_nil();
            mini_push_root(vm, &fvals);
            for (int i = needed - 1; i >= 0; i--) {
                fvals = mini_make_cons(vm, fp[i], fvals);
            }
            Value parent_env = frames[frame_count - 1].closure->env;
            Value new_env = mini_make_cons(vm, fvals, parent_env);
            PROF_SITE(prof_prev);
            mini_pop_root(vm);
            frames[frame_count - 1].env = new_env;
            code = frames[frame_count - 1].code;
        } else {
            frames[frame_count - 1].env = frames[frame_count - 1].closure->env;
        }
        frames[frame_count - 1].pc = CODE_BYTES(code);

        pc = CODE_BYTES(code);
        consts = CODE_CONSTS(code);
    }

#define SYNC_VM_STATE() do { \
    frames[frame_count - 1].pc = pc; \
    vm->sp = sp; \
    vm->call_frame_count = frame_count; \
} while (0)

#define RELOAD_VM_STATE() do { \
    code = frames[frame_count - 1].code; \
    pc = frames[frame_count - 1].pc; \
    consts = CODE_CONSTS(code); \
    sp = vm->sp; \
} while (0)

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    /* Direct threaded code dispatch table (GCC/Clang computed goto) */
    static const void *const dispatch_table[] = {
        [OP_HALT]          = &&do_op_halt,
        [OP_CONST]         = &&do_op_const,
        [OP_INT8]          = &&do_op_int8,
        [OP_NIL]           = &&do_op_nil,
        [OP_TRUE]          = &&do_op_true,
        [OP_FALSE]         = &&do_op_false,
        [OP_VOID]          = &&do_op_void,
        [OP_GET_LOCAL]     = &&do_op_get_local,
        [OP_SET_LOCAL]     = &&do_op_set_local,
        [OP_GET_UPVALUE]   = &&do_op_get_upvalue,
        [OP_SET_UPVALUE]   = &&do_op_set_upvalue,
        [OP_GET_GLOBAL]    = &&do_op_get_global,
        [OP_SET_GLOBAL]    = &&do_op_set_global,
        [OP_DEF_GLOBAL]    = &&do_op_def_global,
        [OP_POP]           = &&do_op_pop,
        [OP_JUMP]          = &&do_op_jump,
        [OP_JUMP_IF_FALSE] = &&do_op_jump_if_false,
        [OP_CLOSURE]       = &&do_op_closure,
        [OP_CALL]          = &&do_op_call,
        [OP_TAIL_CALL]     = &&do_op_tail_call,
        [OP_RETURN]        = &&do_op_return,
        [OP_ADD]           = &&do_op_add,
        [OP_SUB]           = &&do_op_sub,
        [OP_MUL]           = &&do_op_mul,
        [OP_LT]            = &&do_op_lt,
        [OP_GT]            = &&do_op_gt,
        [OP_LE]            = &&do_op_le,
        [OP_GE]            = &&do_op_ge,
        [OP_NUM_EQ]        = &&do_op_num_eq,
        [OP_COUNT ... 255] = &&do_op_bad   /* a byte no instruction has: an error, not a jump anywhere */
    };
    #define DISPATCH() goto *dispatch_table[*pc++]
    DISPATCH();
#else
    #define DISPATCH() goto vm_loop
    DISPATCH();

vm_loop:
    switch (*pc++) {
        case OP_HALT:          goto do_op_halt;
        case OP_CONST:         goto do_op_const;
        case OP_INT8:          goto do_op_int8;
        case OP_NIL:           goto do_op_nil;
        case OP_TRUE:          goto do_op_true;
        case OP_FALSE:         goto do_op_false;
        case OP_VOID:          goto do_op_void;
        case OP_GET_LOCAL:     goto do_op_get_local;
        case OP_SET_LOCAL:     goto do_op_set_local;
        case OP_GET_UPVALUE:   goto do_op_get_upvalue;
        case OP_SET_UPVALUE:   goto do_op_set_upvalue;
        case OP_GET_GLOBAL:    goto do_op_get_global;
        case OP_SET_GLOBAL:    goto do_op_set_global;
        case OP_DEF_GLOBAL:    goto do_op_def_global;
        case OP_POP:           goto do_op_pop;
        case OP_JUMP:          goto do_op_jump;
        case OP_JUMP_IF_FALSE: goto do_op_jump_if_false;
        case OP_CLOSURE:       goto do_op_closure;
        case OP_CALL:          goto do_op_call;
        case OP_TAIL_CALL:     goto do_op_tail_call;
        case OP_RETURN:        goto do_op_return;
        case OP_ADD:           goto do_op_add;
        case OP_SUB:           goto do_op_sub;
        case OP_MUL:           goto do_op_mul;
        case OP_LT:            goto do_op_lt;
        case OP_GT:            goto do_op_gt;
        case OP_LE:            goto do_op_le;
        case OP_GE:            goto do_op_ge;
        case OP_NUM_EQ:        goto do_op_num_eq;
        default:               goto do_op_bad;
    }
#endif
    {

    /* Stop VM execution */
    do_op_halt: {
        goto vm_done;
    }

    do_op_bad: {
        vm_error(vm, "invalid instruction %u (an image for a newer firmware API?)", (unsigned)pc[-1]);
        goto vm_err;
    }

    /* Two-argument arithmetic and comparisons (API 1.14). A fixnum is its value shifted
     * left by 3 with tag 0, so tagged fixnums add, subtract and compare as they are and an
     * overflow of the machine word is exactly a result outside the fixnum range. The slow
     * path calls the primitive on the two stack slots (rooted there), as OP_CALL would. */
#define ARITH_SLOW(prim) do { \
        SYNC_VM_STATE(); \
        Value res_ = prim(vm, 2, sp - 2); \
        if (vm->has_error) goto vm_err; \
        RELOAD_VM_STATE(); \
        sp -= 2; \
        *sp++ = res_; \
        DISPATCH(); \
    } while (0)
#define ARITH_COMPARE(cmp, prim) do { \
        Value a_ = sp[-2], b_ = sp[-1]; \
        if (mini_is_fixnum(a_) && mini_is_fixnum(b_)) { \
            sp--; \
            sp[-1] = ((intptr_t)a_ cmp (intptr_t)b_) ? MINI_CONST_TRUE : MINI_CONST_FALSE; \
            DISPATCH(); \
        } \
        ARITH_SLOW(prim); \
    } while (0)

    do_op_add: {
        Value a = sp[-2], b = sp[-1];
        intptr_t r;
        if (mini_is_fixnum(a) && mini_is_fixnum(b) && fixnum_add_ok(a, b, &r)) {
            sp--;
            sp[-1] = (Value)r;
            DISPATCH();
        }
        ARITH_SLOW(prim_add);
    }
    do_op_sub: {
        Value a = sp[-2], b = sp[-1];
        intptr_t r;
        if (mini_is_fixnum(a) && mini_is_fixnum(b) && fixnum_sub_ok(a, b, &r)) {
            sp--;
            sp[-1] = (Value)r;
            DISPATCH();
        }
        ARITH_SLOW(prim_sub);
    }
    do_op_mul: {
        Value a = sp[-2], b = sp[-1];
        intptr_t r;
        if (mini_is_fixnum(a) && mini_is_fixnum(b) && fixnum_mul_ok(a, b, &r)) {
            sp--;
            sp[-1] = (Value)r;
            DISPATCH();
        }
        ARITH_SLOW(prim_mul);
    }
    do_op_lt:     ARITH_COMPARE(<,  prim_lt);
    do_op_gt:     ARITH_COMPARE(>,  prim_gt);
    do_op_le:     ARITH_COMPARE(<=, prim_le);
    do_op_ge:     ARITH_COMPARE(>=, prim_ge);
    do_op_num_eq: ARITH_COMPARE(==, prim_num_eq);
#undef ARITH_COMPARE
#undef ARITH_SLOW

    /* Load constant from pool: operand is 16-bit little-endian index */
    do_op_const: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        *sp++ = vm_fetch_const(vm, code, consts, idx);
        DISPATCH();
    }

    /* Load 8-bit signed immediate integer (-128 to 127) */
    do_op_int8: {
        *sp++ = mini_make_fixnum((int8_t)*pc++);
        DISPATCH();
    }

    /* Push singleton constants */
    do_op_nil: {
        *sp++ = MINI_CONST_NIL;
        DISPATCH();
    }

    do_op_true: {
        *sp++ = MINI_CONST_TRUE;
        DISPATCH();
    }

    do_op_false: {
        *sp++ = MINI_CONST_FALSE;
        DISPATCH();
    }

    do_op_void: {
        *sp++ = MINI_CONST_VOID;
        DISPATCH();
    }

    do_op_get_local: {
        uint8_t slot = *pc++;
        if (code->has_inner_lambdas) {
            Value node = mini_car(frames[frame_count - 1].env);
            for (uint8_t s = 0; s < slot && mini_is_pair(node); s++) {
                node = mini_cdr(node);
            }
            if (mini_is_pair(node)) {
                *sp++ = mini_car(node);
                DISPATCH();
            }
        }
        *sp++ = fp[slot];
        DISPATCH();
    }

    do_op_set_local: {
        uint8_t slot = *pc++;
        Value val = *(sp - 1);
        fp[slot] = val;
        if (code->has_inner_lambdas) {
            Value node = mini_car(frames[frame_count - 1].env);
            for (uint8_t s = 0; s < slot && mini_is_pair(node); s++) {
                node = mini_cdr(node);
            }
            if (mini_is_pair(node)) {
                mini_set_car(node, val);
                mini_write_barrier(vm, node, val);
            }
        }
        DISPATCH();
    }

    do_op_get_upvalue: {
        uint8_t depth = *pc++;
        uint8_t slot = *pc++;
        Value env_node = frames[frame_count - 1].env;
        if (code->has_inner_lambdas) {
            env_node = mini_cdr(env_node);
        }
        for (uint8_t d = 0; d < depth - 1 && mini_is_pair(env_node); d++) {
            env_node = mini_cdr(env_node);
        }
        Value fvals = mini_car(env_node);
        for (uint8_t s = 0; s < slot && mini_is_pair(fvals); s++) {
            fvals = mini_cdr(fvals);
        }
        *sp++ = mini_car(fvals);
        DISPATCH();
    }

    do_op_set_upvalue: {
        uint8_t depth = *pc++;
        uint8_t slot = *pc++;
        Value val = *(sp - 1);
        Value env_node = frames[frame_count - 1].env;
        if (code->has_inner_lambdas) {
            env_node = mini_cdr(env_node);
        }
        for (uint8_t d = 0; d < depth - 1 && mini_is_pair(env_node); d++) {
            env_node = mini_cdr(env_node);
        }
        Value fvals = mini_car(env_node);
        for (uint8_t s = 0; s < slot && mini_is_pair(fvals); s++) {
            fvals = mini_cdr(fvals);
        }
        mini_set_car(fvals, val);
        mini_write_barrier(vm, fvals, val);
        DISPATCH();
    }

    /* Read global variable from global environment */
    do_op_get_global: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value sym = vm_fetch_const(vm, code, consts, idx);
        Value val = env_lookup(vm, vm->global_env, sym);
        if (mini_is_void(val)) {
            SYNC_VM_STATE();
            val = global_miss(vm, sym);
            RELOAD_VM_STATE();
            if (mini_is_void(val)) {
                const char *hint = mini_api_unbound_hook ? mini_api_unbound_hook(mini_symbol_name(sym)) : NULL;
                vm_error(vm, "unbound variable: %s%s", mini_symbol_name(sym), hint ? hint : "");
                goto vm_err;
            }
        }
        *sp++ = val;
        DISPATCH();
    }

    /* Mutate existing global variable */
    do_op_set_global: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value sym = vm_fetch_const(vm, code, consts, idx);
        SYNC_VM_STATE();
        env_set(vm, vm->global_env, sym, *(sp - 1));
        RELOAD_VM_STATE();
        if (vm->has_error) goto vm_err;
        DISPATCH();
    }

    /* Define new global variable binding */
    do_op_def_global: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value sym = vm_fetch_const(vm, code, consts, idx);
        Value val = *(--sp);
        SYNC_VM_STATE();
        env_define(vm, vm->global_env, sym, val);
        if (vm->has_error) goto vm_err;
        RELOAD_VM_STATE();
        *sp++ = sym;
        DISPATCH();
    }

    /* Pop top operand from stack */
    do_op_pop: {
        --sp;
        DISPATCH();
    }

    /* Unconditional relative jump */
    do_op_jump: {
        int16_t offset = (int16_t)(pc[0] | ((uint16_t)pc[1] << 8));
        pc += 2 + offset;
        DISPATCH();
    }

    /* Conditional jump: branch if condition is #f */
    do_op_jump_if_false: {
        int16_t offset = (int16_t)(pc[0] | ((uint16_t)pc[1] << 8));
        pc += 2;
        Value cond = *(--sp);
        if (cond == MINI_CONST_FALSE) {
            pc += offset;
        }
        DISPATCH();
    }

    /* Instantiate a closure capturing the current lexical environment */
    do_op_closure: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value code_val = vm_fetch_const(vm, code, consts, idx);
        SYNC_VM_STATE();
        PROF_SAVE(prof_prev);
        PROF_SITE("<closure>");
        PROF_CODE(UNTAG_PTR(code_val));
        Value clo = mini_make_closure(vm, code_val, frames[frame_count - 1].env);
        PROF_SITE(prof_prev);
        if (vm->has_error) goto vm_err;
        RELOAD_VM_STATE();
        *sp++ = clo;
        DISPATCH();
    }

    /* General procedure invocation */
    do_op_call: {
        call_argc = *pc++;
        callee = *(sp - call_argc - 1);

    do_call_dispatch:
        if (mini_is_cfunc(callee)) {
            CFuncObj *cf = (CFuncObj *)UNTAG_PTR(callee);
            if (cf->fn == prim_callcc) {
                if (call_argc != 1) {
                    vm_error(vm, "call/cc: expected 1 argument, got %d", (int)call_argc);
                    goto vm_err;
                }
                Value proc = *(sp - 1);
                if (!mini_is_closure(proc) && !mini_is_cfunc(proc) && !mini_is_continuation(proc)) {
                    vm_error(vm, "call/cc: argument must be a procedure");
                    goto vm_err;
                }
                frames[frame_count - 1].pc = pc;
                Value *caller_sp = sp - call_argc - 1;
                size_t stack_len = (size_t)(caller_sp - vm->stack);
                SYNC_VM_STATE();
                Value k = make_continuation(vm, frame_count, stack_len, base_frame);
                if (vm->has_error) goto vm_err;
                RELOAD_VM_STATE();
                caller_sp = vm->stack + stack_len;
                proc = caller_sp[1];
                caller_sp[0] = proc;
                caller_sp[1] = k;
                sp = caller_sp + 2;
                vm->sp = sp;
                callee = proc;
                call_argc = 1;
                goto do_call_dispatch;
            }

            frames[frame_count - 1].pc = pc;
            vm->sp = sp;
            vm->call_frame_count = frame_count;
            PROF_SAVE(prof_prev);
            PROF_SITE(cf->name);
            Value res = cf->fn(vm, call_argc, sp - call_argc);
            PROF_SITE(prof_prev);
            if (vm->has_error) goto vm_err;
            if (vm->state_reloaded) {
                vm->state_reloaded = false;
                vm->sp = vm->stack;
                vm->call_frame_count = 0;
                return res;
            }
            code = frames[frame_count - 1].code;
            pc = frames[frame_count - 1].pc;
            consts = CODE_CONSTS(code);
            sp = vm->sp;
            sp -= (call_argc + 1);
            *sp++ = res;
            DISPATCH();
        }

        if (mini_is_continuation(callee)) {
            goto do_continuation_restore;
        }

        if (mini_is_closure(callee)) {
            if ((size_t)frame_count >= vm->frame_cap) {
                vm_error(vm, "call stack overflow");
                goto vm_err;
            }
            frames[frame_count - 1].pc = pc;

            ClosureObj *callee_cl = (ClosureObj *)UNTAG_PTR(callee);
            CodeObj *callee_code = (CodeObj *)UNTAG_PTR(callee_cl->code);
            Value *new_fp = sp - call_argc;

            frames[frame_count].closure = callee_cl;
            frames[frame_count].code = callee_code;
            frames[frame_count].pc = CODE_BYTES(callee_code);
            frames[frame_count].fp = new_fp;
            frames[frame_count].env = callee_cl->env;
            frame_count++;
            vm->call_frame_count = frame_count;
            if ((uint32_t)frame_count > vm->stat_frame_peak) vm->stat_frame_peak = (uint32_t)frame_count;
            if ((uint32_t)(sp - vm->stack) > vm->stat_stack_peak) vm->stat_stack_peak = (uint32_t)(sp - vm->stack);

            if (callee_code->is_variadic) {
                int fixed = callee_code->num_params - 1;
                vm->sp = sp;
                Value rest = mini_nil();
                mini_push_root(vm, &rest);
                for (int i = call_argc - 1; i >= fixed; i--) {
                    rest = mini_make_cons(vm, new_fp[i], rest);
                }
                new_fp[fixed] = rest;
                mini_pop_root(vm);
                sp = new_fp + fixed + 1;
                vm->sp = sp;
                callee_code = frames[frame_count - 1].code;
            }

            int needed_locals = get_code_num_locals(callee_code);
            if (new_fp + needed_locals + 64 >= vm->stack + vm->stack_cap) {
                vm_error(vm, "stack overflow");
                goto vm_err;
            }
            while (sp < new_fp + needed_locals) *sp++ = MINI_CONST_VOID;
            vm->sp = sp;

            if (callee_code->has_inner_lambdas) {
                PROF_SAVE(prof_prev);
                PROF_SITE("<env-frame>");
                PROF_CODE(callee_code);
                Value fvals = mini_nil();
                mini_push_root(vm, &fvals);
                for (int i = needed_locals - 1; i >= 0; i--) {
                    fvals = mini_make_cons(vm, new_fp[i], fvals);
                }
                Value parent_env = frames[frame_count - 1].closure->env;
                Value new_env = mini_make_cons(vm, fvals, parent_env);
                PROF_SITE(prof_prev);
                mini_pop_root(vm);
                frames[frame_count - 1].env = new_env;
                callee_code = frames[frame_count - 1].code;
            } else {
                frames[frame_count - 1].env = frames[frame_count - 1].closure->env;
            }
            frames[frame_count - 1].pc = CODE_BYTES(callee_code);

            fp = new_fp;
            pc = CODE_BYTES(callee_code);
            code = callee_code;
            consts = CODE_CONSTS(callee_code);

            DISPATCH();
        }

        CodeObj *caller_code = (frame_count > 0) ? frames[frame_count - 1].code : NULL;
        vm_error(vm, "attempt to call non-procedure: tag=%d val=0x%llx (caller code_len=%u pc_off=%td)",
                 (int)(callee & MINI_TAG_MASK), (unsigned long long)callee,
                 caller_code ? caller_code->code_len : 0,
                 caller_code ? (pc - CODE_BYTES(caller_code)) : 0);
        goto vm_err;
    }

    do_op_tail_call: {
        call_argc = *pc++;
        callee = *(sp - call_argc - 1);

    do_tail_call_dispatch:
        if (mini_is_cfunc(callee)) {
            CFuncObj *cf = (CFuncObj *)UNTAG_PTR(callee);
            if (cf->fn == prim_callcc) {
                if (call_argc != 1) {
                    vm_error(vm, "call/cc: expected 1 argument, got %d", (int)call_argc);
                    goto vm_err;
                }
                Value proc = *(sp - 1);
                if (!mini_is_closure(proc) && !mini_is_cfunc(proc) && !mini_is_continuation(proc)) {
                    vm_error(vm, "call/cc: argument must be a procedure");
                    goto vm_err;
                }
                int cont_frames = frame_count - 1;
                size_t stack_len = (size_t)((fp - 1) - vm->stack);
                size_t fp_offset = (size_t)(fp - vm->stack);
                size_t proc_offset = (size_t)((sp - 1) - vm->stack);
                SYNC_VM_STATE();
                Value k = make_continuation(vm, cont_frames, stack_len, base_frame);
                if (vm->has_error) goto vm_err;
                RELOAD_VM_STATE();
                fp = vm->stack + fp_offset;
                proc = vm->stack[proc_offset];
                fp[0] = k;
                sp = fp + 1;
                vm->sp = sp;
                callee = proc;
                call_argc = 1;
                goto do_tail_call_dispatch;
            }

            frames[frame_count - 1].pc = pc;
            vm->sp = sp;
            vm->call_frame_count = frame_count;
            PROF_SAVE(prof_prev);
            PROF_SITE(cf->name);
            Value res = cf->fn(vm, call_argc, sp - call_argc);
            PROF_SITE(prof_prev);
            if (vm->has_error) goto vm_err;
            if (vm->state_reloaded) {
                vm->state_reloaded = false;
                vm->sp = vm->stack;
                vm->call_frame_count = 0;
                return res;
            }

            frame_count--;
            vm->call_frame_count = frame_count;
            if (frame_count == base_frame) {
                vm->sp = fp - 1;
                return res;
            }

            CallFrame *parent_f = &frames[frame_count - 1];
            sp = fp - 1;
            *sp++ = res;
            fp = parent_f->fp;
            code = parent_f->code;
            pc = parent_f->pc;
            consts = CODE_CONSTS(code);
            DISPATCH();
        }

        if (mini_is_continuation(callee)) {
            goto do_continuation_restore;
        }

        if (mini_is_closure(callee)) {
            ClosureObj *callee_cl = (ClosureObj *)UNTAG_PTR(callee);
            CodeObj *callee_code = (CodeObj *)UNTAG_PTR(callee_cl->code);
            Value *args_src = sp - call_argc;

            for (int i = 0; i < call_argc; i++) {
                fp[i] = args_src[i];
            }
            sp = fp + call_argc;

            frames[frame_count - 1].closure = callee_cl;
            frames[frame_count - 1].code = callee_code;
            frames[frame_count - 1].pc = CODE_BYTES(callee_code);
            frames[frame_count - 1].fp = fp;
            frames[frame_count - 1].env = callee_cl->env;
            vm->call_frame_count = frame_count;

            if (callee_code->is_variadic) {
                int fixed = callee_code->num_params - 1;
                vm->sp = sp;
                Value rest = mini_nil();
                mini_push_root(vm, &rest);
                for (int i = call_argc - 1; i >= fixed; i--) {
                    rest = mini_make_cons(vm, fp[i], rest);
                }
                fp[fixed] = rest;
                mini_pop_root(vm);
                sp = fp + fixed + 1;
                vm->sp = sp;
                callee_code = frames[frame_count - 1].code;
            }

            int needed_locals = get_code_num_locals(callee_code);
            if (fp + needed_locals + 64 >= vm->stack + vm->stack_cap) {
                vm_error(vm, "stack overflow");
                goto vm_err;
            }
            while (sp < fp + needed_locals) *sp++ = MINI_CONST_VOID;
            vm->sp = sp;

            if (callee_code->has_inner_lambdas) {
                PROF_SAVE(prof_prev);
                PROF_SITE("<env-frame>");
                PROF_CODE(callee_code);
                Value fvals = mini_nil();
                mini_push_root(vm, &fvals);
                for (int i = needed_locals - 1; i >= 0; i--) {
                    fvals = mini_make_cons(vm, fp[i], fvals);
                }
                Value parent_env = frames[frame_count - 1].closure->env;
                Value new_env = mini_make_cons(vm, fvals, parent_env);
                PROF_SITE(prof_prev);
                mini_pop_root(vm);
                frames[frame_count - 1].env = new_env;
                callee_code = frames[frame_count - 1].code;
            } else {
                frames[frame_count - 1].env = frames[frame_count - 1].closure->env;
            }
            frames[frame_count - 1].pc = CODE_BYTES(callee_code);

            pc = CODE_BYTES(callee_code);
            code = callee_code;
            consts = CODE_CONSTS(callee_code);

            DISPATCH();
        }

        CodeObj *caller_code = (frame_count > 0) ? frames[frame_count - 1].code : NULL;
        vm_error(vm, "attempt to tail-call non-procedure: tag=%d val=0x%llx (caller code_len=%u pc_off=%td)",
                 (int)(callee & MINI_TAG_MASK), (unsigned long long)callee,
                 caller_code ? caller_code->code_len : 0,
                 caller_code ? (pc - CODE_BYTES(caller_code)) : 0);
        goto vm_err;
    }

    do_op_return: {
        Value ret_val = *(--sp);
        frame_count--;
        vm->call_frame_count = frame_count;
        if (frame_count == base_frame) {
            vm->sp = fp - 1;
            return ret_val;
        }
        CallFrame *parent_f = &frames[frame_count - 1];
        sp = fp - 1;
        *sp++ = ret_val;
        fp = parent_f->fp;
        pc = parent_f->pc;
        code = parent_f->code;
        consts = CODE_CONSTS(code);
        DISPATCH();
    }

    do_continuation_restore: {
        if (call_argc != 1) {
            vm_error(vm, "continuation: expected 1 argument, got %d", (int)call_argc);
            goto vm_err;
        }
        Value ret_val = *(sp - 1);
        ContinuationObj *cont = (ContinuationObj *)UNTAG_PTR(callee);
        if (cont->base_frame != (uint32_t)base_frame) {
            vm_error(vm, "cannot invoke continuation across foreign C frame boundary");
            goto vm_err;
        }
        if (cont->frame_count == (uint32_t)base_frame) {
            vm->sp = vm->stack + cont->stack_len;
            *vm->sp++ = ret_val;
            vm->call_frame_count = base_frame;
            return ret_val;
        }

        frame_count = (int)cont->frame_count;
        vm->call_frame_count = frame_count;
        SavedFrame *sf = CONT_FRAMES(cont);
        for (int i = 0; i < frame_count; i++) {
            frames[i].closure = sf[i].closure_val ? (ClosureObj *)UNTAG_PTR(sf[i].closure_val) : NULL;
            frames[i].code    = sf[i].code_val ? (CodeObj *)UNTAG_PTR(sf[i].code_val) : NULL;
            frames[i].pc      = frames[i].code ? (CODE_BYTES(frames[i].code) + sf[i].pc_offset) : NULL;
            frames[i].fp      = vm->stack + sf[i].fp_offset;
            frames[i].env     = sf[i].env;
        }

        Value *stk = CONT_STACK(cont);
        if (cont->stack_len > 0) {
            mini_words_copy(vm->stack, stk, cont->stack_len * sizeof(Value));
        }
        sp = vm->stack + cont->stack_len;
        *sp++ = ret_val;
        vm->sp = sp;

        CallFrame *top_f = &frames[frame_count - 1];
        fp = top_f->fp;
        pc = top_f->pc;
        code = top_f->code;
        consts = CODE_CONSTS(code);
        DISPATCH();
    }

    }

vm_done:
    vm->sp = entry_sp;
    vm->call_frame_count = base_frame;
    return mini_void();

vm_err:
    vm->sp = entry_sp;
    vm->call_frame_count = base_frame;
    return mini_void();
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

Value mini_execute(MiniVM *vm, ClosureObj *entry_closure, int argc, Value *argv) {
    Value clo_val = ((uintptr_t)entry_closure) | MINI_TAG_CLOSURE;
    return mini_vm_run(vm, clo_val, argc, argv);
}

/* ========================================================================= */
/* Standalone Launcher & Universal CLI Dispatcher                            */
/* ========================================================================= */

#ifndef MINISCHEME_NO_MAIN
#include "minischeme_lib.h"

#ifdef MINISCHEME_EMBED_INIT
#include "init_fasl.h"
#endif

#ifdef MINISCHEME_EMBED_COMPILE
#include "compile_fasl.h"
#endif

#if !defined(MINISCHEME_EMBED_INIT) && !defined(MINISCHEME_EMBED_COMPILE)
static Value parse_cli_value(MiniVM *vm, const char *str) {
    if (!str) return mini_nil();
    if (strcmp(str, "#t") == 0) return mini_true();
    if (strcmp(str, "#f") == 0) return mini_false();
    char *endptr = NULL;
    long long v = strtoll(str, &endptr, 10);
    if (endptr && *endptr == '\0' && endptr != str) return mini_make_fixnum(v);
    double d = strtod(str, &endptr);
    if (endptr && *endptr == '\0' && endptr != str && (strchr(str, '.') || strchr(str, 'e') || strchr(str, 'E'))) {
        return mini_make_flonum(vm, d);
    }
    return mini_make_string(vm, str);
}

static void print_vm_usage(const char *prog) {
    printf("MiniScheme VM - Standalone Bytecode Virtual Machine\n");
    printf("Usage: %s [options] <file.fasl> [args...]\n", prog);
    printf("       %s [options] -l <lib.fasl> [<func> [args...] | -e <expr> | <app.fasl> [args...]]\n\n", prog);
    printf("Options:\n");
    printf("  -l <file.fasl>   Load FASL image as library without calling (main)\n");
    printf("  -c, --call <fn>  Call Scheme function <fn> with following arguments\n");
    printf("  -e <expr>        Evaluate Scheme expression\n");
    printf("  -m <bytes>       Set maximum heap memory limit\n");
    printf("  -h, --help       Show this help message\n");
    printf("  -v, --version    Show version information\n");
}
#endif

int main(int argc, char **argv) {
    size_t mem_limit = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mem_limit = (size_t)strtoull(argv[i + 1], NULL, 10);
            for (int j = i; j + 2 <= argc; j++) argv[j] = argv[j + 2];
            argc -= 2;
            i--;
        }
    }

    MiniVM *vm = mini_create(0);
    if (!vm) {
        fprintf(stderr, "Error: failed to initialize MiniScheme VM.\n");
        return 1;
    }
    if (mem_limit > 0) mini_set_memory_limit(vm, mem_limit);
    mini_register_hardware_lib(vm);

#ifdef MINISCHEME_EMBED_COMPILE
    if (!mini_load_fasl_mem(vm, compile_fasl_data, compile_fasl_data_len)) {
        fprintf(stderr, "MiniScheme Fatal: failed to load embedded compiler FASL: %s\n",
                mini_get_error(vm) ? mini_get_error(vm) : "unknown error");
        mini_destroy(vm);
        return 1;
    }
#endif

    int exit_code = 0;

#if defined(MINISCHEME_EMBED_INIT) || defined(MINISCHEME_EMBED_COMPILE)
    Value main_fn = mini_get_global(vm, "main");
    if (mini_is_closure(main_fn) || mini_is_cfunc(main_fn)) {
        Value arg_list = mini_nil();
        mini_push_root(vm, &arg_list);
        for (int k = argc - 1; k >= 1; k--) {
            Value s = mini_make_string(vm, argv[k]);
            arg_list = mini_make_cons(vm, s, arg_list);
        }
        Value res = mini_call(vm, main_fn, 1, &arg_list);
        mini_pop_root(vm);

        if (mini_get_error(vm)) {
            fprintf(stderr, "MiniScheme Error: %s\n", mini_get_error(vm));
            exit_code = 1;
        } else if (mini_is_fixnum(res)) {
            exit_code = (int)mini_to_fixnum(res);
        }
    }
#else
    if (argc < 2) {
        print_vm_usage(argv[0]);
        mini_destroy(vm);
        return 0;
    }

    bool call_main = false;
    const char *main_fasl = NULL;
    const char *call_func = NULL;
    const char *eval_expr = NULL;
    int app_arg_start = argc;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            const char *lib_path = argv[i + 1];
            if (!mini_load_fasl(vm, lib_path)) {
                fprintf(stderr, "MiniScheme VM Error: failed to load library '%s': %s\n",
                        lib_path, mini_get_error(vm) ? mini_get_error(vm) : "unknown error");
                mini_destroy(vm);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            eval_expr = argv[i + 1];
            i += 2;
            break;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--call") == 0) && i + 1 < argc) {
            call_func = argv[i + 1];
            app_arg_start = i + 2;
            break;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_vm_usage(argv[0]); mini_destroy(vm); return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("MiniScheme VM 1.0 (Standalone Bytecode Virtual Machine)\n"); mini_destroy(vm); return 0;
        } else if (strcmp(argv[i], "--") == 0) {
            i++; break;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "MiniScheme VM: unknown option '%s'\n", argv[i]);
            print_vm_usage(argv[0]); mini_destroy(vm); return 1;
        } else {
            size_t len = strlen(argv[i]);
            bool is_fasl = (len >= 5 && strcmp(argv[i] + len - 5, ".fasl") == 0);
            if (is_fasl) {
                main_fasl = argv[i];
                call_main = true;
                app_arg_start = i + 1;
            } else {
                FILE *tf = fopen(argv[i], "rb");
                if (tf) {
                    fclose(tf);
                    main_fasl = argv[i];
                    call_main = true;
                    app_arg_start = i + 1;
                } else {
                    call_func = argv[i];
                    app_arg_start = i + 1;
                }
            }
            break;
        }
    }

    if (main_fasl) {
        if (!mini_load_fasl(vm, main_fasl)) {
            fprintf(stderr, "MiniScheme VM Error: failed to load '%s': %s\n",
                    main_fasl, mini_get_error(vm) ? mini_get_error(vm) : "unknown error");
            mini_destroy(vm);
            return 1;
        }
    }

    if (call_main) {
        Value main_fn = mini_get_global(vm, "main");
        if (mini_is_closure(main_fn) || mini_is_cfunc(main_fn)) {
            Value arg_list = mini_nil();
            mini_push_root(vm, &arg_list);
            for (int k = argc - 1; k >= app_arg_start; k--) {
                Value s = mini_make_string(vm, argv[k]);
                arg_list = mini_make_cons(vm, s, arg_list);
            }
            Value res = mini_call(vm, main_fn, 1, &arg_list);
            mini_pop_root(vm);

            if (mini_get_error(vm)) {
                fprintf(stderr, "MiniScheme Error: %s\n", mini_get_error(vm));
                exit_code = 1;
            } else if (mini_is_fixnum(res)) {
                exit_code = (int)mini_to_fixnum(res);
            }
        }
    } else if (call_func) {
        Value fn = mini_get_global(vm, call_func);
        if (mini_is_void(fn)) {
            fprintf(stderr, "MiniScheme Error: undefined function '%s'\n", call_func);
            mini_destroy(vm);
            return 1;
        }
        if (!mini_is_closure(fn) && !mini_is_cfunc(fn)) {
            fprintf(stderr, "MiniScheme Error: '%s' is not a procedure\n", call_func);
            mini_destroy(vm);
            return 1;
        }

        int remaining_argc = argc - app_arg_start;
        int num_params = 0;
        bool is_variadic = false;
        if (mini_is_closure(fn)) {
            ClosureObj *cl = (ClosureObj *)UNTAG_PTR(fn);
            CodeObj *co = (CodeObj *)UNTAG_PTR(cl->code);
            num_params = (int)co->num_params;
            is_variadic = co->is_variadic != 0;
        }

        Value res = mini_void();
        if (num_params == 1 && !is_variadic && remaining_argc != 1) {
            Value arg_list = mini_nil();
            mini_push_root(vm, &arg_list);
            for (int k = argc - 1; k >= app_arg_start; k--) {
                Value val = parse_cli_value(vm, argv[k]);
                arg_list = mini_make_cons(vm, val, arg_list);
            }
            res = mini_call(vm, fn, 1, &arg_list);
            mini_pop_root(vm);
        } else {
            Value *call_args = NULL;
            if (remaining_argc > 0) {
                call_args = (Value *)malloc(remaining_argc * sizeof(Value));
                for (int k = 0; k < remaining_argc; k++) {
                    call_args[k] = parse_cli_value(vm, argv[app_arg_start + k]);
                    mini_push_root(vm, &call_args[k]);
                }
            }
            res = mini_call(vm, fn, remaining_argc, call_args);
            if (remaining_argc > 0) {
                for (int k = 0; k < remaining_argc; k++) {
                    mini_pop_root(vm);
                }
                free(call_args);
            }
        }

        if (mini_get_error(vm)) {
            fprintf(stderr, "MiniScheme Error: %s\n", mini_get_error(vm));
            exit_code = 1;
        } else if (!mini_is_void(res)) {
            mini_print_value(vm, res, stdout, true);
            putchar('\n');
            if (strcmp(call_func, "main") == 0 && mini_is_fixnum(res)) {
                exit_code = (int)mini_to_fixnum(res);
            }
        }
    } else if (eval_expr) {
        Value res = mini_eval_string(vm, eval_expr);
        if (mini_get_error(vm)) {
            fprintf(stderr, "MiniScheme Error: %s\n", mini_get_error(vm));
            exit_code = 1;
        } else if (!mini_is_void(res)) {
            mini_print_value(vm, res, stdout, true);
            putchar('\n');
        }
    }
#endif

    mini_console_history_free();
    mini_destroy(vm);
    return exit_code;
}
#endif /* MINISCHEME_NO_MAIN */
