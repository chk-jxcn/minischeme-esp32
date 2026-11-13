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
#include "esp_timer.h"
#define MINI_GC_NOW_US() ((uint64_t)esp_timer_get_time())
#endif

/* ========================================================================= */
/* Memory Allocation & Cheney Copying GC                                     */
/* ========================================================================= */

/* ========================================================================= */
/* Word-only memory (see minischeme_internal.h)                              */
/* ========================================================================= */

static size_t s_word_iram = 0;

void *mini_word_alloc(size_t bytes) {
#if defined(ESP_PLATFORM) && defined(MALLOC_CAP_EXEC)   /* a chip with a word-only IRAM heap (ESP32) */
    /* EXEC|32BIT prefers the IRAM-only region; past it the allocator would
     * carve DRAM through its IRAM alias, which saves nothing */
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (p && esp_ptr_in_iram(p) && !esp_ptr_in_diram_iram(p)) {
        s_word_iram += bytes;
        return p;
    }
    if (p) heap_caps_free(p);
#elif defined(ESP_PLATFORM)
    /* no word-only heap (ESP32-S3): internal RAM, not PSRAM, for the stack and the frames */
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(bytes);
}

void mini_words_copy(void *dst, const void *src, size_t bytes) {
#if defined(ESP_PLATFORM) && defined(MALLOC_CAP_EXEC)   /* a chip with a word-only IRAM heap (ESP32) */
    /* volatile: the compiler must not turn the loop back into memcpy */
    volatile uint32_t *d = (volatile uint32_t *)dst;
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    for (size_t i = 0; i < bytes / 4; i++) d[i] = s[i];
#else
    if (bytes > 0) memcpy(dst, src, bytes);
#endif
}

void mini_words_fill(void *dst, uint32_t value, size_t bytes) {
#if defined(ESP_PLATFORM) && defined(MALLOC_CAP_EXEC)   /* a chip with a word-only IRAM heap (ESP32) */
    volatile uint32_t *d = (volatile uint32_t *)dst;
    for (size_t i = 0; i < bytes / 4; i++) d[i] = value;
#else
    if (bytes == 0) return;
    if (value == 0) {
        memset(dst, 0, bytes);
    } else {
        uint32_t *d = (uint32_t *)dst;
        for (size_t i = 0; i < bytes / 4; i++) d[i] = value;
    }
#endif
}

