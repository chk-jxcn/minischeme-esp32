/**
 * @file minischeme_gc.c
 * @brief MiniScheme 2-Space Cheney Copying Garbage Collector & Heap Allocator.
 *
 * Implements:
 *  - 2-space Cheney copying GC with automatic geometric expansion.
 *  - 8-byte aligned allocation.
 *  - GC root management for C extensions.
 *  - Heap object constructors and accessors (pairs, symbols, strings, vectors, etc.).
 *  - Deep equality comparison (mini_equal).
 */

#include "minischeme_internal.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif

/* ========================================================================= */
/* Memory Allocation & Cheney Copying GC                                     */
/* ========================================================================= */

/* ========================================================================= */
/* Word-only memory (see minischeme_internal.h)                              */
/* ========================================================================= */

static size_t s_word_iram = 0;

void *mini_word_alloc(size_t bytes) {
#if defined(ESP_PLATFORM)
    /* EXEC|32BIT prefers the IRAM-only region; past it the allocator would
     * carve DRAM through its IRAM alias, which saves nothing */
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (p && esp_ptr_in_iram(p) && !esp_ptr_in_diram_iram(p)) {
        s_word_iram += bytes;
        return p;
    }
    if (p) heap_caps_free(p);
#endif
    return malloc(bytes);
}

void mini_words_copy(void *dst, const void *src, size_t bytes) {
    /* volatile: the compiler must not turn the loop back into memcpy */
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    for (size_t i = 0; i < bytes / 4; i++) d[i] = s[i];
}

void mini_words_fill(void *dst, uint32_t value, size_t bytes) {
    volatile uint32_t *d = (volatile uint32_t *)dst;
    for (size_t i = 0; i < bytes / 4; i++) d[i] = value;
}

void *mini_word_grow(void *old, size_t old_bytes, size_t new_bytes) {
    void *p = mini_word_alloc(new_bytes);
    if (!p) return NULL;
    if (old) {
        mini_words_copy(p, old, old_bytes < new_bytes ? old_bytes : new_bytes);
#if defined(ESP_PLATFORM)
        if (esp_ptr_in_iram(old) && !esp_ptr_in_diram_iram(old)) {
            s_word_iram -= old_bytes < s_word_iram ? old_bytes : s_word_iram;
        }
#endif
        free(old);
    }
    return p;
}

size_t mini_word_iram_bytes(void) {
    return s_word_iram;
}

void mini_vm_mem_owners(MiniVM *vm, MiniMemOwnerFn fn, void *user) {
    if (!vm) return;
    fn(vm, "vm struct", user);
    fn(vm->tenured_raw, "vm heap", user);
    fn(vm->eden_raw, "vm eden", user);
    fn(vm->stack, "vm stack", user);
    fn(vm->call_frames, "vm frames", user);
    fn(vm->mark_stack, "gc mark stack", user);
    fn(vm->remset, "gc remset", user);
    fn(vm->c_roots, "vm c roots", user);
    fn(vm->gcache, "vm global cache", user);
}

char *alloc_aligned8(size_t size, char **raw_ptr) {
#if defined(ESP_PLATFORM)
    char *raw = NULL;
    if (size >= 64 * 1024) {
        raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
    } else {
        raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!raw) {
        raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    if (!raw) {
        raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
    }
    if (!raw) {
        raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_8BIT);
    }
#else
    char *raw = (char *)malloc(size + 8);
#endif
    if (!raw) {
        *raw_ptr = NULL;
        return NULL;
    }
    *raw_ptr = raw;
    uintptr_t p = (uintptr_t)raw;
    uintptr_t aligned = (p + 7) & ~(uintptr_t)7;
    return (char *)aligned;
}

static MiniVM *s_active_vm = NULL;

void mini_set_active_vm(MiniVM *vm) {
    s_active_vm = vm;
}

MiniVM *mini_get_active_vm(void) {
    return s_active_vm;
}

void mini_remset_add(MiniVM *vm, Value parent) {
    if (!vm) return;
    if (vm->remset_count >= vm->remset_cap) {
        size_t new_cap = vm->remset_cap < 64 ? 64 : vm->remset_cap * 2;
        Value *new_arr = (Value *)mini_word_grow(vm->remset, vm->remset_cap * sizeof(Value), new_cap * sizeof(Value));
        if (!new_arr) {
            vm_error(vm, "out of memory expanding remembered set");
            return;
        }
        vm->remset = new_arr;
        vm->remset_cap = new_cap;
    }
    vm->remset[vm->remset_count++] = parent;
}

/* ========================================================================= */
/* Minor GC (Eden Nursery Promotion to Tenured)                             */
/* ========================================================================= */

static Value promote_val(MiniVM *vm, Value val) {
    uintptr_t tag = val & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) {
        return val;
    }

    HeapObj *old_obj = (HeapObj *)UNTAG_PTR(val);
    if (!old_obj) return val;

    /* If object is not in Eden (it is in Tenured, or in ROM/Flash), leave untouched */
    if ((char *)old_obj < vm->eden_start || (char *)old_obj >= vm->eden_hp) {
        return val;
    }

    /* If already forwarded, return the forwarding target in Tenured */
    if ((old_obj->type & MINI_GC_TYPE_MASK) == OBJ_FORWARDED) {
        return ((ForwardedObj *)old_obj)->new_val;
    }

    size_t sz = ALIGN8(old_obj->size);
    HeapObj *new_obj = (HeapObj *)vm->tenured_hp;
    memcpy(new_obj, old_obj, sz);
    vm->tenured_hp += sz;

    /* Clear any GC flags on promoted object in Tenured */
    new_obj->type &= MINI_GC_TYPE_MASK;

    Value new_val = ((uintptr_t)new_obj) | tag;
    old_obj->type = OBJ_FORWARDED;
    ((ForwardedObj *)old_obj)->new_val = new_val;

    return new_val;
}

