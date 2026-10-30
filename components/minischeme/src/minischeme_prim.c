/**
 * @file minischeme_prim.c
 * @brief MiniScheme Built-in C Primitive Procedures and Standard Library.
 *
 * Implements:
 *  - Pairs, lists, vectors, strings, symbols, and character primitives.
 *  - Bytecode compiler engine primitives (%make-code, %make-closure, %execute).
 *  - FASL dump and load primitives.
 *  - Built-in primitive registration and function dispatch table.
 */

#include "minischeme_internal.h"

/* Forward declarations of FASL primitives from minischeme_fasl.c */
Value prim_dump_fasl(MiniVM *vm, int argc, Value *argv);
Value prim_load_fasl(MiniVM *vm, int argc, Value *argv);
Value prim_fasl_to_c(MiniVM *vm, int argc, Value *argv);

/* ========================================================================= */
/* Vector Primitives                                                         */
/* ========================================================================= */

static Value prim_vector(MiniVM *vm, int argc, Value *argv) {
    Value vec = mini_make_vector(vm, argc, mini_void());
    for (int i = 0; i < argc; i++) {
        mini_vector_set(vm, vec, i, argv[i]);
    }
    return vec;
}

static Value prim_make_vector(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_fixnum(argv[0])) {
        vm_error(vm, "make-vector: expected integer length");
        return mini_void();
    }
    int64_t len = mini_to_fixnum(argv[0]);
    if (len < 0) {
        vm_error(vm, "make-vector: negative length");
        return mini_void();
    }
    Value fill = (argc >= 2) ? argv[1] : mini_make_fixnum(0);
    return mini_make_vector(vm, (size_t)len, fill);
}

static Value prim_vector_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc == 1 && mini_is_vector(argv[0])) ? mini_true() : mini_false();
}

static Value prim_vector_length(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_vector(argv[0])) {
        vm_error(vm, "vector-length: expected vector");
        return mini_void();
    }
    return mini_make_fixnum((int64_t)mini_vector_length(argv[0]));
}

Value prim_vector_ref(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_vector(argv[0]) || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "vector-ref: expected vector and fixnum index");
        return mini_void();
    }
    int64_t idx = mini_to_fixnum(argv[1]);
    if (idx < 0) {
        vm_error(vm, "vector-ref: negative index");
        return mini_void();
    }
    return mini_vector_ref(vm, argv[0], (size_t)idx);
}

static Value prim_vector_set(MiniVM *vm, int argc, Value *argv) {
    if (argc != 3 || !mini_is_vector(argv[0]) || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "vector-set!: expected vector, index, and value");
        return mini_void();
    }
    int64_t idx = mini_to_fixnum(argv[1]);
    if (idx < 0) {
        vm_error(vm, "vector-set!: negative index");
        return mini_void();
    }
    if (mini_rom_contains(vm, argv[0])) {
        vm_error(vm, "%s: cannot modify a literal constant", "vector-set!");
        return mini_void();
    }
    mini_vector_set(vm, argv[0], (size_t)idx, argv[2]);
    return mini_void();
}

static Value prim_vector_to_list(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_vector(argv[0])) {
        vm_error(vm, "vector->list: expected vector");
        return mini_void();
    }
    size_t len = mini_vector_length(argv[0]);
    Value list = mini_nil();
    mini_push_root(vm, &list);
    for (size_t i = len; i > 0; i--) {
        Value elem = mini_vector_ref(vm, argv[0], i - 1);
        list = mini_make_cons(vm, elem, list);
    }
    mini_pop_root(vm);
    return list;
}

static Value prim_list_to_vector(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) {
        vm_error(vm, "list->vector: expected 1 argument");
        return mini_void();
    }
    size_t len = 0;
    Value curr = argv[0];
    while (mini_is_pair(curr)) {
        len++;
        curr = mini_cdr(curr);
    }
    Value vec = mini_make_vector(vm, len, mini_void());
    curr = argv[0];
    for (size_t i = 0; i < len; i++) {
        mini_vector_set(vm, vec, i, mini_car(curr));
        curr = mini_cdr(curr);
    }
    return vec;
}

static Value prim_vector_fill(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_vector(argv[0])) {
        vm_error(vm, "vector-fill!: expected vector and value");
        return mini_void();
    }
    if (mini_rom_contains(vm, argv[0])) {
        vm_error(vm, "%s: cannot modify a literal constant", "vector-fill!");
        return mini_void();
    }
    size_t len = mini_vector_length(argv[0]);
    for (size_t i = 0; i < len; i++) {
        mini_vector_set(vm, argv[0], i, argv[1]);
    }
    return mini_void();
}

/* ========================================================================= */
/* Pair and List Primitives                                                  */
/* ========================================================================= */

static Value prim_cons(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2) {
        vm_error(vm, "cons: expected 2 arguments");
        return mini_void();
    }
    return mini_make_cons(vm, argv[0], argv[1]);
}

static const char *mini_describe_type(Value v) {
    if (mini_is_false(v)) return "#f";
    if (mini_is_true(v)) return "#t";
    if (mini_is_nil(v)) return "()";
    if (mini_is_void(v)) return "#<void>";
    if (mini_is_fixnum(v)) return "fixnum";
    if (mini_is_flonum(v)) return "flonum";
    if (mini_is_symbol(v)) return "symbol";
    if (mini_is_string(v)) return "string";
    if (mini_is_vector(v)) return "vector";
    if (mini_is_closure(v)) return "procedure";
    if (mini_is_cfunc(v)) return "c-procedure";
    if (mini_is_continuation(v)) return "continuation";
    return "non-pair";
}

Value prim_car(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "car: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    return mini_car(argv[0]);
}

Value prim_cdr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cdr: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    return mini_cdr(argv[0]);
}

/* (vm-rom-bytes) => size of the attached Code ROM image, 0 when none */
static Value prim_vm_rom_bytes(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_fixnum((int64_t)mini_rom_size(vm));
}

static void stats_push(MiniVM *vm, Value *lst, const char *key, int64_t v) {
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value cell = mini_make_cons(vm, k, mini_make_fixnum(v));
    mini_pop_root(vm);
    mini_push_root(vm, &cell);
    *lst = mini_make_cons(vm, cell, *lst);
    mini_pop_root(vm);
}