void *mini_word_grow(void *old, size_t old_bytes, size_t new_bytes) {
    void *p = mini_word_alloc(new_bytes);
    if (!p) return NULL;
    if (old) {
        mini_words_copy(p, old, old_bytes < new_bytes ? old_bytes : new_bytes);
#if defined(ESP_PLATFORM) && defined(MALLOC_CAP_EXEC)   /* a chip with a word-only IRAM heap (ESP32) */
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
    fn(vm->gslots, "vm global slots", user);
    fn(vm->prim_fns, "vm api procedures", user);
}

char *alloc_aligned8(size_t size, char **raw_ptr) {
#if defined(ESP_PLATFORM) && defined(MALLOC_CAP_EXEC)   /* a chip with a word-only IRAM heap (ESP32) */
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
#elif defined(ESP_PLATFORM)
    /* with PSRAM: the heap (large) goes there unless the build keeps it internal
     * (VM_HEAP_INTERNAL); the eden and other small blocks stay in internal RAM */
    char *raw = NULL;
#if CONFIG_SPIRAM && !VM_HEAP_INTERNAL
    if (size >= 16 * 1024) raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!raw) raw = (char *)heap_caps_malloc(size + 8, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw) raw = (char *)malloc(size + 8);
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
/* Mark stack (major GC marking; the promotion worklist with mark-sweep)     */
/* ========================================================================= */

#ifndef MINI_GC_NOW_US
#define MINI_GC_NOW_US() ((uint64_t)0)       /* no clock: the pause statistics stay 0 */
#endif

static inline void gc_pause_done(MiniVM *vm, uint64_t t0) {
    uint64_t d = MINI_GC_NOW_US() - t0;
    vm->stat_gc_total_us += d;
    if (d > vm->stat_gc_max_us) vm->stat_gc_max_us = d > 0xffffffffu ? 0xffffffffu : (uint32_t)d;
}

void mini_gc_stats_reset(MiniVM *vm) {
    if (vm) vm->stat_gc_max_us = 0;
}

#ifdef MINI_GC_VERIFY
static void gc_verify(MiniVM *vm, const char *when);
#define GC_VERIFY(vm, when) gc_verify(vm, when)
#else
#define GC_VERIFY(vm, when) ((void)0)
#endif

#ifndef MINI_MARK_STACK_MAX
#define MINI_MARK_STACK_MAX (1u << 20)   /* entries; past it marking rescans the heap instead */
#endif

static bool mark_stack_grow(MiniVM *vm, size_t want) {
    if (want <= vm->mark_stack_cap) return true;
    size_t new_cap = vm->mark_stack_cap < 512 ? 512 : vm->mark_stack_cap * 2;
    while (new_cap < want) new_cap *= 2;
    HeapObj **new_stack;
#if defined(ESP_PLATFORM) && !defined(MALLOC_CAP_EXEC)
    /* no word-only heap: malloc, so that a large stack lands in PSRAM when there is some */
    new_stack = (HeapObj **)malloc(new_cap * sizeof(HeapObj *));
    if (new_stack && vm->mark_stack) {
        memcpy(new_stack, vm->mark_stack, vm->mark_stack_count * sizeof(HeapObj *));
        free(vm->mark_stack);
    }
#else
    new_stack = (HeapObj **)mini_word_grow(vm->mark_stack, vm->mark_stack_cap * sizeof(HeapObj *),
                                           new_cap * sizeof(HeapObj *));
#endif
    if (!new_stack) return false;
    vm->mark_stack = new_stack;
    vm->mark_stack_cap = new_cap;
    return true;
}

#if MINI_GC_MARKSWEEP
/* ========================================================================= */
/* Mark-sweep old generation: free lists and the lazy sweep                  */
/*                                                                           */
/* The tenured block stays one run of chunks, each with a HeapObj header:    */
/* objects, and free chunks (OBJ_FREE) linked into lists by size. Above      */
/* tenured_hp is space never used since the last compaction (the tail).      */
/* After a mark phase the chunks are swept a few at a time: marked ones lose */
/* their mark, runs of unmarked ones become free chunks. Allocation takes    */
/* only swept chunks, so nothing is ever allocated ahead of the sweep.       */
/* ========================================================================= */

#define MS_SWEEP_PER_MINOR 512u     /* chunks swept after each minor GC */
#define MS_SWEEP_FORCED    2048u    /* chunks per step when an allocation waits for room */

static void ms_put(MiniVM *vm, char *p, size_t sz) {
    if (sz == 0) return;
    MsFree *f = (MsFree *)p;
    f->type = OBJ_FREE;
    f->size = (uint32_t)sz;
    if (sz < MS_MIN_CHUNK) return;          /* 8 bytes: no room for the link, freed with a neighbour */
    if (sz <= MS_SMALL_MAX) {
        unsigned b = MS_BIN(sz);
        f->next = vm->ms_bins[b];
        vm->ms_bins[b] = f;
        vm->ms_bin_count[b]++;
    } else if (sz <= MS_MEDIUM_MAX) {
        f->next = vm->ms_medium;
        vm->ms_medium = f;
    } else {
        f->next = vm->ms_big;
        vm->ms_big = f;
    }
}

static void ms_lists_reset(MiniVM *vm) {
    memset(vm->ms_bins, 0, sizeof(vm->ms_bins));
    memset(vm->ms_bin_count, 0, sizeof(vm->ms_bin_count));
    vm->ms_medium = NULL;
    vm->ms_big = NULL;
}

/* What is left of a region goes back to the lists (the tail simply stays the tail) */
static void ms_retire(MiniVM *vm, MsRegion *r, bool tail) {
    if (!tail && r->end > r->ptr) ms_put(vm, r->ptr, (size_t)(r->end - r->ptr));
    r->ptr = NULL;
    r->end = NULL;
}

static void ms_retire_all(MiniVM *vm) {
    ms_retire(vm, &vm->ms_res, vm->ms_res_tail);
    vm->ms_res_tail = false;
    for (unsigned i = vm->ms_resq_i; i < vm->ms_resq_n && i < MS_RESQ; i++) ms_retire(vm, &vm->ms_resq[i], false);
    vm->ms_resq_i = vm->ms_resq_n = 0;
    ms_retire(vm, &vm->ms_small, false);
}

void mini_gc_heap_reset(MiniVM *vm) {
    ms_lists_reset(vm);
    vm->ms_res.ptr = vm->ms_res.end = NULL;
    vm->ms_resq_i = vm->ms_resq_n = 0;
    vm->ms_small.ptr = vm->ms_small.end = NULL;
    vm->ms_res_tail = false;
    vm->ms_sweep = vm->ms_sweep_end = NULL;
    vm->ms_used = (size_t)(vm->tenured_hp - vm->tenured_start);
}

static void ms_sweep_step(MiniVM *vm, size_t budget) {
    char *p = vm->ms_sweep;
    char *end = vm->ms_sweep_end;
    while (p < end && budget > 0) {
        HeapObj *o = (HeapObj *)p;
        if (o->type & MINI_GC_MARK_BIT) {
            o->type &= ~MINI_GC_MARK_BIT;
            p += ALIGN8(o->size);
            budget--;
            continue;
        }
        char *run = p;                       /* dead objects and old free chunks, merged */
        do {
            p += ALIGN8(((HeapObj *)p)->size);
            if (budget > 0) budget--;
        } while (p < end && budget > 0 && !(((HeapObj *)p)->type & MINI_GC_MARK_BIT));
        if (p >= end && end == vm->tenured_hp) {
            /* garbage right below the tail: the tail grows down over it */
            vm->tenured_hp = run;
            if (vm->ms_res_tail) vm->ms_res.ptr = run;
            end = run;
            p = run;
        } else {
            ms_put(vm, run, (size_t)(p - run));
        }
    }
    vm->ms_sweep = p;
    vm->ms_sweep_end = end;
}

static inline bool ms_sweeping(const MiniVM *vm) {
    return vm->ms_sweep < vm->ms_sweep_end;
}

/* First chunk of at least `need` bytes among the first max_scan of a list, unlinked */
static char *ms_take_fit(MsFree **list, size_t need, size_t max_scan, size_t *got) {
    MsFree **pp = list;
    while (*pp && max_scan > 0) {
        MsFree *f = *pp;
        if (f->size >= need) {
            *pp = f->next;
            *got = f->size;
            return (char *)f;
        }
        pp = &f->next;
        max_scan--;
    }
    return NULL;
}

/* Bytes that objects of at most `largest` bytes can surely take from the reserve: every change
 * of chunk may leave up to largest - 8 bytes unused */
static size_t ms_reserve_usable(const MiniVM *vm, size_t largest) {
    size_t total = (size_t)(vm->ms_res.end - vm->ms_res.ptr);
    size_t waste = largest > 8 ? largest - 8 : 0;
    for (unsigned i = vm->ms_resq_i; i < vm->ms_resq_n && i < MS_RESQ; i++) {
        size_t len = (size_t)(vm->ms_resq[i].end - vm->ms_resq[i].ptr);
        total += len > waste ? len - waste : 0;
    }
    return total;
}

/* A new reserve for `need` bytes of objects no larger than `largest`: free chunks of at least
 * `largest` bytes each (holes between live objects will do), or the tail */
static bool ms_find_reserve(MiniVM *vm, size_t need, size_t largest) {
    MsRegion got[MS_RESQ + 1];
    unsigned n = 0;
    size_t usable = 0;
    size_t waste = largest > 8 ? largest - 8 : 0;
    ms_retire_all(vm);                       /* what is left of the old one may serve again */
    /* holes first (the medium list), those that yield at least half their size; when they do
     * not add up they go back and the large chunks are tried */
    for (int pass = 0; pass < 2 && usable < need; pass++) {
        MsFree **pp = pass == 0 ? &vm->ms_medium : &vm->ms_big;
        size_t scan = pass == 0 ? 64 : (size_t)-1;
        if (pass == 0 && largest > MS_MEDIUM_MAX) continue;
        while (*pp && scan > 0 && n < MS_RESQ + 1 && usable < need) {
            MsFree *f = *pp;
            if (f->size >= largest && (pass == 1 || f->size >= 2 * waste)) {
                *pp = f->next;
                got[n].ptr = (char *)f;
                got[n].end = (char *)f + f->size;
                usable += n == 0 ? f->size : (f->size > waste ? f->size - waste : 0);
                n++;
            } else {
                pp = &f->next;
            }
            scan--;
        }
        if (pass == 0 && usable < need) {
            for (unsigned i = 0; i < n && i <= MS_RESQ; i++) ms_put(vm, got[i].ptr, (size_t)(got[i].end - got[i].ptr));
            n = 0;
            usable = 0;
        }
    }
    if (usable >= need) {
        vm->ms_res = got[0];
        for (unsigned i = 1; i < n && i <= MS_RESQ; i++) vm->ms_resq[i - 1] = got[i];
        vm->ms_resq_i = 0;
        vm->ms_resq_n = (uint8_t)(n - 1);
        return true;
    }
    for (unsigned i = 0; i < n && i <= MS_RESQ; i++) ms_put(vm, got[i].ptr, (size_t)(got[i].end - got[i].ptr));
    if ((size_t)(vm->tenured_limit - vm->tenured_hp) >= need) {
        vm->ms_res.ptr = vm->tenured_hp;
        vm->ms_res.end = vm->tenured_limit;
        vm->ms_res_tail = true;
        return true;
    }
    return false;
}

/* Can every object of the eden be promoted? Each takes a hole of its size when there is one;
 * the rest must fit in the reserve (dead objects are counted too: an upper bound). Sweeps on
 * while that may still help. */
static bool ms_room_for_eden(MiniVM *vm) {
    uint32_t need[MS_BINS];
    size_t other = 0, other_max = 0;
    memset(need, 0, sizeof(need));
    for (char *p = vm->eden_start; p < vm->eden_hp; ) {
        size_t sz = ALIGN8(((HeapObj *)p)->size);
        if (sz >= MS_MIN_CHUNK && sz <= MS_SMALL_MAX) {
            need[MS_BIN(sz)]++;
        } else {
            other += sz;
            if (sz > other_max) other_max = sz;
        }
        p += sz;
    }
    for (;;) {
        size_t r = other, largest = other_max;
        for (unsigned b = 0; b < MS_BINS; b++) {
            if (need[b] > vm->ms_bin_count[b]) {
                r += (size_t)(need[b] - vm->ms_bin_count[b]) * MS_BIN_SIZE(b);
                if (MS_BIN_SIZE(b) > largest) largest = MS_BIN_SIZE(b);
            }
        }
        if (r <= ms_reserve_usable(vm, largest)) return true;
        if (ms_find_reserve(vm, r, largest)) return true;
        if (!ms_sweeping(vm)) return false;
        ms_sweep_step(vm, MS_SWEEP_FORCED);
    }
}

/* Room for one promoted object: cannot fail after ms_room_for_eden() */
static inline char *ms_alloc_young(MiniVM *vm, size_t sz) {
    vm->ms_used += sz;
    if (sz >= MS_MIN_CHUNK && sz <= MS_SMALL_MAX) {
        unsigned b = MS_BIN(sz);
        MsFree *f = vm->ms_bins[b];
        if (f) {
            vm->ms_bins[b] = f->next;
            vm->ms_bin_count[b]--;
            return (char *)f;
        }
        if ((size_t)(vm->ms_small.end - vm->ms_small.ptr) < sz && vm->ms_medium) {
            MsFree *m = vm->ms_medium;       /* holes of other sizes are used up from their start */
            vm->ms_medium = m->next;
            ms_retire(vm, &vm->ms_small, false);
            vm->ms_small.ptr = (char *)m;
            vm->ms_small.end = (char *)m + m->size;
        }
        if ((size_t)(vm->ms_small.end - vm->ms_small.ptr) >= sz) {
            char *p = vm->ms_small.ptr;
            vm->ms_small.ptr += sz;
            return p;
        }
    }
    while ((size_t)(vm->ms_res.end - vm->ms_res.ptr) < sz) {
        if (vm->ms_resq_i >= vm->ms_resq_n) {
            fprintf(stderr, "minischeme: gc: promotion reserve exhausted\n");
            abort();
        }
        ms_retire(vm, &vm->ms_res, vm->ms_res_tail);   /* the next chunk lined up */
        vm->ms_res_tail = false;
        vm->ms_res = vm->ms_resq[vm->ms_resq_i++ & (MS_RESQ - 1)];
    }
    char *p = vm->ms_res.ptr;
    vm->ms_res.ptr += sz;
    if (vm->ms_res_tail) vm->tenured_hp = vm->ms_res.ptr;
    return p;
}

static void marksweep_gc(MiniVM *vm);

/* The old generation is out of room for `need` more bytes */
static void ms_collect_old(MiniVM *vm, size_t need) {
    bool can_grow = vm->max_memory_bytes == 0 || vm->tenured_size < vm->max_memory_bytes;
    size_t eden_used = (size_t)(vm->eden_hp - vm->eden_start);
    if (can_grow && !vm->heap_at_limit &&
        vm->ms_used + eden_used + need + vm->eden_size > vm->tenured_size * 0.75) {
        major_gc(vm, need);                  /* grows the heap (and compacts it on the way) */
    } else {
        marksweep_gc(vm);
    }
}

/* An object allocated in the old generation directly (too large for the eden). May collect. */
static char *ms_alloc_old(MiniVM *vm, size_t sz) {
    for (int pass = 0; pass < 3; pass++) {
        for (;;) {
            size_t got = 0;
            char *p = NULL;
            if (sz >= MS_MIN_CHUNK && sz <= MS_SMALL_MAX && vm->ms_bins[MS_BIN(sz)]) {
                MsFree *f = vm->ms_bins[MS_BIN(sz)];
                vm->ms_bins[MS_BIN(sz)] = f->next;
                vm->ms_bin_count[MS_BIN(sz)]--;
                p = (char *)f;
                got = sz;
            }
            if (!p && sz <= MS_MEDIUM_MAX) p = ms_take_fit(&vm->ms_medium, sz, 16, &got);
            if (!p) p = ms_take_fit(&vm->ms_big, sz, (size_t)-1, &got);
            if (p) {
                if (got > sz) ms_put(vm, p + sz, got - sz);
                vm->ms_used += sz;
                return p;
            }
            if ((size_t)(vm->ms_res.end - vm->ms_res.ptr) >= sz) {
                p = vm->ms_res.ptr;
                vm->ms_res.ptr += sz;
                if (vm->ms_res_tail) vm->tenured_hp = vm->ms_res.ptr;
                vm->ms_used += sz;
                return p;
            }
            if (!vm->ms_res_tail && (size_t)(vm->tenured_limit - vm->tenured_hp) >= sz) {
                p = vm->tenured_hp;
                vm->tenured_hp += sz;
                vm->ms_used += sz;
                return p;
            }
            if (!ms_sweeping(vm)) break;
            ms_sweep_step(vm, MS_SWEEP_FORCED);
        }
        if (pass == 0) ms_collect_old(vm, sz);
        else if (pass == 1) major_gc(vm, sz);   /* fragmented: compact */
    }
    return NULL;
}
#else
void mini_gc_heap_reset(MiniVM *vm) {
    (void)vm;
}
#endif /* MINI_GC_MARKSWEEP */

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
#if MINI_GC_MARKSWEEP
    HeapObj *new_obj = (HeapObj *)ms_alloc_young(vm, sz);
    memcpy(new_obj, old_obj, sz);
    new_obj->type &= MINI_GC_TYPE_MASK;
    /* promoted objects are scattered over the holes: those with fields go on a worklist
     * (room for all of them was made before the collection started) */
    switch (new_obj->type) {
        case OBJ_PAIR: case OBJ_CLOSURE: case OBJ_CODE: case OBJ_VECTOR: case OBJ_CONTINUATION: case OBJ_MACRO:
            vm->mark_stack[vm->mark_stack_count++] = new_obj;
            break;
        default:
            break;
    }
#else
    HeapObj *new_obj = (HeapObj *)vm->tenured_hp;
    memcpy(new_obj, old_obj, sz);
    vm->tenured_hp += sz;

    /* Clear any GC flags on promoted object in Tenured */
    new_obj->type &= MINI_GC_TYPE_MASK;
#endif

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
#if MINI_GC_MARKSWEEP
    (void)needed_bytes;                      /* an object too large for the eden finds its own room */
    if (!mark_stack_grow(vm, eden_used / 16 + 16)) {
        major_gc(vm, eden_used);             /* no worklist: the compacting collector needs none */
        return;
    }
    if (!ms_room_for_eden(vm)) {
        ms_collect_old(vm, eden_used);
        if (eden_used > 0 && vm->eden_hp == vm->eden_start) return;   /* a compaction took the eden along */
        if (!ms_room_for_eden(vm)) {
            major_gc(vm, eden_used);         /* free space there is, in pieces too small: compact */
            return;
        }
    }
    vm->mark_stack_count = 0;
#else
    /* If Tenured space cannot fit Eden promotion, run Major GC which collects both */
    if (vm->tenured_hp + eden_used + needed_bytes > vm->tenured_limit) {
        major_gc(vm, eden_used + needed_bytes);
        return;
    }

    char *old_tenured_hp = vm->tenured_hp;
#endif

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

    for (uint32_t i = 0; i < vm->gslot_count; i++) {   /* bindings of the global slots */
        if (vm->gslots[i]) vm->gslots[i] = promote_val(vm, vm->gslots[i]);
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
#if MINI_GC_MARKSWEEP
    while (vm->mark_stack_count > 0) {
        promote_object_fields(vm, vm->mark_stack[--vm->mark_stack_count]);
    }
#else
    char *scan_ptr = old_tenured_hp;
    while (scan_ptr < vm->tenured_hp) {
        HeapObj *obj = (HeapObj *)scan_ptr;
        promote_object_fields(vm, obj);
        scan_ptr += ALIGN8(obj->size);
    }
#endif

    /* Step 4: Reset Eden generation to empty */
    vm->eden_hp = vm->eden_start;
    {
#if MINI_GC_MARKSWEEP
        size_t used = vm->ms_used;
#else
        size_t used = (size_t)(vm->tenured_hp - vm->tenured_start);
#endif
        if (used > vm->stat_live_peak) vm->stat_live_peak = used;
    }

    /* Update legacy compatibility pointers */
    vm->hp = vm->tenured_hp;
    vm->to_space = vm->tenured_start;
    vm->hp_limit = vm->tenured_limit;

    /* Step 5: a heap that can still grow is collected (and grown) early, at 80%. One that
     * cannot (at its limit, or no larger block to be had) waits until the next promotion
     * does not fit: with more than 80% live it was collected in full after every minor GC */
#if MINI_GC_MARKSWEEP
    if (ms_sweeping(vm)) ms_sweep_step(vm, MS_SWEEP_PER_MINOR);   /* a little of the sweep each time */
    GC_VERIFY(vm, "minor");
#else
    size_t tenured_used = (size_t)(vm->tenured_hp - vm->tenured_start);
    if (!vm->heap_at_limit && tenured_used + needed_bytes + vm->eden_size > vm->tenured_size * 0.8) {
        major_gc(vm, needed_bytes);
    } else {
        GC_VERIFY(vm, "minor");
    }
#endif
}

/* ========================================================================= */
/* Major GC (In-Place Sliding Mark-Compact in Tenured Generation)           */
/* ========================================================================= */

static void mark_push(MiniVM *vm, HeapObj *obj) {
    if (vm->mark_stack_count >= vm->mark_stack_cap &&
        (vm->mark_stack_cap >= MINI_MARK_STACK_MAX || !mark_stack_grow(vm, vm->mark_stack_cap + 1))) {
        vm->mark_overflow = true;            /* marked, its fields not yet: found again by a heap scan */
        return;
    }
    vm->mark_stack[vm->mark_stack_count++] = obj;
}

/* Sets the mark of an object of the heap; false when it has one already or lies outside */
static inline bool mark_set(MiniVM *vm, HeapObj *obj) {
    bool in_tenured = ((char *)obj >= vm->tenured_start && (char *)obj < vm->tenured_hp);
    bool in_eden    = ((char *)obj >= vm->eden_start && (char *)obj < vm->eden_hp);
    if (!in_tenured && !in_eden) return false;
    if (obj->type & MINI_GC_MARK_BIT) return false;
#ifdef MINI_GC_VERIFY
    {
        uint32_t ty = obj->type & MINI_GC_TYPE_MASK;
        if (ty < OBJ_PAIR || ty > OBJ_BIGINT || ty == OBJ_FORWARDED || ALIGN8(obj->size) < 8) {
            fprintf(stderr, "gc verify: reachable object %p has header type %u size %u\n", (void *)obj,
                    (unsigned)obj->type, (unsigned)obj->size);
            abort();
        }
    }
#endif
    obj->type |= MINI_GC_MARK_BIT;
#if MINI_GC_MARKSWEEP
    if (in_tenured) vm->ms_marked += ALIGN8(obj->size);
#endif
    return true;
}

static void mark_val(MiniVM *vm, Value val) {
    uintptr_t tag = val & MINI_TAG_MASK;
    if (tag == MINI_TAG_FIXNUM || tag == MINI_TAG_CONST) return;
    HeapObj *obj = (HeapObj *)UNTAG_PTR(val);
    if (!obj) return;
    if (mark_set(vm, obj)) mark_push(vm, obj);
}

static void mark_children(MiniVM *vm, HeapObj *obj) {
    uint32_t type = obj->type & MINI_GC_TYPE_MASK;
    switch (type) {
        case OBJ_PAIR: {
            PairObj *p = (PairObj *)obj;
            while (p) {                      /* a list is walked, not pushed cell by cell */
                mark_val(vm, p->car);
                Value cdr_val = p->cdr;
                if (mini_is_pair(cdr_val)) {
                    HeapObj *cdr_obj = (HeapObj *)UNTAG_PTR(cdr_val);
                    if (mark_set(vm, cdr_obj)) {
                        p = (PairObj *)cdr_obj;
                        continue;
                    }
                    break;
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

static void mark_drain(MiniVM *vm) {
    while (vm->mark_stack_count > 0) {
        mark_children(vm, vm->mark_stack[--vm->mark_stack_count]);
    }
}

/* Marks everything reachable from the roots, in the tenured generation and in the eden.
 * Both must be a run of valid headers (free chunks included). */
static void gc_mark_all(MiniVM *vm) {
    vm->mark_stack_count = 0;
    vm->mark_overflow = false;

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

    if (vm->gslots) {
        for (uint32_t i = 0; i < vm->gslot_count; i++) {
            if (vm->gslots[i]) mark_val(vm, vm->gslots[i]);
        }
    }

    mark_drain(vm);

    /* The stack could not grow: some marked objects were never traced. Trace every marked
     * object again until a pass leaves nothing behind. */
    while (vm->mark_overflow) {
        vm->mark_overflow = false;
        for (int part = 0; part < 2; part++) {
            char *scan = part == 0 ? vm->tenured_start : vm->eden_start;
            char *end  = part == 0 ? vm->tenured_hp : vm->eden_hp;
            while (scan < end) {
                HeapObj *obj = (HeapObj *)scan;
#if MINI_GC_MARKSWEEP
                /* a chunk being filled has no header over its unused part (only the debug
                 * verifier marks while one is open) */
                if (part == 0 && scan == vm->ms_res.ptr && !vm->ms_res_tail && vm->ms_res.end > scan) {
                    scan = vm->ms_res.end;
                    continue;
                }
                if (part == 0 && scan == vm->ms_small.ptr && vm->ms_small.end > scan) {
                    scan = vm->ms_small.end;
                    continue;
                }
#endif
                if (obj->type & MINI_GC_MARK_BIT) {
                    mark_children(vm, obj);
                    mark_drain(vm);
                }
                scan += ALIGN8(obj->size);
            }
        }
    }
}

#ifdef MINI_GC_VERIFY
/* Debug builds: the heap is a run of valid chunks, the free lists hold free chunks, and (when
 * no sweep is pending) everything reachable is a valid object */
static void gc_verify(MiniVM *vm, const char *when) {
    size_t free_listed = 0, free_seen = 0;
    for (char *p = vm->tenured_start; p < vm->tenured_hp; ) {
        HeapObj *o = (HeapObj *)p;
        uint32_t ty = o->type & MINI_GC_TYPE_MASK;
        size_t sz = ALIGN8(o->size);
        bool in_region = false;
#if MINI_GC_MARKSWEEP
        if (p == vm->ms_res.ptr && !vm->ms_res_tail && vm->ms_res.end > p) { p = vm->ms_res.end; in_region = true; }
        else if (p == vm->ms_small.ptr && vm->ms_small.end > p) { p = vm->ms_small.end; in_region = true; }
#endif
        if (in_region) continue;
        if (sz < 8 || ty < OBJ_PAIR || ty > OBJ_FREE || p + sz > vm->tenured_hp) {
            fprintf(stderr, "gc verify (%s): bad chunk at %p (+%u): type %u size %u\n", when, (void *)p,
                    (unsigned)(p - vm->tenured_start), (unsigned)o->type, (unsigned)o->size);
            abort();
        }
        if (ty == OBJ_FREE && sz >= 16) free_seen += sz;
        p += sz;
    }
#if MINI_GC_MARKSWEEP
    for (unsigned b = 0; b < MS_BINS + 2; b++) {
        MsFree *f = b < MS_BINS ? vm->ms_bins[b] : b == MS_BINS ? vm->ms_medium : vm->ms_big;
        uint32_t n = 0;
        for (; f; f = f->next, n++) {
            if ((char *)f < vm->tenured_start || (char *)f >= vm->tenured_hp || f->type != OBJ_FREE ||
                (b < MS_BINS && f->size != MS_BIN_SIZE(b))) {
                fprintf(stderr, "gc verify (%s): bad free chunk %p in list %u\n", when, (void *)f, b);
                abort();
            }
            free_listed += f->size;
        }
        if (b < MS_BINS && n != vm->ms_bin_count[b]) {
            fprintf(stderr, "gc verify (%s): list %u holds %u chunks, counted %u\n", when, b, (unsigned)n,
                    (unsigned)vm->ms_bin_count[b]);
            abort();
        }
    }
    if (free_listed > free_seen) {
        fprintf(stderr, "gc verify (%s): %u bytes listed free, %u seen\n", when, (unsigned)free_listed,
                (unsigned)free_seen);
        abort();
    }
    if (ms_sweeping(vm)) return;             /* marks pending: the trace below would disturb them */
    size_t marked = vm->ms_marked;
#endif
    size_t saved = vm->mark_stack_count;     /* never non-zero here, but cheap to keep */
    gc_mark_all(vm);
    vm->mark_stack_count = saved;
    for (int part = 0; part < 2; part++) {
        char *scan = part == 0 ? vm->tenured_start : vm->eden_start;
        char *end  = part == 0 ? vm->tenured_hp : vm->eden_hp;
        while (scan < end) {
            HeapObj *obj = (HeapObj *)scan;
#if MINI_GC_MARKSWEEP
            if (part == 0 && scan == vm->ms_res.ptr && !vm->ms_res_tail && vm->ms_res.end > scan) { scan = vm->ms_res.end; continue; }
            if (part == 0 && scan == vm->ms_small.ptr && vm->ms_small.end > scan) { scan = vm->ms_small.end; continue; }
#endif
            obj->type &= ~MINI_GC_MARK_BIT;
            scan += ALIGN8(obj->size);
        }
    }
#if MINI_GC_MARKSWEEP
    vm->ms_marked = marked;
#endif
    (void)free_listed; (void)free_seen;
}
#endif

#if MINI_GC_MARKSWEEP
/* Major GC without moving anything: mark, then the sweep is left to later allocations. The
 * eden is traced through but stays as it is. */
static void marksweep_gc(MiniVM *vm) {
    vm->stat_major_gcs++;
    ms_sweep_step(vm, (size_t)-1);           /* marks left over from the last cycle */
    ms_retire_all(vm);                       /* every byte below tenured_hp under a header */
    ms_lists_reset(vm);                      /* the sweep builds the lists anew */

    vm->ms_marked = 0;
    gc_mark_all(vm);

    /* dead old objects leave the remembered set: their memory is about to be reused */
    size_t kept = 0;
    for (size_t i = 0; i < vm->remset_count; i++) {
        HeapObj *robj = (HeapObj *)UNTAG_PTR(vm->remset[i]);
        if (robj && (robj->type & MINI_GC_MARK_BIT)) vm->remset[kept++] = vm->remset[i];
    }
    vm->remset_count = kept;

    for (char *p = vm->eden_start; p < vm->eden_hp; p += ALIGN8(((HeapObj *)p)->size)) {
        ((HeapObj *)p)->type &= ~MINI_GC_MARK_BIT;
    }

    vm->ms_used = vm->ms_marked;
    vm->stat_live_bytes = vm->ms_marked;
    if (vm->stat_live_bytes > vm->stat_live_peak) vm->stat_live_peak = vm->stat_live_bytes;
    vm->ms_sweep = vm->tenured_start;
    vm->ms_sweep_end = vm->tenured_hp;
}
#endif

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
    vm->stat_compact_gcs++;
    vm->gcache_gen++; /* objects move */
#if MINI_GC_MARKSWEEP
    ms_sweep_step(vm, (size_t)-1);           /* no marks left from a mark-sweep cycle */
    ms_retire_all(vm);                       /* every byte below tenured_hp under a header */
    ms_lists_reset(vm);
#endif

    /* bench patch: clear the remembered set while its pointers are still valid */
    for (size_t i = 0; i < vm->remset_count; i++) {
        HeapObj *robj = (HeapObj *)UNTAG_PTR(vm->remset[i]);
        if (robj) robj->type &= ~MINI_GC_REMSET_BIT;
    }
    vm->remset_count = 0;

#if MINI_GC_MARKSWEEP
    size_t tenured_used = vm->ms_used;       /* tenured_hp says nothing: there are holes below it */
#else
    size_t tenured_used = (size_t)(vm->tenured_hp - vm->tenured_start);
#endif
    size_t eden_used = (size_t)(vm->eden_hp - vm->eden_start);
    size_t total_used = tenured_used + eden_used;
    size_t target_size = vm->tenured_size;
    size_t old_size = vm->tenured_size;

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
    bool wanted_more = total_used + needed_bytes + vm->eden_size > vm->tenured_size * 0.75;

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
    gc_mark_all(vm);

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

    for (uint32_t i = 0; i < vm->gslot_count; i++) {
        if (vm->gslots[i]) vm->gslots[i] = fix_forwarded_val(vm, target_start, vm->gslots[i]);
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
    /* growth was due and did not happen: collecting early (minor_gc, step 5) gains nothing */
    vm->heap_at_limit = wanted_more && target_size == old_size;

    /* Reset Eden generation to empty */
    vm->eden_hp = vm->eden_start;

    /* Remembered Set: already cleared at the start of major_gc (bench patch) */
    vm->remset_count = 0;

    /* Legacy compatibility pointers */
    vm->hp = vm->tenured_hp;
    vm->to_space = vm->tenured_start;
    vm->to_space_raw = vm->tenured_raw;
    vm->hp_limit = vm->tenured_limit;

    vm->stat_live_bytes = (size_t)(vm->tenured_hp - vm->tenured_start);
    if (vm->stat_live_bytes > vm->stat_live_peak) vm->stat_live_peak = vm->stat_live_bytes;
    mini_gc_heap_reset(vm);                  /* mark-sweep: no holes, everything free is the tail */
    GC_VERIFY(vm, "major");
}

void mini_get_stats(const MiniVM *vm, MiniStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!vm) return;
    out->alloc_bytes = vm->stat_alloc_bytes;
    out->minor_gcs   = vm->stat_minor_gcs;
    out->major_gcs   = vm->stat_major_gcs;
    out->compact_gcs = vm->stat_compact_gcs;
    out->gc_max_us   = vm->stat_gc_max_us;
    out->gc_total_us = vm->stat_gc_total_us;
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
#if !MINI_GC_MARKSWEEP
    if (vm->tenured_hp + needed_bytes > vm->tenured_limit) {
        major_gc(vm, needed_bytes);
    }
#endif
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
        uint64_t t0 = MINI_GC_NOW_US();
        minor_gc(vm, sz);
        /* If Eden is still too small (sz > eden_size), allocate directly in Tenured */
        if (vm->eden_hp + sz > vm->eden_limit) {
#if MINI_GC_MARKSWEEP
            HeapObj *old = (HeapObj *)ms_alloc_old(vm, sz);
            gc_pause_done(vm, t0);
            if (old) {
                old->type = (uint32_t)type;
                old->size = (uint32_t)sz;
                return (void *)old;
            }
#else
            if (vm->tenured_hp + sz > vm->tenured_limit) {
                major_gc(vm, sz);
            }
            gc_pause_done(vm, t0);
            if (vm->tenured_hp + sz <= vm->tenured_limit) {
                HeapObj *obj = (HeapObj *)vm->tenured_hp;
                vm->tenured_hp += sz;
                obj->type = (uint32_t)type;
                obj->size = (uint32_t)sz;
                return (void *)obj;
            }
#endif
            vm_error(vm, "out of memory: heap exhausted (%zu bytes requested)", sz);
            return NULL;
        }
        gc_pause_done(vm, t0);
    }

    HeapObj *obj = (HeapObj *)vm->eden_hp;
    vm->eden_hp += sz;
    obj->type = (uint32_t)type;
    obj->size = (uint32_t)sz;
    return (void *)obj;
}

void mini_gc(MiniVM *vm) {
    uint64_t t0 = MINI_GC_NOW_US();
    minor_gc(vm, 0);
#if MINI_GC_MARKSWEEP
    marksweep_gc(vm);
#else
    major_gc(vm, 0);
#endif
    gc_pause_done(vm, t0);
}

size_t mini_get_heap_size(const MiniVM *vm) {
    return vm ? vm->tenured_size : 0;
}

size_t mini_get_allocated_bytes(const MiniVM *vm) {
    if (!vm) return 0;
    size_t eden_used = (size_t)(vm->eden_hp - vm->eden_start);
#if MINI_GC_MARKSWEEP
    size_t tenured_used = vm->ms_used;
#else
    size_t tenured_used = (size_t)(vm->tenured_hp - vm->tenured_start);
#endif
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
    size_t aligned_sz = ALIGN8(sizeof(PairObj));
    if (MINI_LIKELY(vm->eden_hp + aligned_sz <= vm->eden_limit)) {
        PairObj *p = (PairObj *)vm->eden_hp;
        vm->eden_hp += aligned_sz;
        vm->stat_alloc_bytes += aligned_sz;
#ifdef MINI_ALLOC_PROFILE
        mini_prof_alloc(aligned_sz, OBJ_PAIR);
#endif
        p->header.type = OBJ_PAIR;
        p->header.size = (uint32_t)aligned_sz;
        p->car = car;
        p->cdr = cdr;
        return ((uintptr_t)p) | MINI_TAG_PAIR;
    }
    mini_push_root(vm, &car);
    mini_push_root(vm, &cdr);
    PairObj *p = (PairObj *)gc_alloc(vm, sizeof(PairObj), OBJ_PAIR);
    mini_pop_root(vm);
    mini_pop_root(vm);
    if (!p) return mini_void();
    p->car = car;
    p->cdr = cdr;
    Value res = ((uintptr_t)p) | MINI_TAG_PAIR;
    if (MINI_UNLIKELY((char *)p >= vm->tenured_start && (char *)p < vm->tenured_limit)) {
        mini_write_barrier(vm, res, car);
        mini_write_barrier(vm, res, cdr);
    }
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
#ifdef MINI_HAS_IMM_FLONUM
    Value imm;
    if (MINI_LIKELY(mini_try_imm_flonum(num, &imm))) return imm;
#endif
    FlonumObj *f = (FlonumObj *)gc_alloc(vm, sizeof(FlonumObj), OBJ_FLONUM);
    if (!f) return mini_void();
    f->val = num;
    return ((uintptr_t)f) | MINI_TAG_RECORD;
}

double mini_to_flonum(Value v) {
#ifdef MINI_HAS_IMM_FLONUM
    if (MINI_LIKELY(mini_is_imm_flonum(v))) return mini_imm_flonum_to_double(v);
#endif
    if (mini_is_record(v) && (((MiniHeapObj *)(v & ~MINI_TAG_MASK))->type & MINI_OBJ_TYPE_MASK) == MINI_OBJ_FLONUM) {
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