static void promote_object_fields(MiniVM *vm, HeapObj *obj) {
    uint32_t type = obj->type & MINI_GC_TYPE_MASK;
    switch (type) {
        case OBJ_PAIR: {
            PairObj *p = (PairObj *)obj;
            p->car = promote_val(vm, p->car);
            p->cdr = promote_val(vm, p->cdr);
            break;
        }
        case OBJ_CLOSURE: {
            ClosureObj *c = (ClosureObj *)obj;
            c->code = promote_val(vm, c->code);
            c->env  = promote_val(vm, c->env);
            break;
        }
        case OBJ_CODE: {
            CodeObj *co = (CodeObj *)obj;
            Value *c_arr = CODE_CONSTS(co);
            for (uint16_t i = 0; i < co->num_consts; i++) {
                c_arr[i] = promote_val(vm, c_arr[i]);
            }
            break;
        }
        case OBJ_VECTOR: {
            VectorObj *vec = (VectorObj *)obj;
            for (size_t i = 0; i < vec->length; i++) {
                vec->data[i] = promote_val(vm, vec->data[i]);
            }
            break;
        }
        case OBJ_CONTINUATION: {
            ContinuationObj *cont = (ContinuationObj *)obj;
            SavedFrame *sf = CONT_FRAMES(cont);
            for (uint32_t i = 0; i < cont->frame_count; i++) {
                if (sf[i].closure_val) sf[i].closure_val = promote_val(vm, sf[i].closure_val);
                if (sf[i].code_val)    sf[i].code_val    = promote_val(vm, sf[i].code_val);
                sf[i].env = promote_val(vm, sf[i].env);
            }
            Value *stk = CONT_STACK(cont);
            for (uint32_t i = 0; i < cont->stack_len; i++) {
                stk[i] = promote_val(vm, stk[i]);
            }
            break;
        }
        case OBJ_MACRO: {
            MacroObj *m = (MacroObj *)obj;
            m->proc_or_literals = promote_val(vm, m->proc_or_literals);
            m->rules            = promote_val(vm, m->rules);
            break;
        }
        default:
            break;
    }
}

void minor_gc(MiniVM *vm, size_t needed_bytes) {
    if (!vm) return;
    vm->stat_minor_gcs++;
    vm->gcache_gen++; /* objects move */

    size_t eden_used = (size_t)(vm->eden_hp - vm->eden_start);
    /* If Tenured space cannot fit Eden promotion, run Major GC which collects both */
    if (vm->tenured_hp + eden_used + needed_bytes > vm->tenured_limit) {
        major_gc(vm, eden_used + needed_bytes);
        return;
    }

    char *old_tenured_hp = vm->tenured_hp;

    /* Step 1: Promote roots */
    vm->global_env   = promote_val(vm, vm->global_env);
    vm->baseline_env = promote_val(vm, vm->baseline_env);
    vm->symbols      = promote_val(vm, vm->symbols);
    vm->sym_quote    = promote_val(vm, vm->sym_quote);
    vm->sym_if       = promote_val(vm, vm->sym_if);
    vm->sym_begin    = promote_val(vm, vm->sym_begin);
    vm->sym_lambda   = promote_val(vm, vm->sym_lambda);
    vm->sym_define   = promote_val(vm, vm->sym_define);
    vm->sym_set      = promote_val(vm, vm->sym_set);
    vm->sym_let      = promote_val(vm, vm->sym_let);
    vm->sym_letstar  = promote_val(vm, vm->sym_letstar);
    vm->sym_letrec   = promote_val(vm, vm->sym_letrec);

    vm->sym_define_syntax = promote_val(vm, vm->sym_define_syntax);
    vm->sym_syntax_rules  = promote_val(vm, vm->sym_syntax_rules);
    vm->sym_define_macro  = promote_val(vm, vm->sym_define_macro);
    vm->sym_ellipsis      = promote_val(vm, vm->sym_ellipsis);
    vm->sym_wildcard      = promote_val(vm, vm->sym_wildcard);

    for (Value *s = vm->stack; s < vm->sp; s++) {
        *s = promote_val(vm, *s);
    }

    for (size_t i = 0; i < vm->c_roots_count; i++) {
        if (vm->c_roots[i]) {
            *vm->c_roots[i] = promote_val(vm, *vm->c_roots[i]);
        }
    }

    for (int f = 0; f < vm->call_frame_count; f++) {
        CallFrame *frame = &vm->call_frames[f];
        if (frame->closure) {
            Value old_clo_val = ((uintptr_t)frame->closure) | MINI_TAG_CLOSURE;
            Value new_clo_val = promote_val(vm, old_clo_val);
            frame->closure = (ClosureObj *)UNTAG_PTR(new_clo_val);
        }
        if (frame->code && frame->pc) {
            CodeObj *old_code = frame->code;
            size_t pc_offset = (size_t)(frame->pc - (uint8_t *)old_code);
            Value old_code_val = ((uintptr_t)old_code) | MINI_TAG_CODE;
            Value new_code_val = promote_val(vm, old_code_val);
            frame->code = (CodeObj *)UNTAG_PTR(new_code_val);
            frame->pc = ((uint8_t *)frame->code) + pc_offset;
        } else if (frame->code) {
            Value old_code_val = ((uintptr_t)frame->code) | MINI_TAG_CODE;
            Value new_code_val = promote_val(vm, old_code_val);
            frame->code = (CodeObj *)UNTAG_PTR(new_code_val);
        }
        frame->env = promote_val(vm, frame->env);
    }

    /* Step 2: Scan and promote objects referenced by Remembered Set */
    for (size_t i = 0; i < vm->remset_count; i++) {
        Value v = vm->remset[i];
        HeapObj *obj = (HeapObj *)UNTAG_PTR(v);
        if (obj) {
            promote_object_fields(vm, obj);
            obj->type &= ~MINI_GC_REMSET_BIT;
        }
    }
    vm->remset_count = 0;

    /* Step 3: Scavenge newly promoted objects in Tenured */
    char *scan_ptr = old_tenured_hp;
    while (scan_ptr < vm->tenured_hp) {
        HeapObj *obj = (HeapObj *)scan_ptr;
        promote_object_fields(vm, obj);
        scan_ptr += ALIGN8(obj->size);
    }

    /* Step 4: Reset Eden generation to empty */
    vm->eden_hp = vm->eden_start;
    {
        size_t used = (size_t)(vm->tenured_hp - vm->tenured_start);
        if (used > vm->stat_live_peak) vm->stat_live_peak = used;
    }

    /* Update legacy compatibility pointers */
    vm->hp = vm->tenured_hp;
    vm->to_space = vm->tenured_start;
    vm->hp_limit = vm->tenured_limit;

    /* Step 5: Check if Tenured space requires compaction */
    size_t tenured_used = (size_t)(vm->tenured_hp - vm->tenured_start);
    if (tenured_used + needed_bytes + vm->eden_size > vm->tenured_size * 0.8) {
        major_gc(vm, needed_bytes);
    }
}

/* ========================================================================= */
/* Major GC (In-Place Sliding Mark-Compact in Tenured Generation)           */
/* ========================================================================= */