/* (vm-stats) => alist of allocation, GC and depth counters */
static Value prim_vm_stats(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    MiniStats st;
    mini_get_stats(vm, &st);
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    stats_push(vm, &lst, "rom-bytes", (int64_t)st.rom_bytes);
    stats_push(vm, &lst, "frame-slots", (int64_t)st.frame_slots);
    stats_push(vm, &lst, "stack-slots", (int64_t)st.stack_slots);
    stats_push(vm, &lst, "frame-peak", (int64_t)st.frame_peak);
    stats_push(vm, &lst, "stack-peak", (int64_t)st.stack_peak);
    stats_push(vm, &lst, "live-peak", (int64_t)st.live_peak);
    stats_push(vm, &lst, "live-bytes", (int64_t)st.live_bytes);
    stats_push(vm, &lst, "major-gcs", (int64_t)st.major_gcs);
    stats_push(vm, &lst, "minor-gcs", (int64_t)st.minor_gcs);
    stats_push(vm, &lst, "alloc-kb", (int64_t)(st.alloc_bytes / 1024));
    mini_pop_root(vm);
    return lst;
}

/* (vm-stack-trace [on?]) => boolean: query or enable/disable stack trace */
static Value prim_vm_stack_trace(MiniVM *vm, int argc, Value *argv) {
    if (argc == 0) {
        return mini_is_stack_trace_enabled(vm) ? mini_true() : mini_false();
    }
    bool on = !mini_is_false(argv[0]);
    mini_set_stack_trace_enabled(vm, on);
    return on ? mini_true() : mini_false();
}

static Value prim_set_car(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_pair(argv[0])) {
        vm_error(vm, "set-car!: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    if (mini_rom_contains(vm, argv[0])) {
        vm_error(vm, "%s: cannot modify a literal constant", "set-car!");
        return mini_void();
    }
    mini_set_car(argv[0], argv[1]);
    mini_write_barrier(vm, argv[0], argv[1]);
    return mini_void();
}

static Value prim_set_cdr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_pair(argv[0])) {
        vm_error(vm, "set-cdr!: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    if (mini_rom_contains(vm, argv[0])) {
        vm_error(vm, "%s: cannot modify a literal constant", "set-cdr!");
        return mini_void();
    }
    mini_set_cdr(argv[0], argv[1]);
    mini_write_barrier(vm, argv[0], argv[1]);
    return mini_void();
}

static Value prim_list(MiniVM *vm, int argc, Value *argv) {
    Value res = mini_nil();
    mini_push_root(vm, &res);
    for (int i = argc - 1; i >= 0; i--) {
        res = mini_make_cons(vm, argv[i], res);
    }
    mini_pop_root(vm);
    return res;
}

static Value prim_caar(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "caar: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    Value a = mini_car(argv[0]);
    if (!mini_is_pair(a)) {
        vm_error(vm, "caar: requires a pair in car (got %s)", mini_describe_type(a));
        return mini_void();
    }
    return mini_car(a);
}

static Value prim_cadr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cadr: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    Value d = mini_cdr(argv[0]);
    if (!mini_is_pair(d)) {
        vm_error(vm, "cadr: requires a pair in cdr (got %s)", mini_describe_type(d));
        return mini_void();
    }
    return mini_car(d);
}

static Value prim_cdar(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cdar: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    Value a = mini_car(argv[0]);
    if (!mini_is_pair(a)) {
        vm_error(vm, "cdar: requires a pair in car (got %s)", mini_describe_type(a));
        return mini_void();
    }
    return mini_cdr(a);
}

static Value prim_cddr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cddr: requires a pair (got %s)", argc >= 1 ? mini_describe_type(argv[0]) : "none");
        return mini_void();
    }
    Value d = mini_cdr(argv[0]);
    if (!mini_is_pair(d)) {
        vm_error(vm, "cddr: requires a pair in cdr");
        return mini_void();
    }
    return mini_cdr(d);
}

static Value prim_caddr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "caddr: requires a pair");
        return mini_void();
    }
    Value d = mini_cdr(argv[0]);
    if (!mini_is_pair(d) || !mini_is_pair(mini_cdr(d))) {
        vm_error(vm, "caddr: list too short");
        return mini_void();
    }
    return mini_car(mini_cdr(d));
}

static Value prim_cdddr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cdddr: requires a pair");
        return mini_void();
    }
    Value d = mini_cdr(argv[0]);
    if (!mini_is_pair(d) || !mini_is_pair(mini_cdr(d))) {
        vm_error(vm, "cdddr: list too short");
        return mini_void();
    }
    return mini_cdr(mini_cdr(d));
}

static Value prim_cadddr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cadddr: requires a pair");
        return mini_void();
    }
    Value d = mini_cdr(argv[0]);
    if (!mini_is_pair(d)) goto err;
    d = mini_cdr(d);
    if (!mini_is_pair(d)) goto err;
    d = mini_cdr(d);
    if (!mini_is_pair(d)) goto err;
    return mini_car(d);
err:
    vm_error(vm, "cadddr: list too short");
    return mini_void();
}

static Value prim_cddddr(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_pair(argv[0])) {
        vm_error(vm, "cddddr: requires a pair");
        return mini_void();
    }
    Value d = mini_cdr(argv[0]);
    if (!mini_is_pair(d)) goto err;
    d = mini_cdr(d);
    if (!mini_is_pair(d)) goto err;
    d = mini_cdr(d);
    if (!mini_is_pair(d)) goto err;
    return mini_cdr(d);
err:
    vm_error(vm, "cddddr: list too short");
    return mini_void();
}


#define DEFINE_PRED(name, pred) \
static Value name(MiniVM *vm, int argc, Value *argv) { \
    (void)vm; \
    return (argc == 1 && pred(argv[0])) ? mini_true() : mini_false(); \
}

DEFINE_PRED(prim_pair_p, mini_is_pair)
DEFINE_PRED(prim_null_p, mini_is_nil)
DEFINE_PRED(prim_boolean_p, mini_is_bool)
DEFINE_PRED(prim_symbol_p, mini_is_symbol)
DEFINE_PRED(prim_string_p, mini_is_string)
DEFINE_PRED(prim_char_p, mini_is_fixnum)

static Value prim_procedure_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc == 1 && (mini_is_closure(argv[0]) || mini_is_cfunc(argv[0]) || mini_is_continuation(argv[0]))) ? mini_true() : mini_false();
}

static Value prim_eq_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 2) return mini_false();
    return mini_make_bool(mini_eq(argv[0], argv[1]));
}

static Value prim_equal_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 2) return mini_false();
    return mini_make_bool(mini_equal(argv[0], argv[1]));
}

static Value prim_not(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1) return mini_false();
    return mini_make_bool(argv[0] == MINI_CONST_FALSE);
}

/* ========================================================================= */
/* I/O and System Primitives                                                 */
/* ========================================================================= */

static Value prim_display(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1) return mini_void();
    mini_print_value(vm, argv[0], stdout, false);
    return mini_void();
}

static Value prim_write(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1) return mini_void();
    mini_print_value(vm, argv[0], stdout, true);
    return mini_void();
}

static Value prim_newline(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    putchar('\n');
    return mini_void();
}

