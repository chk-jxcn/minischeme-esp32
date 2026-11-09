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
#include "minischeme_api_ids.h"

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

void mini_globals_reset(MiniVM *vm) {
    vm->gcache_gen++;
    if (vm->gslots) mini_words_fill(vm->gslots, 0, vm->gslot_count * sizeof(Value));
    if (vm->prim_fns) memset(vm->prim_fns, 0, MINI_API_ID_COUNT * sizeof(MiniCFunc));
}

Value prim_api_id(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1 || !mini_is_symbol(argv[0])) return mini_false();
    const char *name = mini_symbol_name(argv[0]);
    size_t lo = 0, hi = MINI_API_ID_INDEX_COUNT;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(mini_api_id_index[mid].name, name);
        if (c == 0) return mini_make_fixnum(mini_api_id_index[mid].id);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return mini_false();
}

/* An API number whose name is bound to something else than a C procedure (a Scheme definition,
 * call/cc) or to nothing: OP_CALL_PRIM calls the global the ordinary way. Never called. */
static Value prim_by_call(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return MINI_CONST_VOID;
}

/* An existing symbol of that name, without making one (no allocation) */
static Value find_symbol(MiniVM *vm, const char *name) {
    size_t len = strlen(name);
    if (vm->xrom_sym_count) {
        Value r = xrom_find_symbol(vm, name, len);
        if (r) return r;
    }
    for (Value c = vm->symbols; mini_is_pair(c); c = mini_cdr(c)) {
        Value s = mini_car(c);
        if (mini_is_symbol(s)) {
            const SymbolObj *o = (const SymbolObj *)UNTAG_PTR(s);
            if (o->len == len && memcmp(o->name, name, len) == 0) return s;
        }
    }
    return 0;
}

static MiniCFunc cfunc_named(MiniVM *vm, const char *name, const char **static_name);
static Value global_binding(MiniVM *vm, Value sym);

/* The k-th name of API number id: 0 its name, then its older names (aliases), NULL after them */
static const char *api_id_name(uint16_t id, size_t k) {
    if (k == 0) return mini_api_id_names[id];
    for (size_t i = 0; i < MINI_API_ID_INDEX_COUNT; i++) {
        if (mini_api_id_index[i].id == id && strcmp(mini_api_id_index[i].name, mini_api_id_names[id]) != 0 && --k == 0) {
            return mini_api_id_index[i].name;
        }
    }
    return NULL;
}

/* The procedure of API number id, found once per VM: the C function its global is bound to,
 * else (unbound) the firmware's C procedure of that name (no binding, no procedure object:
 * no memory); prim_by_call for anything else. NULL with an error for a number this firmware
 * does not know. */
static MiniCFunc prim_resolve(MiniVM *vm, uint16_t id) {
    if (id >= MINI_API_ID_COUNT || !mini_api_id_names[id]) {
        vm_error(vm, "unknown API procedure number %u (an image for a newer firmware API?)", (unsigned)id);
        return NULL;
    }
    if (!vm->prim_fns) {
        vm->prim_fns = (MiniCFunc *)calloc(MINI_API_ID_COUNT, sizeof(MiniCFunc));
        if (!vm->prim_fns) {
            vm_error(vm, "out of memory (API procedure table)");
            return NULL;
        }
    }
    /* a global bound under any of its names decides; else a C procedure of any of them */
    MiniCFunc fn = NULL;
    const char *name;
    for (size_t k = 0; !fn && (name = api_id_name(id, k)) != NULL; k++) {
        Value sym = find_symbol(vm, name);
        Value b = sym ? global_binding(vm, sym) : 0;
        if (b) fn = mini_is_cfunc(mini_cdr(b)) ? ((CFuncObj *)UNTAG_PTR(mini_cdr(b)))->fn : prim_by_call;
    }
    for (size_t k = 0; !fn && (name = api_id_name(id, k)) != NULL; k++) {
        const char *static_name = NULL, *other;
        fn = cfunc_named(vm, name, &static_name);
        for (size_t j = 0; !fn && mini_api_alias_hook && (other = mini_api_alias_hook(name, j)) != NULL; j++) {
            fn = cfunc_named(vm, other, &static_name);
        }
    }
    if (!fn) fn = prim_by_call;   /* the ordinary call says what is missing */
    if (fn == prim_callcc) fn = prim_by_call;
    vm->prim_fns[id] = fn;
    return fn;
}

/* The global slot of a Code ROM symbol (its header holds 1 + its number in the image), or NULL
 * for any other symbol (made at run time, or a FASL image's), which takes the global cache */
static inline Value *global_slot(MiniVM *vm, Value sym) {
    const char *p = (const char *)UNTAG_PTR(sym);
    if (!vm->gslots || p < vm->xrom_start || p >= vm->xrom_end) return NULL;
    uint32_t k = ((const HeapObj *)(const void *)p)->type >> MINI_SYM_SLOT_SHIFT;
    return (k && k <= vm->gslot_count) ? &vm->gslots[k - 1] : NULL;
}

/* The binding pair of sym in the global list, or 0 */
static Value global_walk(MiniVM *vm, Value sym) {
    for (Value curr = mini_cdr(vm->global_env); mini_is_pair(curr); curr = mini_cdr(curr)) {
        Value b = mini_car(curr);
        if (mini_is_pair(b) && mini_car(b) == sym) return b;
    }
    return 0;
}

/* Binding pair of a global, or 0. A Code ROM symbol (an app's names) has a slot of its own:
 * one load once looked up. Other symbols use the cache: it holds MINI_GCACHE_SIZE recent lookups in sets of
 * MINI_GCACHE_WAYS, the most recent first: a hit moves to its set's front, a miss walks the
 * global list and puts what it found there (the set's least recent entry leaves). A
 * direct-mapped cache let two hot globals of one slot evict each other on every access, each
 * time a walk of the whole list (a matrix loop: 1.7 s instead of 0.17 s). Entries of an older
 * gcache_gen (a GC moved the bindings) count as empty. */