static void mark_push(MiniVM *vm, HeapObj *obj) {
    if (vm->mark_stack_count >= vm->mark_stack_cap) {
        size_t new_cap = vm->mark_stack_cap < 512 ? 512 : vm->mark_stack_cap * 2;
        HeapObj **new_stack = (HeapObj **)mini_word_grow(vm->mark_stack, vm->mark_stack_cap * sizeof(HeapObj *),
                                                         new_cap * sizeof(HeapObj *));
        if (!new_stack) {
            vm_error(vm, "out of memory allocating GC mark stack");
            return;
        }
        vm->mark_stack = new_stack;
        vm->mark_stack_cap = new_cap;
    }
    vm->mark_stack[vm->mark_stack_count++] = obj;
}

static void mark_val(MiniVM *vm, Value val) {
    uintptr_t tag = val & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) return;
    HeapObj *obj = (HeapObj *)UNTAG_PTR(val);
    if (!obj) return;
    /* Mark objects inside Tenured generation OR Eden generation */
    bool in_tenured = ((char *)obj >= vm->tenured_start && (char *)obj < vm->tenured_hp);
    bool in_eden    = ((char *)obj >= vm->eden_start && (char *)obj < vm->eden_hp);
    if (!in_tenured && !in_eden) return;
    if (obj->type & MINI_GC_MARK_BIT) return;
    obj->type |= MINI_GC_MARK_BIT;
    mark_push(vm, obj);
}

static inline Value fix_forwarded_val(const MiniVM *vm, const char *target_start, Value val) {
    uintptr_t tag = val & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) return val;
    HeapObj *obj = (HeapObj *)UNTAG_PTR(val);
    if (!obj) return val;
    bool in_tenured = ((char *)obj >= vm->tenured_start && (char *)obj < vm->tenured_hp);
    bool in_eden    = ((char *)obj >= vm->eden_start && (char *)obj < vm->eden_hp);
    if (!in_tenured && !in_eden) return val;
    if (!(obj->type & MINI_GC_MARK_BIT)) return val;
    uint32_t offset = ((obj->type >> MINI_GC_FWD_SHIFT) & MINI_GC_FWD_MASK) << 3;
    const char *new_addr = target_start + offset;
    return ((uintptr_t)new_addr) | tag;
}

static void fix_object_fields(MiniVM *vm, const char *target_start, HeapObj *obj) {
    uint32_t type = obj->type & MINI_GC_TYPE_MASK;
    switch (type) {
        case OBJ_PAIR: {
            PairObj *p = (PairObj *)obj;
            p->car = fix_forwarded_val(vm, target_start, p->car);
            p->cdr = fix_forwarded_val(vm, target_start, p->cdr);
            break;
        }
        case OBJ_CLOSURE: {
            ClosureObj *c = (ClosureObj *)obj;
            c->code = fix_forwarded_val(vm, target_start, c->code);
            c->env  = fix_forwarded_val(vm, target_start, c->env);
            break;
        }
        case OBJ_CODE: {
            CodeObj *co = (CodeObj *)obj;
            Value *c_arr = CODE_CONSTS(co);
            for (uint16_t i = 0; i < co->num_consts; i++) {
                c_arr[i] = fix_forwarded_val(vm, target_start, c_arr[i]);
            }
            break;
        }
        case OBJ_VECTOR: {
            VectorObj *vec = (VectorObj *)obj;
            for (size_t i = 0; i < vec->length; i++) {
                vec->data[i] = fix_forwarded_val(vm, target_start, vec->data[i]);
            }
            break;
        }
        case OBJ_CONTINUATION: {
            ContinuationObj *cont = (ContinuationObj *)obj;
            SavedFrame *sf = CONT_FRAMES(cont);
            for (uint32_t i = 0; i < cont->frame_count; i++) {
                if (sf[i].closure_val) sf[i].closure_val = fix_forwarded_val(vm, target_start, sf[i].closure_val);
                if (sf[i].code_val)    sf[i].code_val    = fix_forwarded_val(vm, target_start, sf[i].code_val);
                sf[i].env = fix_forwarded_val(vm, target_start, sf[i].env);
            }
            Value *stk = CONT_STACK(cont);
            for (uint32_t i = 0; i < cont->stack_len; i++) {
                stk[i] = fix_forwarded_val(vm, target_start, stk[i]);
            }
            break;
        }
        case OBJ_MACRO: {
            MacroObj *m = (MacroObj *)obj;
            m->proc_or_literals = fix_forwarded_val(vm, target_start, m->proc_or_literals);
            m->rules            = fix_forwarded_val(vm, target_start, m->rules);
            break;
        }
        default:
            break;
    }
}