static Value prim_exit(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "exit")) return mini_void();
    (void)vm;
    int code = 0;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        code = (int)mini_to_fixnum(argv[0]);
    }
    exit(code);
    return mini_void();
}

static Value prim_error(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1 && mini_is_string(argv[0])) {
        vm_error(vm, "%s", mini_string_data(argv[0]));
    } else {
        vm_error(vm, "error");
    }
    return mini_void();
}

static Value prim_apply(MiniVM *vm, int argc, Value *argv) {
    if (argc < 2) {
        vm_error(vm, "apply: expected at least 2 arguments");
        return mini_void();
    }
    Value fn = argv[0];
    Value last_arg = argv[argc - 1];

    size_t extra_args = 0;
    Value curr = last_arg;
    while (mini_is_pair(curr)) {
        extra_args++;
        curr = mini_cdr(curr);
    }
    if (curr != MINI_CONST_NIL) {
        vm_error(vm, "apply: last argument must be a proper list");
        return mini_void();
    }

    size_t total_args = (size_t)(argc - 2) + extra_args;
    Value *flat_argv = (Value *)malloc(total_args * sizeof(Value));
    if (!flat_argv) {
        vm_error(vm, "apply: out of memory allocating argument list");
        return mini_void();
    }

    size_t pos = 0;
    for (int i = 1; i < argc - 1; i++) {
        flat_argv[pos++] = argv[i];
    }
    curr = last_arg;
    while (mini_is_pair(curr)) {
        flat_argv[pos++] = mini_car(curr);
        curr = mini_cdr(curr);
    }

    Value res = mini_call(vm, fn, (int)total_args, flat_argv);
    free(flat_argv);
    return res;
}

/* ========================================================================= */
/* String and Symbol Primitives                                              */
/* ========================================================================= */

static Value prim_string_length(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "string-length: expected string");
        return mini_void();
    }
    return mini_make_fixnum((int64_t)mini_string_len(argv[0]));
}

static Value prim_string_ref(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_string(argv[0]) || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "string-ref: expected string and fixnum index");
        return mini_void();
    }
    int64_t idx = mini_to_fixnum(argv[1]);
    size_t len = mini_string_len(argv[0]);
    if (idx < 0 || (size_t)idx >= len) {
        vm_error(vm, "string-ref: index %lld out of bounds (len %zu)", (long long)idx, len);
        return mini_void();
    }
    const char *data = mini_string_data(argv[0]);
    return mini_make_fixnum((unsigned char)data[idx]);
}

static Value prim_string_append(MiniVM *vm, int argc, Value *argv) {
    size_t total_len = 0;
    for (int i = 0; i < argc; i++) {
        if (!mini_is_string(argv[i])) {
            vm_error(vm, "string-append: expected string");
            return mini_void();
        }
        total_len += mini_string_len(argv[i]);
    }
    char *buf = (char *)malloc(total_len + 1);
    if (!buf) {
        vm_error(vm, "string-append: out of memory");
        return mini_void();
    }
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = mini_string_len(argv[i]);
        if (len > 0) {
            memcpy(buf + pos, mini_string_data(argv[i]), len);
            pos += len;
        }
    }
    buf[total_len] = '\0';
    Value res = mini_make_string_len(vm, buf, total_len);
    free(buf);
    return res;
}

static Value prim_substring(MiniVM *vm, int argc, Value *argv) {
    if (argc != 3 || !mini_is_string(argv[0]) || !mini_is_fixnum(argv[1]) || !mini_is_fixnum(argv[2])) {
        vm_error(vm, "substring: expected string, start, and end");
        return mini_void();
    }
    int64_t start = mini_to_fixnum(argv[1]);
    int64_t end = mini_to_fixnum(argv[2]);
    size_t len = mini_string_len(argv[0]);
    if (start < 0 || end < start || (size_t)end > len) {
        vm_error(vm, "substring: invalid slice [%lld, %lld] on string of len %zu", (long long)start, (long long)end, len);
        return mini_void();
    }
    const char *data = mini_string_data(argv[0]);
    return mini_make_string_len(vm, data + start, (size_t)(end - start));
}

static Value prim_symbol_to_string(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_symbol(argv[0])) {
        vm_error(vm, "symbol->string: expected symbol");
        return mini_void();
    }
    return mini_make_string(vm, mini_symbol_name(argv[0]));
}

static Value prim_string_to_symbol(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "string->symbol: expected string");
        return mini_void();
    }
    return mini_make_symbol(vm, mini_string_data(argv[0]));
}

static Value prim_string_to_number(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "string->number: expected string");
        return mini_false();
    }
    const char *str = mini_string_data(argv[0]);
    int radix = 10;
    if (argc >= 2 && mini_is_fixnum(argv[1])) {
        radix = (int)mini_to_fixnum(argv[1]);
    }

    char sign = 0;
    if (*str == '+' || *str == '-') {
        sign = *str;
        str++;
    }

    if (*str == '#') {
        char pfx = str[1];
        if (pfx == 'x' || pfx == 'X') { radix = 16; str += 2; }
        else if (pfx == 'b' || pfx == 'B') { radix = 2; str += 2; }
        else if (pfx == 'o' || pfx == 'O') { radix = 8; str += 2; }
        else if (pfx == 'd' || pfx == 'D') { radix = 10; str += 2; }
        if (*str == '+' || *str == '-') {
            if (sign == 0) sign = *str;
            else return mini_false();
            str++;
        }
    }

    char buf[128];
    const char *parse_str = str;
    if (sign == '-') {
        snprintf(buf, sizeof(buf), "-%s", str);
        parse_str = buf;
    } else if (sign == '+') {
        snprintf(buf, sizeof(buf), "+%s", str);
        parse_str = buf;
    }

    Value b = bigint_from_str(vm, parse_str, radix);
    if (b != mini_false()) {
        return b;
    }

    if (radix == 10) {
        bool has_digit = false;
        for (const char *p = parse_str; *p; p++) {
            if (isdigit((unsigned char)*p)) {
                has_digit = true;
                break;
            }
        }
        if (!has_digit) {
            return mini_false();
        }

        char *endptr = NULL;
        double dval = strtod(parse_str, &endptr);
        if (endptr != parse_str && *endptr == '\0') {
            return mini_make_flonum(vm, dval);
        }
    }
    return mini_false();
}

/* (string->fixed str digits) => the leading decimal number of str times
 * 10^digits, rounded, as an integer: ("12.3 %" 1) -> 123, ("-0,5 V" 2) -> -50.
 * Leading spaces are skipped, '.' or ',' separates decimals; #f when str does
 * not start with a number. No flonum is allocated. */