static Value global_binding(MiniVM *vm, Value sym) {
    Value *slot = global_slot(vm, sym);
    if (slot) {
        if (!*slot) *slot = global_walk(vm, sym);
        return *slot;
    }
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
    Value b = global_walk(vm, sym);
    if (b) {
        for (int k = MINI_GCACHE_WAYS - 1; k > 0; k--) gcache_move(&set[k], &set[k - 1]);
        set[0].sym = sym;
        set[0].binding = b;
        set[0].gen = gen;
    }
    return b;
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
    Value *slot = global_slot(vm, mini_car(mini_car(new_pair)));   /* the binding may have moved */
    if (slot) *slot = mini_car(new_pair);
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

static MINI_NOINLINE Value vm_fetch_const_rom(MiniVM *vm, CodeObj *code, uint16_t idx) {
    return rom_materialize_val(vm, CODE_CONSTS(code)[idx]);
}

static inline Value vm_fetch_const(MiniVM *vm, CodeObj *code, uint16_t idx) {
    Value v = CODE_CONSTS(code)[idx];
    if (MINI_UNLIKELY(vm->rom_base != NULL &&
                      (const char *)code >= vm->rom_base &&
                      (const char *)code < vm->rom_end)) {
        return vm_fetch_const_rom(vm, code, idx);
    }
    return v;
}

static MINI_NOINLINE Value vm_get_heap_local(Value env, uint8_t slot) {
    Value node = mini_car(env);
    for (uint8_t s = 0; s < slot && mini_is_pair(node); s++) {
        node = mini_cdr(node);
    }
    return mini_is_pair(node) ? mini_car(node) : MINI_CONST_VOID;
}

static MINI_NOINLINE void vm_set_heap_local(MiniVM *vm, Value env, uint8_t slot, Value val) {
    Value node = mini_car(env);
    for (uint8_t s = 0; s < slot && mini_is_pair(node); s++) {
        node = mini_cdr(node);
    }
    if (mini_is_pair(node)) {
        mini_set_car(node, val);
        mini_write_barrier(vm, node, val);
    }
}

static MINI_NOINLINE CodeObj *vm_pack_variadic(MiniVM *vm, int frame_idx, int call_argc, int fixed) {
    CallFrame *frames = vm->call_frames;
    size_t fp_off = (size_t)(frames[frame_idx].fp - vm->stack);
    Value rest = mini_nil();
    mini_push_root(vm, &rest);
    for (int i = call_argc - 1; i >= fixed; i--) {
        rest = mini_make_cons(vm, vm->stack[fp_off + (size_t)i], rest);
    }
    mini_pop_root(vm);
    Value *fp = vm->stack + fp_off;
    fp[fixed] = rest;
    vm->sp = fp + fixed + 1;
    return frames[frame_idx].code;
}

static MINI_NOINLINE CodeObj *vm_alloc_env_frame(MiniVM *vm, int frame_idx, int needed_locals) {
    CallFrame *frames = vm->call_frames;
    size_t fp_off = (size_t)(frames[frame_idx].fp - vm->stack);
    PROF_SAVE(prof_prev);
    PROF_SITE("<env-frame>");
    PROF_CODE(frames[frame_idx].code);
    Value fvals = mini_nil();
    mini_push_root(vm, &fvals);
    for (int i = needed_locals - 1; i >= 0; i--) {
        fvals = mini_make_cons(vm, vm->stack[fp_off + (size_t)i], fvals);
    }
    Value parent_env = frames[frame_idx].closure->env;
    Value new_env = mini_make_cons(vm, fvals, parent_env);
    PROF_SITE(prof_prev);
    mini_pop_root(vm);
    frames[frame_idx].env = new_env;
    return frames[frame_idx].code;
}

static MINI_NOINLINE Value prim_cons_slow(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    return mini_make_cons(vm, argv[0], argv[1]);
}

static MINI_NOINLINE Value vm_box_flonum_slow(MiniVM *vm, double d) {
    return mini_make_flonum(vm, d);
}

static inline bool vm_num_to_double_fast(Value v, double *out) {
    if (mini_is_fixnum(v)) {
        *out = (double)mini_to_fixnum(v);
        return true;
    }
#ifdef MINI_HAS_IMM_FLONUM
    if (MINI_LIKELY(mini_is_imm_flonum(v))) {
        *out = mini_imm_flonum_to_double(v);
        return true;
    }
#endif
    if (mini_is_record(v) && (((const MiniHeapObj *)UNTAG_PTR(v))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_FLONUM) {
        *out = ((const FlonumObj *)UNTAG_PTR(v))->val;
        return true;
    }
    return false;
}

/* True ONLY when at least one operand is an inexact flonum and the other is fixnum or flonum.
 * Prevents exact fixnum overflow (both fixnums) from turning into an inexact flonum instead of BigInt! */
static inline bool vm_flonum_pair_to_double(Value a, Value b, double *da, double *db) {
    if (MINI_UNLIKELY(((a | b) & MINI_TAG_MASK) == 0)) return false;
    return vm_num_to_double_fast(a, da) && vm_num_to_double_fast(b, db);
}

/* Fast flonum result: immediate flonum on 64-bit, or Eden bump-pointer on 32-bit;
 * returns 0 ONLY if heap allocation is needed and Eden is full (caller then takes slow path). */
static inline Value vm_try_fast_flonum(MiniVM *vm, double d) {
#ifdef MINI_HAS_IMM_FLONUM
    Value imm;
    if (MINI_LIKELY(mini_try_imm_flonum(d, &imm))) return imm;
#endif
    size_t aligned_sz = ALIGN8(sizeof(FlonumObj));
    if (MINI_LIKELY(vm->eden_hp + aligned_sz <= vm->eden_limit)) {
        FlonumObj *f = (FlonumObj *)vm->eden_hp;
        vm->eden_hp += aligned_sz;
        vm->stat_alloc_bytes += aligned_sz;
#ifdef MINI_ALLOC_PROFILE
        mini_prof_alloc(aligned_sz, OBJ_FLONUM);
#endif
        f->header.type = OBJ_FLONUM;
        f->header.size = (uint32_t)aligned_sz;
        f->val = d;
        return ((uintptr_t)f) | MINI_TAG_RECORD;
    }
    return 0; /* tag 0 with 0 is fixnum 0, so use bool out instead! */
}

static inline bool vm_try_make_flonum_inline(MiniVM *vm, double d, Value *out) {
#ifdef MINI_HAS_IMM_FLONUM
    if (MINI_LIKELY(mini_try_imm_flonum(d, out))) return true;
#endif
    size_t aligned_sz = ALIGN8(sizeof(FlonumObj));
    if (MINI_LIKELY(vm->eden_hp + aligned_sz <= vm->eden_limit)) {
        FlonumObj *f = (FlonumObj *)vm->eden_hp;
        vm->eden_hp += aligned_sz;
        vm->stat_alloc_bytes += aligned_sz;
#ifdef MINI_ALLOC_PROFILE
        mini_prof_alloc(aligned_sz, OBJ_FLONUM);
#endif
        f->header.type = OBJ_FLONUM;
        f->header.size = (uint32_t)aligned_sz;
        f->val = d;
        *out = ((uintptr_t)f) | MINI_TAG_RECORD;
        return true;
    }
    return false;
}

static MINI_NOINLINE Value vm_prim_by_call_lookup(MiniVM *vm, uint16_t id) {
    Value val = MINI_CONST_VOID;
    const char *name;
    for (size_t k = 0; mini_is_void(val) && !vm->has_error && (name = api_id_name(id, k)) != NULL; k++) {
        Value sym = find_symbol(vm, name);
        if (!sym) sym = mini_make_symbol(vm, name);
        Value b = global_binding(vm, sym);
        if (b) {
            uint32_t slot = (uint32_t)id & 31u;
            vm->prim_bcache[slot].id1 = (uint32_t)id + 1u;
            vm->prim_bcache[slot].gen = vm->gcache_gen;
            vm->prim_bcache[slot].binding = b;
            return mini_cdr(b);
        }
        val = global_miss(vm, sym);
        if (!mini_is_void(val)) {
            b = global_binding(vm, sym);
            if (b) {
                uint32_t slot = (uint32_t)id & 31u;
                vm->prim_bcache[slot].id1 = (uint32_t)id + 1u;
                vm->prim_bcache[slot].gen = vm->gcache_gen;
                vm->prim_bcache[slot].binding = b;
            }
            return val;
        }
    }
    if (!vm->has_error && mini_is_void(val)) {
        const char *hint = mini_api_unbound_hook ? mini_api_unbound_hook(mini_api_id_names[id]) : NULL;
        vm_error(vm, "unbound variable: %s%s", mini_api_id_names[id], hint ? hint : "");
    }
    return val;
}

Value mini_vm_run(MiniVM *vm, Value initial_closure, int argc, Value *argv) {
    if (!vm || vm->has_error) return mini_void();
    mini_set_active_vm(vm);
    vm->state_reloaded = false;
    (void)vm_try_fast_flonum;

    int base_frame;
    CallFrame *frames = vm->call_frames;
    int frame_count;
    Value *sp = vm->sp;
    Value *entry_sp = sp;
    Value *fp;
    CodeObj *code;
    uint8_t *pc;
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
    } else {
        base_frame = vm->call_frame_count;
        if (MINI_UNLIKELY((size_t)base_frame >= vm->frame_cap)) {
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

        if (MINI_UNLIKELY(code->is_variadic)) {
            vm->sp = sp;
            code = vm_pack_variadic(vm, frame_count - 1, argc, code->num_params - 1);
            fp = frames[frame_count - 1].fp;
            sp = vm->sp;
        }

        int needed = get_code_num_locals(code);
        if (MINI_UNLIKELY(fp + needed + 64 >= vm->stack + vm->stack_cap)) {
            vm_error(vm, "stack overflow");
            vm->sp = entry_sp;
            vm->call_frame_count = base_frame;
            return mini_void();
        }
        while (sp < fp + needed) *sp++ = MINI_CONST_VOID;
        vm->sp = sp;

        if (MINI_UNLIKELY(code->has_inner_lambdas)) {
            code = vm_alloc_env_frame(vm, frame_count - 1, needed);
            fp = frames[frame_count - 1].fp;
            sp = vm->sp;
        } else {
            frames[frame_count - 1].env = frames[frame_count - 1].closure->env;
        }
        frames[frame_count - 1].pc = CODE_BYTES(code);
        pc = CODE_BYTES(code);
    }

#define SYNC_VM_STATE() do { \
    frames[frame_count - 1].pc = pc; \
    vm->sp = sp; \
    vm->call_frame_count = frame_count; \
    if ((uint32_t)frame_count > vm->stat_frame_peak) vm->stat_frame_peak = (uint32_t)frame_count; \
    if ((uint32_t)(sp - vm->stack) > vm->stat_stack_peak) vm->stat_stack_peak = (uint32_t)(sp - vm->stack); \
} while (0)

#define RELOAD_VM_STATE() do { \
    code = frames[frame_count - 1].code; \
    pc = frames[frame_count - 1].pc; \
    fp = frames[frame_count - 1].fp; \
    sp = vm->sp; \
} while (0)

#define READ_LOCAL(slot) (MINI_LIKELY(!code->has_inner_lambdas) ? fp[slot] : vm_get_heap_local(frames[frame_count - 1].env, (slot)))

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    static const void *const dispatch_table[] = {
        [OP_HALT]              = &&do_op_halt,
        [OP_CONST]             = &&do_op_const,
        [OP_INT8]              = &&do_op_int8,
        [OP_NIL]               = &&do_op_nil,
        [OP_TRUE]              = &&do_op_true,
        [OP_FALSE]             = &&do_op_false,
        [OP_VOID]              = &&do_op_void,
        [OP_GET_LOCAL]         = &&do_op_get_local,
        [OP_SET_LOCAL]         = &&do_op_set_local,
        [OP_GET_UPVALUE]       = &&do_op_get_upvalue,
        [OP_SET_UPVALUE]       = &&do_op_set_upvalue,
        [OP_GET_GLOBAL]        = &&do_op_get_global,
        [OP_SET_GLOBAL]        = &&do_op_set_global,
        [OP_DEF_GLOBAL]        = &&do_op_def_global,
        [OP_POP]               = &&do_op_pop,
        [OP_JUMP]              = &&do_op_jump,
        [OP_JUMP_IF_FALSE]     = &&do_op_jump_if_false,
        [OP_CLOSURE]           = &&do_op_closure,
        [OP_CALL]              = &&do_op_call,
        [OP_TAIL_CALL]         = &&do_op_tail_call,
        [OP_RETURN]            = &&do_op_return,
        [OP_ADD]               = &&do_op_add,
        [OP_SUB]               = &&do_op_sub,
        [OP_MUL]               = &&do_op_mul,
        [OP_LT]                = &&do_op_lt,
        [OP_GT]                = &&do_op_gt,
        [OP_LE]                = &&do_op_le,
        [OP_GE]                = &&do_op_ge,
        [OP_NUM_EQ]            = &&do_op_num_eq,
        [OP_CALL_PRIM]         = &&do_op_call_prim,
        [OP_CAR]               = &&do_op_car,
        [OP_CDR]               = &&do_op_cdr,
        [OP_NOT]               = &&do_op_not,
        [OP_NULLP]             = &&do_op_nullp,
        [OP_PAIRP]             = &&do_op_pairp,
        [OP_EQ]                = &&do_op_eq,
        [OP_VECTOR_REF]        = &&do_op_vector_ref,
        [OP_CALL_SELF]         = &&do_op_call_self,
        [OP_TAIL_CALL_SELF]    = &&do_op_tail_call_self,
        [OP_JUMP_IF_TRUE_KEEP] = &&do_op_jump_if_true_keep,
        [OP_CONS]              = &&do_op_cons,
        [OP_VECTOR_SET]        = &&do_op_vector_set,
        [OP_REM]               = &&do_op_rem,
        [OP_QUOT]              = &&do_op_quot,
        [OP_DIV]               = &&do_op_div,
        [OP_GET_LOCAL2]        = &&do_op_get_local2,
        [OP_SET_LOCAL_POP]     = &&do_op_set_local_pop,
        [OP_ADD_LOC_IMM]       = &&do_op_add_loc_imm,
        [OP_SUB_LOC_IMM]       = &&do_op_sub_loc_imm,
        [OP_ADD_LOC_LOC]       = &&do_op_add_loc_loc,
        [OP_SUB_LOC_LOC]       = &&do_op_sub_loc_loc,
        [OP_MUL_LOC_LOC]       = &&do_op_mul_loc_loc,
        [OP_EQ_LOC_IMM]        = &&do_op_eq_loc_imm,
        [OP_LT_LOC_IMM]        = &&do_op_lt_loc_imm,
        [OP_GT_LOC_IMM]        = &&do_op_gt_loc_imm,
        [OP_EQ_LOC_LOC]        = &&do_op_eq_loc_loc,
        [OP_LT_LOC_LOC]        = &&do_op_lt_loc_loc,
        [OP_LE_LOC_LOC]        = &&do_op_le_loc_loc,
        [OP_GT_LOC_LOC]        = &&do_op_gt_loc_loc,
        [OP_CAR_LOC]           = &&do_op_car_loc,
        [OP_CDR_LOC]           = &&do_op_cdr_loc,
        [OP_NULLP_LOC]         = &&do_op_nullp_loc,
        [OP_COUNT ... 255]     = &&do_op_bad
    };
    #define DISPATCH() goto *dispatch_table[*pc++]
    DISPATCH();
#else
    #define DISPATCH() goto vm_loop
    DISPATCH();

vm_loop:
    switch (*pc++) {
        case OP_HALT:              goto do_op_halt;
        case OP_CONST:             goto do_op_const;
        case OP_INT8:              goto do_op_int8;
        case OP_NIL:               goto do_op_nil;
        case OP_TRUE:              goto do_op_true;
        case OP_FALSE:             goto do_op_false;
        case OP_VOID:              goto do_op_void;
        case OP_GET_LOCAL:         goto do_op_get_local;
        case OP_SET_LOCAL:         goto do_op_set_local;
        case OP_GET_UPVALUE:       goto do_op_get_upvalue;
        case OP_SET_UPVALUE:       goto do_op_set_upvalue;
        case OP_GET_GLOBAL:        goto do_op_get_global;
        case OP_SET_GLOBAL:        goto do_op_set_global;
        case OP_DEF_GLOBAL:        goto do_op_def_global;
        case OP_POP:               goto do_op_pop;
        case OP_JUMP:              goto do_op_jump;
        case OP_JUMP_IF_FALSE:     goto do_op_jump_if_false;
        case OP_CLOSURE:           goto do_op_closure;
        case OP_CALL:              goto do_op_call;
        case OP_TAIL_CALL:         goto do_op_tail_call;
        case OP_RETURN:            goto do_op_return;
        case OP_ADD:               goto do_op_add;
        case OP_SUB:               goto do_op_sub;
        case OP_MUL:               goto do_op_mul;
        case OP_LT:                goto do_op_lt;
        case OP_GT:                goto do_op_gt;
        case OP_LE:                goto do_op_le;
        case OP_GE:                goto do_op_ge;
        case OP_NUM_EQ:            goto do_op_num_eq;
        case OP_CALL_PRIM:         goto do_op_call_prim;
        case OP_CAR:               goto do_op_car;
        case OP_CDR:               goto do_op_cdr;
        case OP_NOT:               goto do_op_not;
        case OP_NULLP:             goto do_op_nullp;
        case OP_PAIRP:             goto do_op_pairp;
        case OP_EQ:                goto do_op_eq;
        case OP_VECTOR_REF:        goto do_op_vector_ref;
        case OP_CALL_SELF:         goto do_op_call_self;
        case OP_TAIL_CALL_SELF:    goto do_op_tail_call_self;
        case OP_JUMP_IF_TRUE_KEEP: goto do_op_jump_if_true_keep;
        case OP_CONS:              goto do_op_cons;
        case OP_VECTOR_SET:        goto do_op_vector_set;
        case OP_REM:               goto do_op_rem;
        case OP_QUOT:              goto do_op_quot;
        case OP_DIV:               goto do_op_div;
        case OP_GET_LOCAL2:        goto do_op_get_local2;
        case OP_SET_LOCAL_POP:     goto do_op_set_local_pop;
        case OP_ADD_LOC_IMM:       goto do_op_add_loc_imm;
        case OP_SUB_LOC_IMM:       goto do_op_sub_loc_imm;
        case OP_ADD_LOC_LOC:       goto do_op_add_loc_loc;
        case OP_SUB_LOC_LOC:       goto do_op_sub_loc_loc;
        case OP_MUL_LOC_LOC:       goto do_op_mul_loc_loc;
        case OP_EQ_LOC_IMM:        goto do_op_eq_loc_imm;
        case OP_LT_LOC_IMM:        goto do_op_lt_loc_imm;
        case OP_GT_LOC_IMM:        goto do_op_gt_loc_imm;
        case OP_EQ_LOC_LOC:        goto do_op_eq_loc_loc;
        case OP_LT_LOC_LOC:        goto do_op_lt_loc_loc;
        case OP_LE_LOC_LOC:        goto do_op_le_loc_loc;
        case OP_GT_LOC_LOC:        goto do_op_gt_loc_loc;
        case OP_CAR_LOC:           goto do_op_car_loc;
        case OP_CDR_LOC:           goto do_op_cdr_loc;
        case OP_NULLP_LOC:         goto do_op_nullp_loc;
        default:                   goto do_op_bad;
    }
#endif
    {

    do_op_halt: {
        goto vm_done;
    }

    do_op_bad: {
        vm_error(vm, "invalid instruction %u (an image for a newer firmware API?)", (unsigned)pc[-1]);
        goto vm_err;
    }

#define PRIM_SLOW(prim, n) do { \
        SYNC_VM_STATE(); \
        Value res_ = prim(vm, (n), sp - (n)); \
        if (MINI_UNLIKELY(vm->has_error)) goto vm_err; \
        RELOAD_VM_STATE(); \
        sp -= (n); \
        *sp++ = res_; \
        DISPATCH(); \
    } while (0)
#define ARITH_SLOW(prim) PRIM_SLOW(prim, 2)

#define BOOL_RESULT(r, n) do { \
        bool r_ = (r); \
        uint8_t next_op_ = *pc; \
        if (MINI_LIKELY(next_op_ == OP_JUMP_IF_FALSE)) { \
            int16_t off_ = (int16_t)(pc[1] | ((uint16_t)pc[2] << 8)); \
            pc += 3; \
            sp -= (int)(n); \
            if (!r_) pc += off_; \
            DISPATCH(); \
        } \
        if (next_op_ == OP_JUMP_IF_TRUE_KEEP) { \
            int16_t off_ = (int16_t)(pc[1] | ((uint16_t)pc[2] << 8)); \
            pc += 3; \
            if (r_) { \
                sp = sp - (int)(n) + 1; \
                sp[-1] = MINI_CONST_TRUE; \
                pc += off_; \
            } else { \
                sp -= (int)(n); \
            } \
            DISPATCH(); \
        } \
        sp = sp - (int)(n) + 1; \
        sp[-1] = r_ ? MINI_CONST_TRUE : MINI_CONST_FALSE; \
        DISPATCH(); \
    } while (0)