void major_gc(MiniVM *vm, size_t needed_bytes) {
    if (!vm) return;
    vm->stat_major_gcs++;
    vm->gcache_gen++; /* objects move */

    size_t tenured_used = (size_t)(vm->tenured_hp - vm->tenured_start);
    size_t eden_used = (size_t)(vm->eden_hp - vm->eden_start);
    size_t total_used = tenured_used + eden_used;
    size_t target_size = vm->tenured_size;

    /* Check if Tenured space needs geometric expansion */
    if (total_used + needed_bytes + vm->eden_size > target_size * 0.75) {
        size_t exp_size = target_size * 2;
        while (total_used + needed_bytes + vm->eden_size > exp_size * 0.75) {
            exp_size *= 2;
        }
        if (vm->max_memory_bytes > 0 && exp_size > vm->max_memory_bytes) {
            exp_size = (target_size > vm->max_memory_bytes) ? target_size : vm->max_memory_bytes;
        }
        if (exp_size > target_size) {
            target_size = exp_size;
        }
    }

    char *target_raw = NULL;
    char *target_start = NULL;

    if (target_size != vm->tenured_size) {
        target_start = alloc_aligned8(target_size, &target_raw);
        if (!target_start) {
            free(target_raw);
            target_raw = vm->tenured_raw;
            target_start = vm->tenured_start;
            target_size = vm->tenured_size;
        }
    } else {
        target_start = vm->tenured_start;
        target_raw = vm->tenured_raw;
    }

    /* Phase 1: Mark reachable objects across roots into Tenured and Eden */
    vm->mark_stack_count = 0;

    mark_val(vm, vm->global_env);
    mark_val(vm, vm->baseline_env);
    mark_val(vm, vm->symbols);
    mark_val(vm, vm->sym_quote);
    mark_val(vm, vm->sym_if);
    mark_val(vm, vm->sym_begin);
    mark_val(vm, vm->sym_lambda);
    mark_val(vm, vm->sym_define);
    mark_val(vm, vm->sym_set);
    mark_val(vm, vm->sym_let);
    mark_val(vm, vm->sym_letstar);
    mark_val(vm, vm->sym_letrec);

    mark_val(vm, vm->sym_define_syntax);
    mark_val(vm, vm->sym_syntax_rules);
    mark_val(vm, vm->sym_define_macro);
    mark_val(vm, vm->sym_ellipsis);
    mark_val(vm, vm->sym_wildcard);

    for (Value *s = vm->stack; s < vm->sp; s++) {
        mark_val(vm, *s);
    }

    for (size_t i = 0; i < vm->c_roots_count; i++) {
        if (vm->c_roots[i]) {
            mark_val(vm, *vm->c_roots[i]);
        }
    }

    for (int f = 0; f < vm->call_frame_count; f++) {
        CallFrame *frame = &vm->call_frames[f];
        if (frame->closure) mark_val(vm, ((uintptr_t)frame->closure) | MINI_TAG_CLOSURE);
        if (frame->code)    mark_val(vm, ((uintptr_t)frame->code) | MINI_TAG_CODE);
        mark_val(vm, frame->env);
    }

    while (vm->mark_stack_count > 0) {
        HeapObj *obj = vm->mark_stack[--vm->mark_stack_count];
        uint32_t type = obj->type & MINI_GC_TYPE_MASK;
        switch (type) {
            case OBJ_PAIR: {
                PairObj *p = (PairObj *)obj;
                while (p) {
                    mark_val(vm, p->car);
                    Value cdr_val = p->cdr;
                    if (mini_is_pair(cdr_val)) {
                        HeapObj *cdr_obj = (HeapObj *)UNTAG_PTR(cdr_val);
                        bool in_tenured = ((char *)cdr_obj >= vm->tenured_start && (char *)cdr_obj < vm->tenured_hp);
                        bool in_eden    = ((char *)cdr_obj >= vm->eden_start && (char *)cdr_obj < vm->eden_hp);
                        if ((in_tenured || in_eden) && !(cdr_obj->type & MINI_GC_MARK_BIT)) {
                            cdr_obj->type |= MINI_GC_MARK_BIT;
                            p = (PairObj *)cdr_obj;
                            continue;
                        }
                    }
                    mark_val(vm, cdr_val);
                    break;
                }
                break;
            }
            case OBJ_CLOSURE: {
                ClosureObj *c = (ClosureObj *)obj;
                mark_val(vm, c->code);
                mark_val(vm, c->env);
                break;
            }
            case OBJ_CODE: {
                CodeObj *co = (CodeObj *)obj;
                Value *c_arr = CODE_CONSTS(co);
                for (uint16_t i = 0; i < co->num_consts; i++) {
                    mark_val(vm, c_arr[i]);
                }
                break;
            }
            case OBJ_VECTOR: {
                VectorObj *vec = (VectorObj *)obj;
                for (size_t i = 0; i < vec->length; i++) {
                    mark_val(vm, vec->data[i]);
                }
                break;
            }
            case OBJ_CONTINUATION: {
                ContinuationObj *cont = (ContinuationObj *)obj;
                SavedFrame *sf = CONT_FRAMES(cont);
                for (uint32_t i = 0; i < cont->frame_count; i++) {
                    if (sf[i].closure_val) mark_val(vm, sf[i].closure_val);
                    if (sf[i].code_val)    mark_val(vm, sf[i].code_val);
                    mark_val(vm, sf[i].env);
                }
                Value *stk = CONT_STACK(cont);
                for (uint32_t i = 0; i < cont->stack_len; i++) {
                    mark_val(vm, stk[i]);
                }
                break;
            }
            case OBJ_MACRO: {
                MacroObj *m = (MacroObj *)obj;
                mark_val(vm, m->proc_or_literals);
                mark_val(vm, m->rules);
                break;
            }
            default:
                break;
        }
    }

    /* Phase 2: Compute Forwarding Offsets for surviving Tenured and Eden objects */
    char *free_ptr = target_start;

    bool oom = false;
    char *target_end = target_start + target_size;

    char *scan = vm->tenured_start;
    while (scan < vm->tenured_hp) {
        HeapObj *obj = (HeapObj *)scan;
        size_t sz = ALIGN8(obj->size);
        if (obj->type & MINI_GC_MARK_BIT) {
            if (free_ptr + sz > target_end) {
                oom = true;
                break;
            }
            uint32_t offset = (uint32_t)(free_ptr - target_start);
            obj->type = (obj->type & (MINI_GC_TYPE_MASK | MINI_GC_MARK_BIT))
                      | (((offset >> 3) & MINI_GC_FWD_MASK) << MINI_GC_FWD_SHIFT);
            free_ptr += sz;
        }
        scan += sz;
    }

    if (!oom) {
        scan = vm->eden_start;
        while (scan < vm->eden_hp) {
            HeapObj *obj = (HeapObj *)scan;
            size_t sz = ALIGN8(obj->size);
            if (obj->type & MINI_GC_MARK_BIT) {
                if (free_ptr + sz > target_end) {
                    oom = true;
                    break;
                }
                uint32_t offset = (uint32_t)(free_ptr - target_start);
                obj->type = (obj->type & (MINI_GC_TYPE_MASK | MINI_GC_MARK_BIT))
                          | (((offset >> 3) & MINI_GC_FWD_MASK) << MINI_GC_FWD_SHIFT);
                free_ptr += sz;
            }
            scan += sz;
        }
    }

    if (oom) {
        scan = vm->tenured_start;
        while (scan < vm->tenured_hp) {
            HeapObj *obj = (HeapObj *)scan;
            obj->type &= MINI_GC_TYPE_MASK;
            scan += ALIGN8(obj->size);
        }
        scan = vm->eden_start;
        while (scan < vm->eden_hp) {
            HeapObj *obj = (HeapObj *)scan;
            obj->type &= MINI_GC_TYPE_MASK;
            scan += ALIGN8(obj->size);
        }
        if (target_start != vm->tenured_start) {
            free(target_raw);
        }
        vm_error(vm, "out of memory: tenured heap exhausted during major GC");
        return;
    }

    /* Phase 3: Update References throughout the VM */
    vm->global_env   = fix_forwarded_val(vm, target_start, vm->global_env);
    vm->baseline_env = fix_forwarded_val(vm, target_start, vm->baseline_env);
    vm->symbols      = fix_forwarded_val(vm, target_start, vm->symbols);
    vm->sym_quote    = fix_forwarded_val(vm, target_start, vm->sym_quote);
    vm->sym_if       = fix_forwarded_val(vm, target_start, vm->sym_if);
    vm->sym_begin    = fix_forwarded_val(vm, target_start, vm->sym_begin);
    vm->sym_lambda   = fix_forwarded_val(vm, target_start, vm->sym_lambda);
    vm->sym_define   = fix_forwarded_val(vm, target_start, vm->sym_define);
    vm->sym_set      = fix_forwarded_val(vm, target_start, vm->sym_set);
    vm->sym_let      = fix_forwarded_val(vm, target_start, vm->sym_let);
    vm->sym_letstar  = fix_forwarded_val(vm, target_start, vm->sym_letstar);
    vm->sym_letrec   = fix_forwarded_val(vm, target_start, vm->sym_letrec);

    vm->sym_define_syntax = fix_forwarded_val(vm, target_start, vm->sym_define_syntax);
    vm->sym_syntax_rules  = fix_forwarded_val(vm, target_start, vm->sym_syntax_rules);
    vm->sym_define_macro  = fix_forwarded_val(vm, target_start, vm->sym_define_macro);
    vm->sym_ellipsis      = fix_forwarded_val(vm, target_start, vm->sym_ellipsis);
    vm->sym_wildcard      = fix_forwarded_val(vm, target_start, vm->sym_wildcard);

    for (Value *s = vm->stack; s < vm->sp; s++) {
        *s = fix_forwarded_val(vm, target_start, *s);
    }

    for (size_t i = 0; i < vm->c_roots_count; i++) {
        if (vm->c_roots[i]) {
            *vm->c_roots[i] = fix_forwarded_val(vm, target_start, *vm->c_roots[i]);
        }
    }

    for (int f = 0; f < vm->call_frame_count; f++) {
        CallFrame *frame = &vm->call_frames[f];
        if (frame->closure) {
            Value clo = ((uintptr_t)frame->closure) | MINI_TAG_CLOSURE;
            frame->closure = (ClosureObj *)UNTAG_PTR(fix_forwarded_val(vm, target_start, clo));
        }
        if (frame->code && frame->pc) {
            CodeObj *old_code = frame->code;
            size_t pc_off = (size_t)(frame->pc - (uint8_t *)old_code);
            Value cd = ((uintptr_t)old_code) | MINI_TAG_CODE;
            frame->code = (CodeObj *)UNTAG_PTR(fix_forwarded_val(vm, target_start, cd));
            frame->pc = ((uint8_t *)frame->code) + pc_off;
        } else if (frame->code) {
            Value cd = ((uintptr_t)frame->code) | MINI_TAG_CODE;
            frame->code = (CodeObj *)UNTAG_PTR(fix_forwarded_val(vm, target_start, cd));
        }
        frame->env = fix_forwarded_val(vm, target_start, frame->env);
    }

    scan = vm->tenured_start;
    while (scan < vm->tenured_hp) {
        HeapObj *obj = (HeapObj *)scan;
        size_t sz = ALIGN8(obj->size);
        if (obj->type & MINI_GC_MARK_BIT) {
            fix_object_fields(vm, target_start, obj);
        }
        scan += sz;
    }

    scan = vm->eden_start;
    while (scan < vm->eden_hp) {
        HeapObj *obj = (HeapObj *)scan;
        size_t sz = ALIGN8(obj->size);
        if (obj->type & MINI_GC_MARK_BIT) {
            fix_object_fields(vm, target_start, obj);
        }
        scan += sz;
    }

    /* Phase 4: Slide or Copy live objects into target_start */
    free_ptr = target_start;

    /* Move surviving objects from Tenured */
    scan = vm->tenured_start;
    while (scan < vm->tenured_hp) {
        HeapObj *obj = (HeapObj *)scan;
        size_t sz = ALIGN8(obj->size);
        if (obj->type & MINI_GC_MARK_BIT) {
            obj->type &= MINI_GC_TYPE_MASK;
            if (target_start != vm->tenured_start) {
                memcpy(free_ptr, scan, sz);
            } else if (free_ptr != scan) {
                memmove(free_ptr, scan, sz);
            }
            free_ptr += sz;
        }
        scan += sz;
    }

    /* Move surviving objects from Eden (appended into Tenured) */
    scan = vm->eden_start;
    while (scan < vm->eden_hp) {
        HeapObj *obj = (HeapObj *)scan;
        size_t sz = ALIGN8(obj->size);
        if (obj->type & MINI_GC_MARK_BIT) {
            obj->type &= MINI_GC_TYPE_MASK;
            memcpy(free_ptr, scan, sz);
            free_ptr += sz;
        }
        scan += sz;
    }

    if (target_start != vm->tenured_start) {
        free(vm->tenured_raw);
        vm->tenured_raw = target_raw;
        vm->tenured_start = target_start;
        vm->tenured_size = target_size;
    }

    vm->tenured_hp = free_ptr;
    vm->tenured_limit = vm->tenured_start + vm->tenured_size;
    vm->heap_size = vm->tenured_size;

    /* Reset Eden generation to empty */
    vm->eden_hp = vm->eden_start;

    /* Clear Remembered Set */
    for (size_t i = 0; i < vm->remset_count; i++) {
        HeapObj *obj = (HeapObj *)UNTAG_PTR(vm->remset[i]);
        if (obj) obj->type &= ~MINI_GC_REMSET_BIT;
    }
    vm->remset_count = 0;

    /* Legacy compatibility pointers */
    vm->hp = vm->tenured_hp;
    vm->to_space = vm->tenured_start;
    vm->to_space_raw = vm->tenured_raw;
    vm->hp_limit = vm->tenured_limit;

    vm->stat_live_bytes = (size_t)(vm->tenured_hp - vm->tenured_start);
    if (vm->stat_live_bytes > vm->stat_live_peak) vm->stat_live_peak = vm->stat_live_bytes;
}