static Value prim_string_to_fixed(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "string->fixed: expected a string and a digit count");
        return mini_void();
    }
    int digits = (argc > 1 && mini_is_fixnum(argv[1])) ? (int)mini_to_fixnum(argv[1]) : 0;
    if (digits < 0 || digits > 9) {
        vm_error(vm, "string->fixed: digits must be 0..9");
        return mini_void();
    }
    const char *p = mini_string_data(argv[0]);
    while (*p == ' ' || *p == '\t') p++;
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = (*p == '-'); p++; }
    if (*p < '0' || *p > '9') return mini_false();
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v < (int64_t)1 << 50) v = v * 10 + (*p - '0');
        p++;
    }
    bool frac = (*p == '.' || *p == ',') && p[1] >= '0' && p[1] <= '9';
    if (frac) p++;
    for (int d = 0; d < digits; d++) {
        v = v * 10;
        if (frac && *p >= '0' && *p <= '9') { v += *p - '0'; p++; }
        else frac = false;
    }
    if (frac && *p >= '5' && *p <= '9') v++; /* round half up on the next digit */
    return bigint_from_int64(vm, neg ? -v : v);
}

/* (string->ints "12,-3,7") => (12 -3 7): the integers of a comma-separated
 * string (series sent by the app proxy), at most STRING_INTS_MAX of them;
 * anything but digits and a sign between commas is skipped */
#define STRING_INTS_MAX 160
static Value prim_string_to_ints(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "string->ints: expected a string");
        return mini_void();
    }
    /* parse first: the string may move once cons collects */
    int64_t nums[STRING_INTS_MAX];
    int n = 0;
    const char *v = mini_string_data(argv[0]);
    while (*v && n < STRING_INTS_MAX) {
        while (*v == ',' || *v == ' ') v++;
        if (!*v) break;
        bool neg = *v == '-';
        if (neg || *v == '+') v++;
        int64_t x = 0;
        bool digits = false;
        while (*v >= '0' && *v <= '9') {
            if (x < 100000000000000LL) x = x * 10 + (*v - '0');
            v++;
            digits = true;
        }
        while (*v && *v != ',') v++;
        if (digits) nums[n++] = neg ? -x : x;
    }
    Value list = mini_nil(), x = mini_void();
    mini_push_root(vm, &list);
    mini_push_root(vm, &x);
    for (int i = n - 1; i >= 0; i--) {
        x = bigint_from_int64(vm, nums[i]);   /* a bignum past the fixnum range */
        list = mini_make_cons(vm, x, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

static Value prim_number_to_string(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || (!mini_is_number(argv[0]))) {
        vm_error(vm, "number->string: expected number");
        return mini_void();
    }
    int radix = 10;
    if (argc >= 2 && mini_is_fixnum(argv[1])) {
        radix = (int)mini_to_fixnum(argv[1]);
    }

    if (mini_is_integer(argv[0])) {
        char *str = bigint_to_str(argv[0], radix);
        if (!str) return mini_void();
        Value res = mini_make_string(vm, str);
        free(str);
        return res;
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", mini_to_flonum(argv[0]));
        return mini_make_string(vm, buf);
    }
}

static Value prim_char_to_integer(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_fixnum(argv[0])) {
        vm_error(vm, "char->integer: expected fixnum/char");
        return mini_void();
    }
    return argv[0];
}

static Value prim_integer_to_char(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_fixnum(argv[0])) {
        vm_error(vm, "integer->char: expected fixnum");
        return mini_void();
    }
    return argv[0];
}

static Value prim_list_to_string(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) {
        vm_error(vm, "list->string: expected list");
        return mini_void();
    }
    size_t len = 0;
    Value curr = argv[0];
    while (mini_is_pair(curr)) {
        len++;
        curr = mini_cdr(curr);
    }
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        vm_error(vm, "list->string: out of memory");
        return mini_void();
    }
    curr = argv[0];
    for (size_t i = 0; i < len; i++) {
        Value c = mini_car(curr);
        buf[i] = (char)(mini_is_fixnum(c) ? mini_to_fixnum(c) : 0);
        curr = mini_cdr(curr);
    }
    buf[len] = '\0';
    Value res = mini_make_string_len(vm, buf, len);
    free(buf);
    return res;
}

static Value prim_string_to_list(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "string->list: expected string");
        return mini_void();
    }
    /* each cons may collect and move the string: its data is read again every time */
    Value str = argv[0];
    size_t len = mini_string_len(str);
    Value list = mini_nil();
    mini_push_root(vm, &str);
    mini_push_root(vm, &list);
    for (size_t i = len; i > 0; i--) {
        Value c = mini_make_fixnum((unsigned char)mini_string_data(str)[i - 1]);
        list = mini_make_cons(vm, c, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

static Value prim_void(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_void();
}

static Value prim_void_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc == 1 && mini_is_void(argv[0])) ? mini_true() : mini_false();
}

static Value prim_eof_object(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_eof();
}

static Value prim_eof_object_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc == 1 && mini_is_eof(argv[0])) ? mini_true() : mini_false();
}

/* ========================================================================= */
/* Engine & Compiler Primitives                                              */
/* ========================================================================= */
static bool extract_bytes(MiniVM *vm, Value val, size_t *out_len, uint8_t **out_buf) {
    if (mini_is_string(val)) {
        *out_len = mini_string_len(val);
        *out_buf = (uint8_t *)malloc(*out_len ? *out_len : 1);
        if (!*out_buf) return false;
        memcpy(*out_buf, mini_string_data(val), *out_len);
        return true;
    }
    if (mini_is_vector(val) || mini_is_pair(val) || val == mini_nil()) {
        size_t len = mini_is_vector(val) ? mini_vector_length(val) : 0;
        if (!mini_is_vector(val)) {
            for (Value c = val; mini_is_pair(c); c = mini_cdr(c)) len++;
        }
        *out_len = len;
        *out_buf = (uint8_t *)malloc(len ? len : 1);
        if (!*out_buf) return false;
        Value c = val;
        for (size_t i = 0; i < len; i++) {
            Value b = mini_is_vector(val) ? mini_vector_ref(vm, val, i) : mini_car(c);
            (*out_buf)[i] = (uint8_t)(mini_is_fixnum(b) ? mini_to_fixnum(b) : 0);
            if (!mini_is_vector(val)) c = mini_cdr(c);
        }
        return true;
    }
    return false;
}