#define ARITH_COMPARE(cmp, prim) do { \
        Value a_ = sp[-2], b_ = sp[-1]; \
        if (MINI_LIKELY(mini_is_fixnum(a_) && mini_is_fixnum(b_))) { \
            BOOL_RESULT((intptr_t)a_ cmp (intptr_t)b_, 2); \
        } \
        double da_, db_; \
        if (MINI_LIKELY(vm_flonum_pair_to_double(a_, b_, &da_, &db_))) { \
            BOOL_RESULT(da_ cmp db_, 2); \
        } \
        ARITH_SLOW(prim); \
    } while (0)

#define BIN_FLONUM_FAST(a_, b_, fop_) do { \
        double da_, db_; \
        if (MINI_LIKELY(vm_flonum_pair_to_double((a_), (b_), &da_, &db_))) { \
            Value fv_; \
            if (MINI_LIKELY(vm_try_make_flonum_inline(vm, da_ fop_ db_, &fv_))) { \
                sp--; \
                sp[-1] = fv_; \
                DISPATCH(); \
            } \
            SYNC_VM_STATE(); \
            fv_ = vm_box_flonum_slow(vm, da_ fop_ db_); \
            if (MINI_UNLIKELY(vm->has_error)) goto vm_err; \
            RELOAD_VM_STATE(); \
            sp--; \
            sp[-1] = fv_; \
            DISPATCH(); \
        } \
    } while (0)

    do_op_add: {
        Value a = sp[-2], b = sp[-1];
        intptr_t r;
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && fixnum_add_ok(a, b, &r))) {
            sp--;
            sp[-1] = (Value)r;
            DISPATCH();
        }
        BIN_FLONUM_FAST(a, b, +);
        ARITH_SLOW(prim_add);
    }
    do_op_sub: {
        Value a = sp[-2], b = sp[-1];
        intptr_t r;
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && fixnum_sub_ok(a, b, &r))) {
            sp--;
            sp[-1] = (Value)r;
            DISPATCH();
        }
        BIN_FLONUM_FAST(a, b, -);
        ARITH_SLOW(prim_sub);
    }
    do_op_mul: {
        Value a = sp[-2], b = sp[-1];
        intptr_t r;
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && fixnum_mul_ok(a, b, &r))) {
            sp--;
            sp[-1] = (Value)r;
            DISPATCH();
        }
        BIN_FLONUM_FAST(a, b, *);
        ARITH_SLOW(prim_mul);
    }
    do_op_div: {
        Value a = sp[-2], b = sp[-1];
        if (((a | b) & MINI_TAG_MASK) == 0) {
            int64_t x = mini_to_fixnum(a), y = mini_to_fixnum(b);
            if (MINI_LIKELY(y != 0)) {
                if (MINI_LIKELY(!(x == MINI_FIXNUM_MIN && y == -1)) && x % y == 0) {
                    sp--;
                    sp[-1] = mini_make_fixnum(x / y);
                    DISPATCH();
                }
                if (MINI_LIKELY(!(x == MINI_FIXNUM_MIN && y == -1))) {
                    Value fv;
                    if (MINI_LIKELY(vm_try_make_flonum_inline(vm, (double)x / (double)y, &fv))) {
                        sp--;
                        sp[-1] = fv;
                        DISPATCH();
                    }
                }
            }
            ARITH_SLOW(prim_div);
        }
        double da, db;
        if (MINI_LIKELY(vm_flonum_pair_to_double(a, b, &da, &db) && db != 0.0)) {
            Value fv;
            if (MINI_LIKELY(vm_try_make_flonum_inline(vm, da / db, &fv))) {
                sp--;
                sp[-1] = fv;
                DISPATCH();
            }
        }
        ARITH_SLOW(prim_div);
    }
    do_op_lt:     ARITH_COMPARE(<,  prim_lt);
    do_op_gt:     ARITH_COMPARE(>,  prim_gt);
    do_op_le:     ARITH_COMPARE(<=, prim_le);
    do_op_ge:     ARITH_COMPARE(>=, prim_ge);
    do_op_num_eq: ARITH_COMPARE(==, prim_num_eq);

    do_op_car: {
        Value p = sp[-1];
        if (MINI_LIKELY(mini_is_pair(p))) {
            sp[-1] = ((const MiniPairCells *)UNTAG_PTR(p))->car;
            DISPATCH();
        }
        PRIM_SLOW(prim_car, 1);
    }
    do_op_cdr: {
        Value p = sp[-1];
        if (MINI_LIKELY(mini_is_pair(p))) {
            sp[-1] = ((const MiniPairCells *)UNTAG_PTR(p))->cdr;
            DISPATCH();
        }
        PRIM_SLOW(prim_cdr, 1);
    }
    do_op_not:    BOOL_RESULT(sp[-1] == MINI_CONST_FALSE, 1);
    do_op_nullp:  BOOL_RESULT(sp[-1] == MINI_CONST_NIL, 1);
    do_op_pairp:  BOOL_RESULT(mini_is_pair(sp[-1]), 1);
    do_op_eq:     BOOL_RESULT(sp[-2] == sp[-1], 2);

    do_op_vector_ref: {
        Value v = sp[-2], k = sp[-1];
        if (MINI_LIKELY(mini_is_vector(v) && mini_is_fixnum(k))) {
            const VectorObj *vec = (const VectorObj *)UNTAG_PTR(v);
            int64_t i = mini_to_fixnum(k);
            if (MINI_LIKELY(i >= 0 && (uint64_t)i < (uint64_t)vec->length)) {
                sp--;
                sp[-1] = vec->data[i];
                DISPATCH();
            }
        }
        PRIM_SLOW(prim_vector_ref, 2);
    }

    do_op_vector_set: {
        Value v = sp[-3], k = sp[-2], val = sp[-1];
        if (MINI_LIKELY(mini_is_vector(v) && mini_is_fixnum(k))) {
            VectorObj *vec = (VectorObj *)UNTAG_PTR(v);
            int64_t i = mini_to_fixnum(k);
            size_t rom_len = (size_t)(vm->xrom_end - vm->xrom_start);
            bool in_rom = rom_len > 0 && ((uintptr_t)vec - (uintptr_t)vm->xrom_start) < rom_len;
            if (MINI_LIKELY(i >= 0 && (uint64_t)i < (uint64_t)vec->length && !in_rom)) {
                vec->data[i] = val;
                mini_write_barrier(vm, v, val);
                if (*pc == OP_POP) {
                    pc++;
                    sp -= 3;
                    DISPATCH();
                }
                sp -= 2;
                sp[-1] = MINI_CONST_VOID;
                DISPATCH();
            }
        }
        PRIM_SLOW(prim_vector_set, 3);
    }

    do_op_cons: {
        size_t aligned_sz = ALIGN8(sizeof(PairObj));
        if (MINI_LIKELY(vm->eden_hp + aligned_sz <= vm->eden_limit)) {
            PairObj *p = (PairObj *)vm->eden_hp;
            vm->eden_hp += aligned_sz;
            vm->stat_alloc_bytes += aligned_sz;
            PROF_SAVE(prof_prev);
            PROF_SITE("cons");
#ifdef MINI_ALLOC_PROFILE
            mini_prof_alloc(aligned_sz, OBJ_PAIR);
#endif
            PROF_SITE(prof_prev);
            p->header.type = OBJ_PAIR;
            p->header.size = (uint32_t)aligned_sz;
            p->car = sp[-2];
            p->cdr = sp[-1];
            sp--;
            sp[-1] = ((uintptr_t)p) | MINI_TAG_PAIR;
            DISPATCH();
        }
        PRIM_SLOW(prim_cons_slow, 2);
    }

    do_op_rem: {
        Value a = sp[-2], b = sp[-1];
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && b != 0)) {
            sp--;
            sp[-1] = (Value)((intptr_t)a % (intptr_t)b);
            DISPATCH();
        }
        ARITH_SLOW(prim_remainder);
    }

    do_op_quot: {
        Value a = sp[-2], b = sp[-1];
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && b != 0)) {
            int64_t q = (intptr_t)a / (intptr_t)b;
            if (MINI_LIKELY(q <= MINI_FIXNUM_MAX)) {
                sp--;
                sp[-1] = mini_make_fixnum(q);
                DISPATCH();
            }
        }
        ARITH_SLOW(prim_quotient);
    }

    /* API 1.16 Local Operand Superinstructions */
    do_op_get_local2: {
        uint8_t s0 = pc[0], s1 = pc[1];
        pc += 2;
        if (MINI_LIKELY(!code->has_inner_lambdas)) {
            sp[0] = fp[s0];
            sp[1] = fp[s1];
            sp += 2;
            DISPATCH();
        }
        Value env = frames[frame_count - 1].env;
        sp[0] = vm_get_heap_local(env, s0);
        sp[1] = vm_get_heap_local(env, s1);
        sp += 2;
        DISPATCH();
    }

    do_op_set_local_pop: {
        uint8_t slot = *pc++;
        Value val = *(--sp);
        fp[slot] = val;
        if (MINI_UNLIKELY(code->has_inner_lambdas)) {
            vm_set_heap_local(vm, frames[frame_count - 1].env, slot, val);
        }
        DISPATCH();
    }

    do_op_add_loc_imm: {
        uint8_t slot = pc[0];
        Value imm = mini_make_fixnum((int8_t)pc[1]);
        pc += 2;
        Value a = READ_LOCAL(slot);
        intptr_t r;
        if (MINI_LIKELY(mini_is_fixnum(a) && fixnum_add_ok(a, imm, &r))) {
            *sp++ = (Value)r;
            DISPATCH();
        }
        sp[0] = a; sp[1] = imm; sp += 2;
        BIN_FLONUM_FAST(a, imm, +);
        ARITH_SLOW(prim_add);
    }

    do_op_sub_loc_imm: {
        uint8_t slot = pc[0];
        Value imm = mini_make_fixnum((int8_t)pc[1]);
        pc += 2;
        Value a = READ_LOCAL(slot);
        intptr_t r;
        if (MINI_LIKELY(mini_is_fixnum(a) && fixnum_sub_ok(a, imm, &r))) {
            *sp++ = (Value)r;
            DISPATCH();
        }
        sp[0] = a; sp[1] = imm; sp += 2;
        BIN_FLONUM_FAST(a, imm, -);
        ARITH_SLOW(prim_sub);
    }

    do_op_add_loc_loc: {
        uint8_t s0 = pc[0], s1 = pc[1];
        pc += 2;
        Value a = READ_LOCAL(s0), b = READ_LOCAL(s1);
        intptr_t r;
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && fixnum_add_ok(a, b, &r))) {
            *sp++ = (Value)r;
            DISPATCH();
        }
        double da, db;
        if (MINI_LIKELY(vm_flonum_pair_to_double(a, b, &da, &db))) {
            Value fv;
            if (MINI_LIKELY(vm_try_make_flonum_inline(vm, da + db, &fv))) {
                *sp++ = fv;
                DISPATCH();
            }
        }
        sp[0] = a; sp[1] = b; sp += 2;
        ARITH_SLOW(prim_add);
    }

    do_op_sub_loc_loc: {
        uint8_t s0 = pc[0], s1 = pc[1];
        pc += 2;
        Value a = READ_LOCAL(s0), b = READ_LOCAL(s1);
        intptr_t r;
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && fixnum_sub_ok(a, b, &r))) {
            *sp++ = (Value)r;
            DISPATCH();
        }
        double da, db;
        if (MINI_LIKELY(vm_flonum_pair_to_double(a, b, &da, &db))) {
            Value fv;
            if (MINI_LIKELY(vm_try_make_flonum_inline(vm, da - db, &fv))) {
                *sp++ = fv;
                DISPATCH();
            }
        }
        sp[0] = a; sp[1] = b; sp += 2;
        ARITH_SLOW(prim_sub);
    }

    do_op_mul_loc_loc: {
        uint8_t s0 = pc[0], s1 = pc[1];
        pc += 2;
        Value a = READ_LOCAL(s0), b = READ_LOCAL(s1);
        intptr_t r;
        if (MINI_LIKELY(((a | b) & MINI_TAG_MASK) == 0 && fixnum_mul_ok(a, b, &r))) {
            *sp++ = (Value)r;
            DISPATCH();
        }
        double da, db;
        if (MINI_LIKELY(vm_flonum_pair_to_double(a, b, &da, &db))) {
            Value fv;
            if (MINI_LIKELY(vm_try_make_flonum_inline(vm, da * db, &fv))) {
                *sp++ = fv;
                DISPATCH();
            }
        }
        sp[0] = a; sp[1] = b; sp += 2;
        ARITH_SLOW(prim_mul);
    }