void mini_get_stats(const MiniVM *vm, MiniStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!vm) return;
    out->alloc_bytes = vm->stat_alloc_bytes;
    out->minor_gcs   = vm->stat_minor_gcs;
    out->major_gcs   = vm->stat_major_gcs;
    out->live_bytes  = vm->stat_live_bytes;
    out->live_peak   = vm->stat_live_peak;
    out->frame_peak  = vm->stat_frame_peak;
    out->stack_peak  = vm->stat_stack_peak;
    out->heap_bytes  = vm->tenured_size;
    out->eden_bytes  = vm->eden_size;
    out->stack_slots = vm->stack_cap;
    out->frame_slots = vm->frame_cap;
    out->rom_bytes   = mini_rom_size(vm);
}

void cheney_gc(MiniVM *vm, size_t needed_bytes) {
    minor_gc(vm, needed_bytes);
    if (vm->tenured_hp + needed_bytes > vm->tenured_limit) {
        major_gc(vm, needed_bytes);
    }
}

void *gc_alloc(MiniVM *vm, size_t size, MiniObjType type) {
    size_t sz = ALIGN8(size);
    if (sz == 0) sz = 8;
    vm->stat_alloc_bytes += sz;
#ifdef MINI_ALLOC_PROFILE
    mini_prof_alloc(sz, (unsigned)type);
#endif

    /* Fast path: bump allocation in Eden */
    if (vm->eden_hp + sz > vm->eden_limit) {
        minor_gc(vm, sz);
        /* If Eden is still too small (sz > eden_size), allocate directly in Tenured */
        if (vm->eden_hp + sz > vm->eden_limit) {
            if (vm->tenured_hp + sz > vm->tenured_limit) {
                major_gc(vm, sz);
            }
            if (vm->tenured_hp + sz <= vm->tenured_limit) {
                HeapObj *obj = (HeapObj *)vm->tenured_hp;
                vm->tenured_hp += sz;
                obj->type = (uint32_t)type;
                obj->size = (uint32_t)sz;
                return (void *)obj;
            }
            vm_error(vm, "out of memory: heap exhausted (%zu bytes requested)", sz);
            return NULL;
        }
    }

    HeapObj *obj = (HeapObj *)vm->eden_hp;
    vm->eden_hp += sz;
    obj->type = (uint32_t)type;
    obj->size = (uint32_t)sz;
    return (void *)obj;
}