static bool extract_consts(MiniVM *vm, Value val, size_t *out_len, Value **out_buf) {
    if (mini_is_vector(val) || mini_is_pair(val) || val == mini_nil()) {
        size_t len = mini_is_vector(val) ? mini_vector_length(val) : 0;
        if (!mini_is_vector(val)) {
            for (Value c = val; mini_is_pair(c); c = mini_cdr(c)) len++;
        }
        *out_len = len;
        *out_buf = (Value *)malloc(len ? len * sizeof(Value) : sizeof(Value));
        if (!*out_buf) return false;
        Value c = val;
        for (size_t i = 0; i < len; i++) {
            (*out_buf)[i] = mini_is_vector(val) ? mini_vector_ref(vm, val, i) : mini_car(c);
            if (!mini_is_vector(val)) c = mini_cdr(c);
        }
        return true;
    }
    return false;
}

static Value prim_make_code(MiniVM *vm, int argc, Value *argv) {
    if (argc < 6) {
        vm_error(vm, "%make-code: requires 6 arguments (bytes consts num-params max-stack is-varargs has-inner-lambdas)");
        return mini_void();
    }

    size_t code_len = 0, num_consts = 0;
    uint8_t *code_buf = NULL;
    Value *consts_buf = NULL;

    if (!extract_bytes(vm, argv[0], &code_len, &code_buf)) {
        vm_error(vm, "%make-code: invalid bytes representation");
        return mini_void();
    }
    if (!extract_consts(vm, argv[1], &num_consts, &consts_buf)) {
        free(code_buf);
        vm_error(vm, "%make-code: invalid consts representation");
        return mini_void();
    }

    Value code_obj = mini_make_code(vm, (uint32_t)code_len, code_buf,
                                    (uint16_t)(mini_is_fixnum(argv[2]) ? mini_to_fixnum(argv[2]) : 0),
                                    (uint16_t)(mini_is_fixnum(argv[3]) ? mini_to_fixnum(argv[3]) : 64),
                                    (uint8_t)(argv[4] == mini_true()),
                                    (uint8_t)(argv[5] == mini_true()),
                                    (uint16_t)num_consts, consts_buf);

    free(code_buf);
    if (consts_buf) free(consts_buf);
    return code_obj;
}

static Value prim_make_closure(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_code(argv[0])) {
        vm_error(vm, "%make-closure: expected code object");
        return mini_void();
    }
    Value env = (argc >= 2) ? argv[1] : mini_nil();
    return mini_make_closure(vm, argv[0], env);
}

static Value prim_execute(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_error(vm, "%execute: requires closure argument");
        return mini_void();
    }
    Value target = argv[0];
    return mini_call(vm, target, argc - 1, argv + 1);
}

static Value prim_read_file_string(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "%read-file-string")) return mini_void();
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "%%read-file-string: expected string filepath");
        return mini_false();
    }
    const char *filepath = mini_string_data(argv[0]);
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return mini_false();

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return mini_false(); }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return mini_false(); }

    size_t read_bytes = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[read_bytes] = '\0';

    Value str_val = mini_make_string_len(vm, buf, read_bytes);
    free(buf);
    return str_val;
}

/* (include-bytes path): compiled code gets the bytes at compile time
 * (compiler.scm); code run by an older compiler reads them here */
static Value prim_include_bytes(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "include-bytes")) return mini_void();
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "include-bytes: expected a file path");
        return mini_void();
    }
    Value data = prim_read_file_string(vm, argc, argv);
    if (mini_is_false(data)) vm_error(vm, "include-bytes: cannot read file %s", mini_string_data(argv[0]));
    return data;
}

Value prim_write_file_string(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_string(argv[0]) || !mini_is_string(argv[1])) {
        vm_error(vm, "write-file-string: expected (filepath string)");
        return mini_false();
    }
    FILE *fp = fopen(mini_string_data(argv[0]), "wb");
    if (!fp) return mini_false();
    size_t len = mini_string_len(argv[1]);
    size_t written = fwrite(mini_string_data(argv[1]), 1, len, fp);
    fclose(fp);
    return written == len ? mini_true() : mini_false();
}

Value prim_file_size(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1 || !mini_is_string(argv[0])) return mini_make_fixnum(-1);
    FILE *fp = fopen(mini_string_data(argv[0]), "rb");
    if (!fp) return mini_make_fixnum(-1);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    return mini_make_fixnum(sz);
}

Value prim_file_remove(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1 || !mini_is_string(argv[0])) return mini_false();
    return remove(mini_string_data(argv[0])) == 0 ? mini_true() : mini_false();
}

Value prim_system(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "system: expected command string");
        return mini_void();
    }
    int ret = system(mini_string_data(argv[0]));
    return mini_make_fixnum(ret);
}

static Value prim_get_global(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) {
        vm_error(vm, "%get-global: expected 1 argument (symbol or string)");
        return mini_void();
    }
    Value sym = mini_void();
    if (mini_is_symbol(argv[0])) {
        sym = argv[0];
    } else if (mini_is_string(argv[0])) {
        sym = mini_make_symbol(vm, mini_string_data(argv[0]));
    } else {
        vm_error(vm, "%get-global: expected symbol or string");
        return mini_void();
    }
    Value val = env_lookup(vm, vm->global_env, sym);
    return mini_is_void(val) ? global_miss(vm, sym) : val;
}

static Value prim_set_global(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_symbol(argv[0])) {
        vm_error(vm, "%%set-global!: expected symbol and value");
        return mini_void();
    }
    env_define(vm, vm->global_env, argv[0], argv[1]);
    return argv[0];
}

static Value prim_global_names(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    Value list = mini_nil();
    mini_push_root(vm, &list);
    for (Value curr = mini_cdr(vm->global_env); mini_is_pair(curr); curr = mini_cdr(curr)) {
        Value binding = mini_car(curr);
        if (mini_is_pair(binding) && mini_is_symbol(mini_car(binding))) {
            list = mini_make_cons(vm, mini_car(binding), list);
        }
    }
    /* primitives not referenced yet live only in the static tables */
    for (int t = 0; t < vm->prim_table_count; t++) {
        for (size_t i = 0; i < vm->prim_table_sizes[t]; i++) {
            const char *name = vm->prim_tables[t][i].name;
            if (prim_table_find(vm, name) != &vm->prim_tables[t][i]) continue; /* shadowed entry */
            Value sym = mini_make_symbol(vm, name);
            if (!mini_is_void(env_lookup(vm, vm->global_env, sym))) continue;
            list = mini_make_cons(vm, sym, list);
        }
    }
    mini_pop_root(vm);
    return list;
}

/* True for a binding that a primitive table created on first reference */
static bool is_lazy_prim_binding(MiniVM *vm, Value binding) {
    Value v = mini_cdr(binding);
    if (!mini_is_cfunc(v)) return false;
    const MiniPrimEntry *e = prim_table_find(vm, mini_symbol_name(mini_car(binding)));
    return e && e->fn == ((CFuncObj *)UNTAG_PTR(v))->fn;
}