#define CMP_LOC_IMM_IMPL(cmp, prim) do { \
        uint8_t slot_ = pc[0]; \
        Value imm_ = mini_make_fixnum((int8_t)pc[1]); \
        pc += 2; \
        Value a_ = READ_LOCAL(slot_); \
        if (MINI_LIKELY(mini_is_fixnum(a_))) { \
            BOOL_RESULT((intptr_t)a_ cmp (intptr_t)imm_, 0); \
        } \
        double da_; \
        if (MINI_LIKELY(mini_is_flonum(a_) && vm_num_to_double_fast(a_, &da_))) { \
            BOOL_RESULT(da_ cmp (double)mini_to_fixnum(imm_), 0); \
        } \
        sp[0] = a_; sp[1] = imm_; sp += 2; \
        ARITH_SLOW(prim); \
    } while (0)

#define CMP_LOC_LOC_IMPL(cmp, prim) do { \
        uint8_t s0_ = pc[0], s1_ = pc[1]; \
        pc += 2; \
        Value a_ = READ_LOCAL(s0_), b_ = READ_LOCAL(s1_); \
        if (MINI_LIKELY(((a_ | b_) & MINI_TAG_MASK) == 0)) { \
            BOOL_RESULT((intptr_t)a_ cmp (intptr_t)b_, 0); \
        } \
        double da_, db_; \
        if (MINI_LIKELY(vm_flonum_pair_to_double(a_, b_, &da_, &db_))) { \
            BOOL_RESULT(da_ cmp db_, 0); \
        } \
        sp[0] = a_; sp[1] = b_; sp += 2; \
        ARITH_SLOW(prim); \
    } while (0)

    do_op_eq_loc_imm: CMP_LOC_IMM_IMPL(==, prim_num_eq);
    do_op_lt_loc_imm: CMP_LOC_IMM_IMPL(<,  prim_lt);
    do_op_gt_loc_imm: CMP_LOC_IMM_IMPL(>,  prim_gt);

    do_op_eq_loc_loc: CMP_LOC_LOC_IMPL(==, prim_num_eq);
    do_op_lt_loc_loc: CMP_LOC_LOC_IMPL(<,  prim_lt);
    do_op_le_loc_loc: CMP_LOC_LOC_IMPL(<=, prim_le);
    do_op_gt_loc_loc: CMP_LOC_LOC_IMPL(>,  prim_gt);

    do_op_car_loc: {
        uint8_t slot = *pc++;
        Value p = READ_LOCAL(slot);
        if (MINI_LIKELY(mini_is_pair(p))) {
            *sp++ = ((const MiniPairCells *)UNTAG_PTR(p))->car;
            DISPATCH();
        }
        *sp++ = p;
        PRIM_SLOW(prim_car, 1);
    }

    do_op_cdr_loc: {
        uint8_t slot = *pc++;
        Value p = READ_LOCAL(slot);
        if (MINI_LIKELY(mini_is_pair(p))) {
            *sp++ = ((const MiniPairCells *)UNTAG_PTR(p))->cdr;
            DISPATCH();
        }
        *sp++ = p;
        PRIM_SLOW(prim_cdr, 1);
    }

    do_op_nullp_loc: {
        uint8_t slot = *pc++;
        Value v = READ_LOCAL(slot);
        BOOL_RESULT(v == MINI_CONST_NIL, 0);
    }

    do_op_call_prim: {
        uint16_t id = (uint16_t)(pc[0] | ((uint16_t)pc[1] << 8));
        call_argc = pc[2];
        pc += 3;
        MiniCFunc fn = (vm->prim_fns && id < MINI_API_ID_COUNT) ? vm->prim_fns[id] : NULL;
        if (MINI_UNLIKELY(!fn)) {
            SYNC_VM_STATE();
            fn = prim_resolve(vm, id);
            RELOAD_VM_STATE();
            if (!fn) goto vm_err;
        }
        if (MINI_UNLIKELY(fn == prim_by_call)) {
            uint32_t slot = (uint32_t)id & 31u;
            Value val;
            if (MINI_LIKELY(vm->prim_bcache[slot].id1 == (uint32_t)id + 1u &&
                            vm->prim_bcache[slot].gen == vm->gcache_gen)) {
                val = mini_cdr(vm->prim_bcache[slot].binding);
            } else {
                SYNC_VM_STATE();
                val = vm_prim_by_call_lookup(vm, id);
                RELOAD_VM_STATE();
                if (MINI_UNLIKELY(vm->has_error)) goto vm_err;
            }
            for (int i = 0; i < call_argc; i++) sp[-i] = sp[-i - 1];
            sp[-call_argc] = val;
            sp++;
            callee = val;
            goto do_call_dispatch;
        }
        SYNC_VM_STATE();
        {
            PROF_SAVE(prof_prev);
            PROF_SITE(mini_api_id_names[id]);
            Value res = fn(vm, call_argc, sp - call_argc);
            PROF_SITE(prof_prev);
            if (MINI_UNLIKELY(vm->has_error)) goto vm_err;
            if (MINI_UNLIKELY(vm->state_reloaded)) {
                vm->state_reloaded = false;
                vm->sp = vm->stack;
                vm->call_frame_count = 0;
                return res;
            }
            RELOAD_VM_STATE();
            sp -= call_argc;
            *sp++ = res;
        }
        DISPATCH();
    }