void mini_gc(MiniVM *vm) {
    minor_gc(vm, 0);
    major_gc(vm, 0);
}

size_t mini_get_heap_size(const MiniVM *vm) {
    return vm ? vm->tenured_size : 0;
}

size_t mini_get_allocated_bytes(const MiniVM *vm) {
    if (!vm) return 0;
    size_t eden_used = (size_t)(vm->eden_hp - vm->eden_start);
    size_t tenured_used = (size_t)(vm->tenured_hp - vm->tenured_start);
    return eden_used + tenured_used;
}

void mini_set_memory_limit(MiniVM *vm, size_t max_bytes) {
    vm->max_memory_bytes = max_bytes;
}

void mini_push_root(MiniVM *vm, Value *root_ptr) {
    if (vm->c_roots_count >= vm->c_roots_cap) {
        size_t new_cap = vm->c_roots_cap < 32 ? 32 : vm->c_roots_cap * 2;
        Value **new_roots = (Value **)realloc(vm->c_roots, new_cap * sizeof(Value *));
        if (!new_roots) {
            vm_error(vm, "out of memory expanding C root table");
            return;
        }
        vm->c_roots = new_roots;
        vm->c_roots_cap = new_cap;
    }
    vm->c_roots[vm->c_roots_count++] = root_ptr;
}

void mini_pop_root(MiniVM *vm) {
    if (vm->c_roots_count > 0) {
        vm->c_roots_count--;
    }
}

/* ========================================================================= */
/* Heap Object Constructors & Accessors                                      */
/* ========================================================================= */

Value mini_make_cons(MiniVM *vm, Value car, Value cdr) {
    mini_push_root(vm, &car);
    mini_push_root(vm, &cdr);
    PairObj *p = (PairObj *)gc_alloc(vm, sizeof(PairObj), OBJ_PAIR);
    mini_pop_root(vm);
    mini_pop_root(vm);
    if (!p) return mini_void();
    p->car = car;
    p->cdr = cdr;
    Value res = ((uintptr_t)p) | MINI_TAG_PAIR;
    mini_write_barrier(vm, res, car);
    mini_write_barrier(vm, res, cdr);
    return res;
}

/* Bytes inside the heap (a string's data) move when an allocation collects,
 * and the old copy's first bytes then hold the forwarding header. When an
 * allocation of sz bytes may collect and src lies in the heap, src is copied
 * to malloc'd memory first (*copy: free it after the allocation). NULL when
 * that copy fails. */
static const char *gc_stable_bytes(MiniVM *vm, const char *src, size_t len, size_t sz, char **copy) {
    *copy = NULL;
    if (!src || len == 0 || vm->eden_hp + ALIGN8(sz) <= vm->eden_limit) return src;
    bool in_eden = src >= vm->eden_start && src < vm->eden_limit;
    bool in_tenured = src >= vm->tenured_start && src < vm->tenured_limit;
    if (!in_eden && !in_tenured) return src;
    *copy = (char *)malloc(len);
    if (!*copy) {
        vm_error(vm, "out of memory copying %zu bytes", len);
        return NULL;
    }
    memcpy(*copy, src, len);
    return *copy;
}

Value mini_make_symbol(MiniVM *vm, const char *name) {
    size_t len = strlen(name);
    if (vm->xrom_sym_count) {
        Value rs = xrom_find_symbol(vm, name, len);
        if (rs) return rs;
    }
    Value curr = vm->symbols;
    while (mini_is_pair(curr)) {
        Value sym = mini_car(curr);
        if (mini_is_symbol(sym)) {
            SymbolObj *s = (SymbolObj *)UNTAG_PTR(sym);
            if (s->len == len && memcmp(s->name, name, len) == 0) {
                return sym;
            }
        }
        curr = mini_cdr(curr);
    }

    /* Reuse interned symbol from read-only ROM / Flash table if present */
    if (vm->rom_base) {
        uint64_t s_curr = vm->rom_symbols_offset & ~(uintptr_t)MINI_TAG_MASK;
        while (s_curr < vm->rom_size) {
            const PairObj *spine = (const PairObj *)(vm->rom_base + s_curr);
            if (spine->header.type != OBJ_PAIR) break;
            uint64_t sym_val = spine->car;
            if ((sym_val & MINI_TAG_MASK) == MINI_TAG_SYMBOL) {
                uint64_t s_off = sym_val & ~(uintptr_t)MINI_TAG_MASK;
                if (s_off < vm->rom_size) {
                    const SymbolObj *s = (const SymbolObj *)(vm->rom_base + s_off);
                    if (s->len == len && memcmp(s->name, name, len) == 0) {
                        Value rom_sym = ((uintptr_t)s) | MINI_TAG_SYMBOL;
                        mini_push_root(vm, &rom_sym);
                        vm->symbols = mini_make_cons(vm, rom_sym, vm->symbols);
                        mini_pop_root(vm);
                        return rom_sym;
                    }
                }
            }
            uint64_t next = spine->cdr;
            if ((next & MINI_TAG_MASK) == MINI_TAG_PAIR) {
                s_curr = next & ~(uintptr_t)MINI_TAG_MASK;
            } else {
                break;
            }
        }
    }

    size_t sz = sizeof(SymbolObj) + len;
    char *copy;
    name = gc_stable_bytes(vm, name, len, sz, &copy);
    if (!name) return mini_void();
    SymbolObj *s = (SymbolObj *)gc_alloc(vm, sz, OBJ_SYMBOL);
    if (!s) {
        free(copy);
        return mini_void();
    }
    s->len = len;
    memcpy(s->name, name, len);
    s->name[len] = '\0';
    free(copy);
    Value sym = ((uintptr_t)s) | MINI_TAG_SYMBOL;

    mini_push_root(vm, &sym);
    vm->symbols = mini_make_cons(vm, sym, vm->symbols);
    mini_pop_root(vm);

    return sym;
}