static Value prim_user_globals(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    Value list = mini_nil();
    mini_push_root(vm, &list);
    for (Value curr = mini_cdr(vm->global_env);
         mini_is_pair(curr) && curr != vm->baseline_env;
         curr = mini_cdr(curr)) {
        Value binding = mini_car(curr);
        if (mini_is_pair(binding) && mini_is_symbol(mini_car(binding)) && !is_lazy_prim_binding(vm, binding)) {
            list = mini_make_cons(vm, mini_car(binding), list);
        }
    }
    mini_pop_root(vm);
    return list;
}

static Value prim_load(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "load")) return mini_void();
    if (argc != 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "load: expected string filepath");
        return mini_void();
    }
    return mini_eval_file(vm, mini_string_data(argv[0]));
}

static Value prim_defined_p(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_symbol(argv[0])) {
        vm_error(vm, "defined?: expected symbol");
        return mini_false();
    }
    Value val = env_lookup(vm, vm->global_env, argv[0]);
    if (mini_is_void(val)) val = global_miss(vm, argv[0]);
    return mini_make_bool(!mini_is_void(val));
}

static Value prim_macro_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc == 1 && mini_is_macro(argv[0])) ? mini_true() : mini_false();
}

static Value prim_macro_expand_1(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_void();
}

static Value prim_make_macro(MiniVM *vm, int argc, Value *argv) {
    if (argc != 3) {
        vm_error(vm, "%%make-macro requires 3 arguments: (%%make-macro is-syntax-rules proc-or-literals rules)");
        return mini_void();
    }
    uint32_t is_syntax_rules = mini_to_bool(argv[0]) ? 1 : 0;
    return mini_make_macro(vm, is_syntax_rules, argv[1], argv[2]);
}

/* ========================================================================= */
/* Built-in Primitives Registry                                              */
/* ========================================================================= */

/* ========================================================================= */
/* Baseline List / Char / String Library                                     */
/* ========================================================================= */
/* Always registered so firmware without init.scm matches the WASM simulator. */
/* init.scm may still redefine these with Scheme versions.                    */

static bool val_eqv(Value a, Value b) {
    if (a == b) return true;
    if (mini_is_number(a) && mini_is_number(b)) {
        /* an exact and an inexact number are never eqv? (R7RS): 0 and 0.0 differ */
        if (mini_is_flonum(a) || mini_is_flonum(b)) {
            return mini_is_flonum(a) && mini_is_flonum(b) && mini_to_flonum(a) == mini_to_flonum(b);
        }
        if (mini_is_bigint(a) && mini_is_bigint(b)) return mini_equal(a, b);
        return false;   /* two fixnums (not eq) or a fixnum and a bignum (normalized: never equal) */
    }
    return false;
}

static Value prim_eqv_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return mini_make_bool(argc == 2 && val_eqv(argv[0], argv[1]));
}

#define DEFINE_CHAR_CMP(name, label, op) \
static Value name(MiniVM *vm, int argc, Value *argv) { \
    if (argc < 2) { vm_error(vm, label ": expected 2 arguments"); return mini_void(); } \
    for (int i = 0; i < argc; i++) { \
        if (!mini_is_fixnum(argv[i])) { vm_error(vm, label ": expected characters"); return mini_void(); } \
    } \
    for (int i = 0; i + 1 < argc; i++) { \
        if (!(mini_to_fixnum(argv[i]) op mini_to_fixnum(argv[i + 1]))) return mini_false(); \
    } \
    return mini_true(); \
}

DEFINE_CHAR_CMP(prim_char_eq, "char=?", ==)
DEFINE_CHAR_CMP(prim_char_lt, "char<?", <)
DEFINE_CHAR_CMP(prim_char_gt, "char>?", >)
DEFINE_CHAR_CMP(prim_char_le, "char<=?", <=)
DEFINE_CHAR_CMP(prim_char_ge, "char>=?", >=)

static int string_cmp_vals(Value a, Value b) {
    size_t la = mini_string_len(a), lb = mini_string_len(b);
    int c = memcmp(mini_string_data(a), mini_string_data(b), la < lb ? la : lb);
    if (c != 0) return c;
    return (la < lb) ? -1 : (la > lb ? 1 : 0);
}

#define DEFINE_STRING_CMP(name, label, test) \
static Value name(MiniVM *vm, int argc, Value *argv) { \
    if (argc < 2) { vm_error(vm, label ": expected 2 arguments"); return mini_void(); } \
    for (int i = 0; i < argc; i++) { \
        if (!mini_is_string(argv[i])) { vm_error(vm, label ": expected strings"); return mini_void(); } \
    } \
    for (int i = 0; i + 1 < argc; i++) { \
        int c = string_cmp_vals(argv[i], argv[i + 1]); \
        if (!(test)) return mini_false(); \
    } \
    return mini_true(); \
}

DEFINE_STRING_CMP(prim_string_eq, "string=?", c == 0)
DEFINE_STRING_CMP(prim_string_lt, "string<?", c < 0)
DEFINE_STRING_CMP(prim_string_gt, "string>?", c > 0)

static Value prim_length(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) { vm_error(vm, "length: expected 1 argument"); return mini_void(); }
    int64_t n = 0;
    Value l = argv[0];
    while (mini_is_pair(l)) { n++; l = mini_cdr(l); }
    if (!mini_is_nil(l)) { vm_error(vm, "length: not a proper list"); return mini_void(); }
    return mini_make_fixnum(n);
}

static Value prim_list_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1) return mini_false();
    Value l = argv[0];
    while (mini_is_pair(l)) l = mini_cdr(l);
    return mini_make_bool(mini_is_nil(l));
}

static Value prim_reverse(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1) { vm_error(vm, "reverse: expected 1 argument"); return mini_void(); }
    Value res = mini_nil();
    Value cur = argv[0];
    mini_push_root(vm, &res);
    mini_push_root(vm, &cur);
    while (mini_is_pair(cur)) {
        res = mini_make_cons(vm, mini_car(cur), res);
        cur = mini_cdr(cur);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return res;
}

/* Copies every list but the last, which is shared (R7RS semantics). */
static Value prim_append(MiniVM *vm, int argc, Value *argv) {
    if (argc == 0) return mini_nil();
    Value head = mini_nil(), tail = mini_nil(), cur = mini_nil();
    mini_push_root(vm, &head);
    mini_push_root(vm, &tail);
    mini_push_root(vm, &cur);
    for (int i = 0; i < argc - 1; i++) {
        cur = argv[i];
        while (mini_is_pair(cur)) {
            Value cell = mini_make_cons(vm, mini_car(cur), mini_nil());
            if (mini_is_nil(head)) head = cell; else mini_set_cdr(tail, cell);
            tail = cell;
            cur = mini_cdr(cur);
        }
    }
    if (mini_is_nil(head)) head = argv[argc - 1];
    else mini_set_cdr(tail, argv[argc - 1]);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return head;
}