#undef CMP_LOC_LOC_IMPL
#undef CMP_LOC_IMM_IMPL
#undef BIN_FLONUM_FAST
#undef ARITH_COMPARE
#undef ARITH_SLOW
#undef PRIM_SLOW
#undef BOOL_RESULT

    do_op_const: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        if (MINI_UNLIKELY(vm->rom_base != NULL)) {
            SYNC_VM_STATE();
            Value cv = vm_fetch_const(vm, code, idx);
            RELOAD_VM_STATE();
            *sp++ = cv;
            DISPATCH();
        }
        *sp++ = CODE_CONSTS(code)[idx];
        DISPATCH();
    }

    do_op_int8: {
        *sp++ = mini_make_fixnum((int8_t)*pc++);
        DISPATCH();
    }

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
        if (MINI_UNLIKELY(code->has_inner_lambdas)) {
            *sp++ = vm_get_heap_local(frames[frame_count - 1].env, slot);
            DISPATCH();
        }
        *sp++ = fp[slot];
        DISPATCH();
    }

    do_op_set_local: {
        uint8_t slot = *pc++;
        Value val = *(sp - 1);
        fp[slot] = val;
        if (MINI_UNLIKELY(code->has_inner_lambdas)) {
            vm_set_heap_local(vm, frames[frame_count - 1].env, slot, val);
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

    do_op_get_global: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value sym = vm_fetch_const(vm, code, idx);
        Value val = env_lookup(vm, vm->global_env, sym);
        if (MINI_UNLIKELY(mini_is_void(val))) {
            SYNC_VM_STATE();
            val = global_miss(vm, sym);
            RELOAD_VM_STATE();
            if (MINI_UNLIKELY(mini_is_void(val))) {
                const char *hint = mini_api_unbound_hook ? mini_api_unbound_hook(mini_symbol_name(sym)) : NULL;
                vm_error(vm, "unbound variable: %s%s", mini_symbol_name(sym), hint ? hint : "");
                goto vm_err;
            }
        }
        *sp++ = val;
        DISPATCH();
    }

    do_op_set_global: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value sym = vm_fetch_const(vm, code, idx);
        SYNC_VM_STATE();
        env_set(vm, vm->global_env, sym, *(sp - 1));
        RELOAD_VM_STATE();
        if (vm->has_error) goto vm_err;
        DISPATCH();
    }

    do_op_def_global: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value sym = vm_fetch_const(vm, code, idx);
        Value val = *(--sp);
        SYNC_VM_STATE();
        env_define(vm, vm->global_env, sym, val);
        if (vm->has_error) goto vm_err;
        RELOAD_VM_STATE();
        *sp++ = sym;
        DISPATCH();
    }

    do_op_pop: {
        --sp;
        DISPATCH();
    }

    do_op_jump: {
        int16_t offset = (int16_t)(pc[0] | ((uint16_t)pc[1] << 8));
        pc += 2 + offset;
        DISPATCH();
    }

    do_op_jump_if_false: {
        int16_t offset = (int16_t)(pc[0] | ((uint16_t)pc[1] << 8));
        pc += 2;
        Value cond = *(--sp);
        if (cond == MINI_CONST_FALSE) {
            pc += offset;
        }
        DISPATCH();
    }

    do_op_jump_if_true_keep: {
        int16_t offset = (int16_t)(pc[0] | ((uint16_t)pc[1] << 8));
        pc += 2;
        if (sp[-1] != MINI_CONST_FALSE) {
            pc += offset;
        } else {
            --sp;
        }
        DISPATCH();
    }

    do_op_closure: {
        uint16_t idx = pc[0] | ((uint16_t)pc[1] << 8);
        pc += 2;
        Value code_val = vm_fetch_const(vm, code, idx);
        SYNC_VM_STATE();
        PROF_SAVE(prof_prev);
        PROF_SITE("<closure>");
        PROF_CODE(UNTAG_PTR(code_val));
        Value clo = mini_make_closure(vm, code_val, frames[frame_count - 1].env);
        PROF_SITE(prof_prev);
        if (MINI_UNLIKELY(vm->has_error)) goto vm_err;
        RELOAD_VM_STATE();
        *sp++ = clo;
        DISPATCH();
    }

    /* API 1.16 Direct Self-Recursion (non-tail & tail) */
    do_op_call_self: {
        call_argc = *pc++;
        ClosureObj *self_cl = frames[frame_count - 1].closure;
        Value self_val = ((uintptr_t)self_cl) | MINI_TAG_CLOSURE;
        for (int i = 0; i < (int)call_argc; i++) sp[-i] = sp[-i - 1];
        sp[-call_argc] = self_val;
        sp++;
        if (MINI_LIKELY(!code->is_variadic && !code->has_inner_lambdas &&
                        call_argc == code->num_params &&
                        (size_t)frame_count < vm->frame_cap)) {
            Value *new_fp = sp - call_argc;
            int needed_locals = get_code_num_locals(code);
            if (MINI_UNLIKELY(new_fp + needed_locals + 64 >= vm->stack + vm->stack_cap)) {
                vm_error(vm, "stack overflow");
                goto vm_err;
            }
            frames[frame_count - 1].pc = pc;
            frames[frame_count].closure = self_cl;
            frames[frame_count].code = code;
            frames[frame_count].pc = CODE_BYTES(code);
            frames[frame_count].fp = new_fp;
            frames[frame_count].env = self_cl->env;
            frame_count++;
            while (sp < new_fp + needed_locals) *sp++ = MINI_CONST_VOID;
            fp = new_fp;
            pc = CODE_BYTES(code);
            DISPATCH();
        }
        callee = self_val;
        goto do_call_dispatch;
    }

    do_op_tail_call_self: {
        call_argc = *pc++;
        if (MINI_LIKELY(!code->is_variadic && !code->has_inner_lambdas &&
                        call_argc == code->num_params)) {
            Value *args_src = sp - call_argc;
            for (int i = 0; i < (int)call_argc; i++) {
                fp[i] = args_src[i];
            }
            int locals = get_code_num_locals(code);
            sp = fp + call_argc;
            while (sp < fp + locals) *sp++ = MINI_CONST_VOID;
            pc = CODE_BYTES(code);
            DISPATCH();
        }
        ClosureObj *self_cl = frames[frame_count - 1].closure;
        Value self_val = ((uintptr_t)self_cl) | MINI_TAG_CLOSURE;
        for (int i = 0; i < (int)call_argc; i++) sp[-i] = sp[-i - 1];
        sp[-call_argc] = self_val;
        sp++;
        callee = self_val;
        goto do_tail_call_dispatch;
    }

    /* General procedure invocation — closure fast path first! */
    do_op_call: {
        call_argc = *pc++;
        callee = *(sp - call_argc - 1);

    do_call_dispatch:
        if (MINI_LIKELY(mini_is_closure(callee))) {
            if (MINI_UNLIKELY((size_t)frame_count >= vm->frame_cap)) {
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

            if (MINI_UNLIKELY(callee_code->is_variadic)) {
                vm->sp = sp;
                vm->call_frame_count = frame_count;
                callee_code = vm_pack_variadic(vm, frame_count - 1, call_argc, callee_code->num_params - 1);
                new_fp = frames[frame_count - 1].fp;
                sp = vm->sp;
            }

            int needed_locals = get_code_num_locals(callee_code);
            if (MINI_UNLIKELY(new_fp + needed_locals + 64 >= vm->stack + vm->stack_cap)) {
                vm_error(vm, "stack overflow");
                goto vm_err;
            }
            while (sp < new_fp + needed_locals) *sp++ = MINI_CONST_VOID;

            if (MINI_UNLIKELY(callee_code->has_inner_lambdas)) {
                vm->sp = sp;
                vm->call_frame_count = frame_count;
                callee_code = vm_alloc_env_frame(vm, frame_count - 1, needed_locals);
                new_fp = frames[frame_count - 1].fp;
                sp = vm->sp;
                frames[frame_count - 1].pc = CODE_BYTES(callee_code);
            }

            fp = new_fp;
            pc = CODE_BYTES(callee_code);
            code = callee_code;
            DISPATCH();
        }

        if (mini_is_cfunc(callee)) {
            CFuncObj *cf = (CFuncObj *)UNTAG_PTR(callee);
            if (MINI_UNLIKELY(cf->fn == prim_callcc)) {
                if (call_argc != 1) {
                    vm_error(vm, "call/cc: expected 1 argument, got %d", (int)call_argc);
                    goto vm_err;
                }
                Value proc = *(sp - 1);
                if (!mini_is_closure(proc) && !mini_is_cfunc(proc) && !mini_is_continuation(proc)) {
                    vm_error(vm, "call/cc: argument must be a procedure");
                    goto vm_err;
                }
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

            SYNC_VM_STATE();
            PROF_SAVE(prof_prev);
            PROF_SITE(cf->name);
            Value res = cf->fn(vm, call_argc, sp - call_argc);
            PROF_SITE(prof_prev);
            if (MINI_UNLIKELY(vm->has_error)) goto vm_err;
            if (MINI_UNLIKELY(vm->state_reloaded)) {
                vm->state_reloaded = false;
                vm->sp = vm->stack;
                vm->call_frame_count = 0;
                return res;
            }
            RELOAD_VM_STATE();
            sp -= (call_argc + 1);
            *sp++ = res;
            DISPATCH();
        }

        if (mini_is_continuation(callee)) {
            goto do_continuation_restore;
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
        if (MINI_LIKELY(mini_is_closure(callee))) {
            ClosureObj *callee_cl = (ClosureObj *)UNTAG_PTR(callee);
            CodeObj *callee_code = (CodeObj *)UNTAG_PTR(callee_cl->code);
            Value *args_src = sp - call_argc;

            for (int i = 0; i < call_argc; i++) {
                fp[i] = args_src[i];
            }
            sp = fp + call_argc;

            if (callee_cl == frames[frame_count - 1].closure && !callee_code->is_variadic &&
                !callee_code->has_inner_lambdas && call_argc == callee_code->num_params) {
                int locals = get_code_num_locals(callee_code);
                while (sp < fp + locals) *sp++ = MINI_CONST_VOID;
                pc = CODE_BYTES(callee_code);
                DISPATCH();
            }

            frames[frame_count - 1].closure = callee_cl;
            frames[frame_count - 1].code = callee_code;
            frames[frame_count - 1].pc = CODE_BYTES(callee_code);
            frames[frame_count - 1].fp = fp;
            frames[frame_count - 1].env = callee_cl->env;

            if (MINI_UNLIKELY(callee_code->is_variadic)) {
                vm->sp = sp;
                vm->call_frame_count = frame_count;
                callee_code = vm_pack_variadic(vm, frame_count - 1, call_argc, callee_code->num_params - 1);
                fp = frames[frame_count - 1].fp;
                sp = vm->sp;
            }

            int needed_locals = get_code_num_locals(callee_code);
            if (MINI_UNLIKELY(fp + needed_locals + 64 >= vm->stack + vm->stack_cap)) {
                vm_error(vm, "stack overflow");
                goto vm_err;
            }
            while (sp < fp + needed_locals) *sp++ = MINI_CONST_VOID;

            if (MINI_UNLIKELY(callee_code->has_inner_lambdas)) {
                vm->sp = sp;
                vm->call_frame_count = frame_count;
                callee_code = vm_alloc_env_frame(vm, frame_count - 1, needed_locals);
                fp = frames[frame_count - 1].fp;
                sp = vm->sp;
                frames[frame_count - 1].pc = CODE_BYTES(callee_code);
            }

            pc = CODE_BYTES(callee_code);
            code = callee_code;
            DISPATCH();
        }

        if (mini_is_cfunc(callee)) {
            CFuncObj *cf = (CFuncObj *)UNTAG_PTR(callee);
            if (MINI_UNLIKELY(cf->fn == prim_callcc)) {
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

            SYNC_VM_STATE();
            PROF_SAVE(prof_prev);
            PROF_SITE(cf->name);
            Value res = cf->fn(vm, call_argc, sp - call_argc);
            PROF_SITE(prof_prev);
            if (MINI_UNLIKELY(vm->has_error)) goto vm_err;
            if (MINI_UNLIKELY(vm->state_reloaded)) {
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
            DISPATCH();
        }

        if (mini_is_continuation(callee)) {
            goto do_continuation_restore;
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
        if (MINI_UNLIKELY(frame_count == base_frame)) {
            vm->call_frame_count = frame_count;
            vm->sp = fp - 1;
            return ret_val;
        }
        CallFrame *parent_f = &frames[frame_count - 1];
        sp = fp - 1;
        *sp++ = ret_val;
        fp = parent_f->fp;
        pc = parent_f->pc;
        code = parent_f->code;
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
        DISPATCH();
    }

    }

#undef READ_LOCAL
#undef RELOAD_VM_STATE
#undef SYNC_VM_STATE

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