Value mini_make_string(MiniVM *vm, const char *str) {
    return mini_make_string_len(vm, str, str ? strlen(str) : 0);
}

Value mini_make_string_len(MiniVM *vm, const char *str, size_t len) {
    size_t sz = sizeof(StringObj) + len;
    char *copy;
    if (str && len > 0) {
        str = gc_stable_bytes(vm, str, len, sz, &copy);   /* (substring s ..): s may move */
        if (!str) return mini_void();
    } else {
        copy = NULL;
    }
    StringObj *s = (StringObj *)gc_alloc(vm, sz, OBJ_STRING);
    if (!s) {
        free(copy);
        return mini_void();
    }
    s->len = len;
    if (str && len > 0) {
        memcpy(s->data, str, len);
    }
    s->data[len] = '\0';
    free(copy);
    return ((uintptr_t)s) | MINI_TAG_STRING;
}

Value mini_make_flonum(MiniVM *vm, double num) {
    FlonumObj *f = (FlonumObj *)gc_alloc(vm, sizeof(FlonumObj), OBJ_FLONUM);
    if (!f) return mini_void();
    f->val = num;
    return ((uintptr_t)f) | MINI_TAG_RECORD;
}

double mini_to_flonum(Value v) {
    if (mini_is_flonum(v)) {
        return ((FlonumObj *)UNTAG_PTR(v))->val;
    }
    return 0.0;
}

Value mini_make_vector(MiniVM *vm, size_t length, Value fill) {
    mini_push_root(vm, &fill);
    size_t sz = sizeof(VectorObj) + (length > 1 ? (length - 1) * sizeof(Value) : 0);
    VectorObj *vec = (VectorObj *)gc_alloc(vm, sz, OBJ_VECTOR);
    mini_pop_root(vm);
    if (!vec) return mini_void();
    vec->length = length;
    for (size_t i = 0; i < length; i++) {
        vec->data[i] = fill;
    }
    return ((uintptr_t)vec) | MINI_TAG_RECORD;
}

size_t mini_vector_length(Value v) {
    if (!mini_is_vector(v)) return 0;
    return ((VectorObj *)UNTAG_PTR(v))->length;
}

Value mini_vector_ref(MiniVM *vm, Value v, size_t idx) {
    if (!mini_is_vector(v)) {
        vm_error(vm, "vector-ref: expected vector");
        return mini_void();
    }
    VectorObj *vec = (VectorObj *)UNTAG_PTR(v);
    if (idx >= vec->length) {
        vm_error(vm, "vector-ref: index %zu out of bounds (length %zu)", idx, vec->length);
        return mini_void();
    }
    return vec->data[idx];
}

void mini_vector_set(MiniVM *vm, Value v, size_t idx, Value val) {
    if (!mini_is_vector(v)) {
        vm_error(vm, "vector-set!: expected vector");
        return;
    }
    VectorObj *vec = (VectorObj *)UNTAG_PTR(v);
    if (idx >= vec->length) {
        vm_error(vm, "vector-set!: index %zu out of bounds (length %zu)", idx, vec->length);
        return;
    }
    vec->data[idx] = val;
    mini_write_barrier(vm, v, val);
}

Value mini_make_code(MiniVM *vm, uint32_t code_len, const uint8_t *code_bytes,
                     uint16_t num_params, uint16_t max_stack, uint8_t is_variadic,
                     uint8_t has_inner_lambdas, uint16_t num_consts, const Value *consts) {
    size_t sz = sizeof(CodeObj) + num_consts * sizeof(Value) + code_len;

    /* Callers pass consts in a C array or a heap vector; neither is a GC root.
     * Park them on the VM stack (scanned and updated by both collectors) so
     * an allocation-triggered GC cannot leave stale pointers behind. */
    Value *saved = NULL;
    bool saved_on_stack = false;
    if (consts && num_consts > 0) {
        if (vm->sp + num_consts < vm->stack + vm->stack_cap) {
            saved = vm->sp;
            memcpy(saved, consts, num_consts * sizeof(Value));
            vm->sp += num_consts;
            saved_on_stack = true;
        } else {
            saved = (Value *)malloc(num_consts * sizeof(Value));
            if (!saved) {
                vm_error(vm, "out of memory staging code constants");
                return mini_void();
            }
            memcpy(saved, consts, num_consts * sizeof(Value));
            for (uint16_t i = 0; i < num_consts; i++) mini_push_root(vm, &saved[i]);
        }
        consts = saved;
    }

    CodeObj *co = (CodeObj *)gc_alloc(vm, sz, OBJ_CODE);
    if (!co) {
        if (saved_on_stack) vm->sp = saved;
        else if (saved) { for (uint16_t i = 0; i < num_consts; i++) mini_pop_root(vm); free(saved); }
        return mini_void();
    }
    co->code_len = code_len;
    co->num_params = num_params;
    co->max_stack = max_stack;
    co->is_variadic = is_variadic;
    co->has_inner_lambdas = has_inner_lambdas;
    co->num_consts = num_consts;

    Value *c_dest = CODE_CONSTS(co);
    Value res = ((uintptr_t)co) | MINI_TAG_RECORD;
    if (consts && num_consts > 0) {
        memcpy(c_dest, consts, num_consts * sizeof(Value));
        for (uint16_t i = 0; i < num_consts; i++) {
            mini_write_barrier(vm, res, consts[i]);
        }
        if (saved_on_stack) {
            vm->sp = saved;
        } else {
            for (uint16_t i = 0; i < num_consts; i++) mini_pop_root(vm);
            free(saved);
        }
    }
    uint8_t *b_dest = CODE_BYTES(co);
    if (code_bytes && code_len > 0) {
        memcpy(b_dest, code_bytes, code_len);
    }
    return res;
}