static Value list_tail_n(MiniVM *vm, Value l, Value k, const char *who) {
    if (!mini_is_fixnum(k) || mini_to_fixnum(k) < 0) {
        vm_error(vm, "%s: index must be a non-negative integer", who);
        return mini_void();
    }
    for (int64_t i = mini_to_fixnum(k); i > 0; i--) {
        if (!mini_is_pair(l)) { vm_error(vm, "%s: index out of range", who); return mini_void(); }
        l = mini_cdr(l);
    }
    return l;
}

static Value prim_list_tail(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2) { vm_error(vm, "list-tail: expected 2 arguments"); return mini_void(); }
    return list_tail_n(vm, argv[0], argv[1], "list-tail");
}

static Value prim_list_ref(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2) { vm_error(vm, "list-ref: expected 2 arguments"); return mini_void(); }
    Value l = list_tail_n(vm, argv[0], argv[1], "list-ref");
    if (vm->has_error) return mini_void();
    if (!mini_is_pair(l)) { vm_error(vm, "list-ref: index out of range"); return mini_void(); }
    return mini_car(l);
}

typedef bool (*ValEqFn)(Value a, Value b);
static bool val_eq_fn(Value a, Value b) { return a == b; }
static bool val_equal_fn(Value a, Value b) { return mini_equal(a, b); }

static Value mem_generic(int argc, Value *argv, ValEqFn eq) {
    if (argc < 2) return mini_false();
    for (Value l = argv[1]; mini_is_pair(l); l = mini_cdr(l)) {
        if (eq(argv[0], mini_car(l))) return l;
    }
    return mini_false();
}

static Value ass_generic(int argc, Value *argv, ValEqFn eq) {
    if (argc < 2) return mini_false();
    for (Value l = argv[1]; mini_is_pair(l); l = mini_cdr(l)) {
        Value entry = mini_car(l);
        if (mini_is_pair(entry) && eq(argv[0], mini_car(entry))) return entry;
    }
    return mini_false();
}

static Value prim_memq(MiniVM *vm, int argc, Value *argv) { (void)vm; return mem_generic(argc, argv, val_eq_fn); }
static Value prim_memv(MiniVM *vm, int argc, Value *argv) { (void)vm; return mem_generic(argc, argv, val_eqv); }
static Value prim_member(MiniVM *vm, int argc, Value *argv) { (void)vm; return mem_generic(argc, argv, val_equal_fn); }
static Value prim_assq(MiniVM *vm, int argc, Value *argv) { (void)vm; return ass_generic(argc, argv, val_eq_fn); }
static Value prim_assv(MiniVM *vm, int argc, Value *argv) { (void)vm; return ass_generic(argc, argv, val_eqv); }
static Value prim_assoc(MiniVM *vm, int argc, Value *argv) { (void)vm; return ass_generic(argc, argv, val_equal_fn); }

#define HOF_MAX_LISTS 4

/* Shared driver for map / for-each over 1..HOF_MAX_LISTS lists. */
static Value hof_map_each(MiniVM *vm, int argc, Value *argv, bool collect, const char *who) {
    if (argc < 2 || argc - 1 > HOF_MAX_LISTS) {
        vm_error(vm, "%s: expected a procedure and 1-%d lists", who, HOF_MAX_LISTS);
        return mini_void();
    }
    int nl = argc - 1;
    Value cur[HOF_MAX_LISTS];
    Value head = mini_nil(), tail = mini_nil();
    for (int i = 0; i < nl; i++) cur[i] = argv[i + 1];
    for (int i = 0; i < nl; i++) mini_push_root(vm, &cur[i]);
    mini_push_root(vm, &head);
    mini_push_root(vm, &tail);
    for (;;) {
        Value args[HOF_MAX_LISTS];
        bool done = false;
        for (int i = 0; i < nl; i++) {
            if (!mini_is_pair(cur[i])) { done = true; break; }
            args[i] = mini_car(cur[i]);
        }
        if (done) break;
        for (int i = 0; i < nl; i++) cur[i] = mini_cdr(cur[i]);
        Value r = mini_call(vm, argv[0], nl, args);
        if (vm->has_error) break;
        if (collect) {
            mini_push_root(vm, &r);
            Value cell = mini_make_cons(vm, r, mini_nil());
            mini_pop_root(vm);
            if (mini_is_nil(head)) head = cell; else mini_set_cdr(tail, cell);
            tail = cell;
        }
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    for (int i = 0; i < nl; i++) mini_pop_root(vm);
    if (vm->has_error) return mini_void();
    return collect ? head : mini_void();
}

static Value prim_map(MiniVM *vm, int argc, Value *argv) { return hof_map_each(vm, argc, argv, true, "map"); }
static Value prim_for_each(MiniVM *vm, int argc, Value *argv) { return hof_map_each(vm, argc, argv, false, "for-each"); }

static Value prim_filter(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2) { vm_error(vm, "filter: expected 2 arguments"); return mini_void(); }
    Value cur = argv[1], head = mini_nil(), tail = mini_nil();
    mini_push_root(vm, &cur);
    mini_push_root(vm, &head);
    mini_push_root(vm, &tail);
    while (mini_is_pair(cur)) {
        Value item = mini_car(cur);
        mini_push_root(vm, &item);
        Value keep = mini_call(vm, argv[0], 1, &item);
        if (!vm->has_error && keep != MINI_CONST_FALSE) {
            Value cell = mini_make_cons(vm, item, mini_nil());
            if (mini_is_nil(head)) head = cell; else mini_set_cdr(tail, cell);
            tail = cell;
        }
        mini_pop_root(vm);
        if (vm->has_error) break;
        cur = mini_cdr(cur);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return vm->has_error ? mini_void() : head;
}

static Value prim_fold_left(MiniVM *vm, int argc, Value *argv) {
    if (argc != 3) { vm_error(vm, "fold-left: expected 3 arguments"); return mini_void(); }
    Value acc = argv[1], cur = argv[2];
    mini_push_root(vm, &acc);
    mini_push_root(vm, &cur);
    while (mini_is_pair(cur)) {
        Value args[2] = { acc, mini_car(cur) };
        cur = mini_cdr(cur);
        acc = mini_call(vm, argv[0], 2, args);
        if (vm->has_error) break;
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return vm->has_error ? mini_void() : acc;
}

static Value prim_fold_right(MiniVM *vm, int argc, Value *argv) {
    if (argc != 3) { vm_error(vm, "fold-right: expected 3 arguments"); return mini_void(); }
    /* Walk a reversed copy so long lists need no C recursion */
    Value rev = mini_nil(), cur = argv[2], acc = argv[1];
    mini_push_root(vm, &rev);
    mini_push_root(vm, &cur);
    mini_push_root(vm, &acc);
    while (mini_is_pair(cur)) {
        rev = mini_make_cons(vm, mini_car(cur), rev);
        cur = mini_cdr(cur);
    }
    while (mini_is_pair(rev)) {
        Value args[2] = { mini_car(rev), acc };
        rev = mini_cdr(rev);
        acc = mini_call(vm, argv[0], 2, args);
        if (vm->has_error) break;
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return vm->has_error ? mini_void() : acc;
}

const BuiltinCFuncEntry BUILTIN_CFUNCS[] = {
    { "call-with-current-continuation", prim_callcc },
    { "call/cc", prim_callcc },
    { "+", prim_add },
    { "-", prim_sub },
    { "*", prim_mul },
    { "/", prim_div },
    { "<", prim_lt },
    { ">", prim_gt },
    { "<=", prim_le },
    { ">=", prim_ge },
    { "=", prim_num_eq },
    { "min", prim_min },
    { "max", prim_max },
    { "modulo", prim_modulo },
    { "remainder", prim_remainder },
    { "quotient", prim_quotient },
    { "cons", prim_cons },
    { "car", prim_car },
    { "%api-id", prim_api_id },
    { "cdr", prim_cdr },
    { "set-car!", prim_set_car },
    { "vm-rom-bytes", prim_vm_rom_bytes },
    { "vm-stats", prim_vm_stats },
    { "vm-stack-trace", prim_vm_stack_trace },
    { "set-cdr!", prim_set_cdr },
    { "list", prim_list },
    { "caar", prim_caar },
    { "cadr", prim_cadr },
    { "cdar", prim_cdar },
    { "cddr", prim_cddr },
    { "caddr", prim_caddr },
    { "cdddr", prim_cdddr },
    { "cadddr", prim_cadddr },
    { "cddddr", prim_cddddr },
    { "pair?", prim_pair_p },
    { "null?", prim_null_p },
    { "number?", prim_number_p },
    { "integer?", prim_integer_p },
    { "real?", prim_number_p },
    { "flonum?", prim_flonum_p },
    { "exact?", prim_exact_p },
    { "inexact?", prim_inexact_p },
    { "exact->inexact", prim_exact_to_inexact },
    { "inexact->exact", prim_inexact_to_exact },
    { "sqrt", prim_sqrt },
    { "sin", prim_sin },
    { "cos", prim_cos },
    { "tan", prim_tan },
    { "asin", prim_asin },
    { "acos", prim_acos },
    { "atan", prim_atan },
    { "exp", prim_exp },
    { "log", prim_log },
    { "expt", prim_expt },
    { "abs", prim_abs },
    { "floor", prim_floor },
    { "ceiling", prim_ceiling },
    { "round", prim_round },
    { "truncate", prim_truncate },
    { "vector?", prim_vector_p },
    { "make-vector", prim_make_vector },
    { "vector", prim_vector },
    { "vector-length", prim_vector_length },
    { "vector-ref", prim_vector_ref },
    { "vector-set!", prim_vector_set },
    { "vector->list", prim_vector_to_list },
    { "list->vector", prim_list_to_vector },
    { "vector-fill!", prim_vector_fill },
    { "symbol?", prim_symbol_p },
    { "string?", prim_string_p },
    { "procedure?", prim_procedure_p },
    { "char?", prim_char_p },
    { "boolean?", prim_boolean_p },
    { "eq?", prim_eq_p },
    { "equal?", prim_equal_p },
    { "not", prim_not },
    { "display", prim_display },
    { "write", prim_write },
    { "newline", prim_newline },
    { "exit", prim_exit },
    { "error", prim_error },
    { "dump-fasl", prim_dump_fasl },
    { "load-fasl", prim_load_fasl },
    { "load-fasl-xip", prim_load_fasl_xip },
    { "xip-active?", prim_xip_active_p },
    { "xip-stats", prim_xip_stats },
    { "string-length", prim_string_length },
    { "string-ref", prim_string_ref },
    { "string-append", prim_string_append },
    { "substring", prim_substring },
    { "symbol->string", prim_symbol_to_string },
    { "string->symbol", prim_string_to_symbol },
    { "string->number", prim_string_to_number },
    { "number->string", prim_number_to_string },
    { "string->fixed", prim_string_to_fixed },
    { "string->ints", prim_string_to_ints },
    { "char->integer", prim_char_to_integer },
    { "integer->char", prim_integer_to_char },
    { "list->string", prim_list_to_string },
    { "string->list", prim_string_to_list },
    { "void", prim_void },
    { "void?", prim_void_p },
    { "eof-object", prim_eof_object },
    { "eof-object?", prim_eof_object_p },
    { "%make-code", prim_make_code },
    { "%make-closure", prim_make_closure },
    { "%execute", prim_execute },
    { "%read-file-string", prim_read_file_string },
    { "include-bytes", prim_include_bytes },
    { "%get-global", prim_get_global },
    { "%set-global!", prim_set_global },
    { "%macro?", prim_macro_p },
    { "%macro-expand-1", prim_macro_expand_1 },
    { "%make-macro", prim_make_macro },
    { "%defined?", prim_defined_p },
    { "defined?", prim_defined_p },
    { "load", prim_load },
    { "apply", prim_apply },
    { "%global-names", prim_global_names },
    { "global-names", prim_global_names },
    { "%user-globals", prim_user_globals },
    { "user-globals", prim_user_globals },
    /* Baseline library (see above) */
    { "eqv?", prim_eqv_p },
    { "char=?", prim_char_eq },
    { "char<?", prim_char_lt },
    { "char>?", prim_char_gt },
    { "char<=?", prim_char_le },
    { "char>=?", prim_char_ge },
    { "string=?", prim_string_eq },
    { "string<?", prim_string_lt },
    { "string>?", prim_string_gt },
    { "length", prim_length },
    { "list?", prim_list_p },
    { "reverse", prim_reverse },
    { "append", prim_append },
    { "list-tail", prim_list_tail },
    { "list-ref", prim_list_ref },
    { "memq", prim_memq },
    { "memv", prim_memv },
    { "member", prim_member },
    { "assq", prim_assq },
    { "assv", prim_assv },
    { "assoc", prim_assoc },
    { "map", prim_map },
    { "for-each", prim_for_each },
    { "filter", prim_filter },
    { "fold-left", prim_fold_left },
    { "fold-right", prim_fold_right },
    { NULL, NULL }
};

void mini_register_primitives(MiniVM *vm) {
    mini_register_prim_table(vm, BUILTIN_CFUNCS, (size_t)-1);
}