Value mini_make_closure(MiniVM *vm, Value code, Value env) {
    mini_push_root(vm, &code);
    mini_push_root(vm, &env);
    ClosureObj *c = (ClosureObj *)gc_alloc(vm, sizeof(ClosureObj), OBJ_CLOSURE);
    mini_pop_root(vm);
    mini_pop_root(vm);
    if (!c) return mini_void();
    c->code = code;
    c->env = env;
    Value res = ((uintptr_t)c) | MINI_TAG_CLOSURE;
    mini_write_barrier(vm, res, code);
    mini_write_barrier(vm, res, env);
    return res;
}

Value mini_make_continuation(MiniVM *vm, uint32_t frame_count, const CallFrame *frames,
                             uint32_t stack_len, const Value *stack_vals, uint32_t base_frame) {
    size_t frames_sz = frame_count * sizeof(SavedFrame);
    size_t stack_sz = stack_len * sizeof(Value);
    size_t total_sz = sizeof(ContinuationObj) + frames_sz + stack_sz;

    ContinuationObj *cont = (ContinuationObj *)gc_alloc(vm, total_sz, OBJ_CONTINUATION);
    if (!cont) return mini_void();
    cont->frame_count = frame_count;
    cont->stack_len = stack_len;
    cont->base_frame = base_frame;
    cont->reserved = 0;

    SavedFrame *sf = CONT_FRAMES(cont);
    for (uint32_t i = 0; i < frame_count; i++) {
        sf[i].closure_val = frames[i].closure ? (((uintptr_t)frames[i].closure) | MINI_TAG_CLOSURE) : 0;
        sf[i].code_val = frames[i].code ? (((uintptr_t)frames[i].code) | MINI_TAG_CODE) : 0;
        sf[i].pc_offset = (frames[i].code && frames[i].pc) ? (uint32_t)(frames[i].pc - CODE_BYTES(frames[i].code)) : 0;
        sf[i].fp_offset = (uint32_t)(frames[i].fp - vm->stack);
        sf[i].env = frames[i].env;
#if UINTPTR_MAX == 0xffffffff
        sf[i].pad = 0;
#endif
    }

    Value *stk = CONT_STACK(cont);
    if (stack_vals && stack_len > 0) {
        mini_words_copy(stk, stack_vals, stack_len * sizeof(Value));   /* from the VM stack (IRAM) */
    }
    return ((uintptr_t)cont) | MINI_TAG_RECORD;
}

Value mini_make_macro(MiniVM *vm, uint32_t is_syntax_rules, Value proc_or_literals, Value rules) {
    mini_push_root(vm, &proc_or_literals);
    mini_push_root(vm, &rules);
    MacroObj *m = (MacroObj *)gc_alloc(vm, sizeof(MacroObj), OBJ_MACRO);
    mini_pop_root(vm);
    mini_pop_root(vm);
    if (!m) return mini_void();
    m->is_syntax_rules = is_syntax_rules;
#if UINTPTR_MAX == 0xffffffff
    m->pad = 0;
#endif
    m->proc_or_literals = proc_or_literals;
    m->rules = rules;
    return ((uintptr_t)m) | MINI_TAG_RECORD;
}

/* mini_car / mini_cdr are inline in minischeme.h on MiniPairCells: the same layout as PairObj */
typedef char mini_pair_cells_car_check[offsetof(PairObj, car) == offsetof(MiniPairCells, car) ? 1 : -1];
typedef char mini_pair_cells_cdr_check[offsetof(PairObj, cdr) == offsetof(MiniPairCells, cdr) ? 1 : -1];

void mini_set_car(Value pair, Value val) {
    if (mini_is_pair(pair)) {
        ((PairObj *)UNTAG_PTR(pair))->car = val;
        if (s_active_vm) {
            mini_write_barrier(s_active_vm, pair, val);
        }
    }
}

void mini_set_cdr(Value pair, Value val) {
    if (mini_is_pair(pair)) {
        ((PairObj *)UNTAG_PTR(pair))->cdr = val;
        if (s_active_vm) {
            mini_write_barrier(s_active_vm, pair, val);
        }
    }
}

const char *mini_symbol_name(Value sym) {
    if (!mini_is_symbol(sym)) return "";
    return ((SymbolObj *)UNTAG_PTR(sym))->name;
}

const char *mini_string_data(Value str) {
    if (!mini_is_string(str)) return "";
    return ((StringObj *)UNTAG_PTR(str))->data;
}

size_t mini_string_len(Value str) {
    if (!mini_is_string(str)) return 0;
    return ((StringObj *)UNTAG_PTR(str))->len;
}

bool mini_eq(Value a, Value b) {
    return a == b;
}

bool mini_equal(Value a, Value b) {
    if (a == b) return true;
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) return a == b;
    if (mini_is_integer(a) && mini_is_integer(b)) {
        return bigint_cmp(a, b) == 0;
    }
    if (mini_is_flonum(a) && mini_is_flonum(b)) {
        return mini_to_flonum(a) == mini_to_flonum(b);
    }
    if (mini_is_string(a) && mini_is_string(b)) {
        size_t la = mini_string_len(a);
        size_t lb = mini_string_len(b);
        if (la != lb) return false;
        return memcmp(mini_string_data(a), mini_string_data(b), la) == 0;
    }
    if (mini_is_pair(a) && mini_is_pair(b)) {
        return mini_equal(mini_car(a), mini_car(b)) &&
               mini_equal(mini_cdr(a), mini_cdr(b));
    }
    if (mini_is_vector(a) && mini_is_vector(b)) {
        VectorObj *va = (VectorObj *)UNTAG_PTR(a);
        VectorObj *vb = (VectorObj *)UNTAG_PTR(b);
        if (va->length != vb->length) return false;
        for (size_t i = 0; i < va->length; i++) {
            if (!mini_equal(va->data[i], vb->data[i])) return false;
        }
        return true;
    }
    return false;
}
