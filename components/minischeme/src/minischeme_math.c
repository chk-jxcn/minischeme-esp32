/**
 * @file minischeme_math.c
 * @brief MiniScheme Numeric & Mathematical Primitives.
 *
 * Implements:
 *  - Arbitrary-precision arithmetic (+, -, *, /, quotient, remainder, modulo).
 *  - Comparison operators (<, >, <=, >=, =).
 *  - Trigonometric, transcendental, and rounding functions.
 *  - Exactness and numeric predicates.
 */

#include "minischeme_internal.h"

/* Two fixnums whose result is a fixnum: no bigint call, no rooted accumulator */
static bool two_fixnums(int argc, const Value *argv) {
    return argc == 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]);
}

static bool small_result(int64_t n, Value *out) {
    if (n < MINI_FIXNUM_MIN || n > MINI_FIXNUM_MAX) return false;
    *out = mini_make_fixnum(n);
    return true;
}

Value prim_add(MiniVM *vm, int argc, Value *argv) {
    Value fast;
    if (two_fixnums(argc, argv) && small_result(mini_to_fixnum(argv[0]) + mini_to_fixnum(argv[1]), &fast)) return fast;
    bool has_float = false;
    for (int i = 0; i < argc; i++) {
        if (mini_is_flonum(argv[i])) {
            has_float = true;
        } else if (!mini_is_integer(argv[i])) {
            vm_error(vm, "+: expected number");
            return mini_void();
        }
    }
    if (has_float) {
        double sum = 0.0;
        for (int i = 0; i < argc; i++) {
            sum += to_double_val(argv[i]);
        }
        return mini_make_flonum(vm, sum);
    }
    Value sum = mini_make_fixnum(0);
    mini_push_root(vm, &sum);
    for (int i = 0; i < argc; i++) {
        sum = bigint_add(vm, sum, argv[i]);
    }
    mini_pop_root(vm);
    return sum;
}

Value prim_sub(MiniVM *vm, int argc, Value *argv) {
    Value fast;
    if (two_fixnums(argc, argv) && small_result(mini_to_fixnum(argv[0]) - mini_to_fixnum(argv[1]), &fast)) return fast;
    if (argc == 0) {
        vm_error(vm, "-: requires at least 1 argument");
        return mini_void();
    }
    bool has_float = false;
    for (int i = 0; i < argc; i++) {
        if (mini_is_flonum(argv[i])) {
            has_float = true;
        } else if (!mini_is_integer(argv[i])) {
            vm_error(vm, "-: expected number");
            return mini_void();
        }
    }
    if (has_float) {
        if (argc == 1) return mini_make_flonum(vm, -to_double_val(argv[0]));
        double diff = to_double_val(argv[0]);
        for (int i = 1; i < argc; i++) diff -= to_double_val(argv[i]);
        return mini_make_flonum(vm, diff);
    }
    if (argc == 1) {
        return bigint_negate(vm, argv[0]);
    }
    Value diff = argv[0];
    mini_push_root(vm, &diff);
    for (int i = 1; i < argc; i++) {
        diff = bigint_sub(vm, diff, argv[i]);
    }
    mini_pop_root(vm);
    return diff;
}

Value prim_mul(MiniVM *vm, int argc, Value *argv) {
    if (two_fixnums(argc, argv)) {
        int64_t x = mini_to_fixnum(argv[0]), y = mini_to_fixnum(argv[1]);
        Value fast;
        /* both under 2^30 in magnitude: the product fits 64 bits on every host */
        if (x > -((int64_t)1 << 30) && x < ((int64_t)1 << 30) && y > -((int64_t)1 << 30) && y < ((int64_t)1 << 30) &&
            small_result(x * y, &fast)) return fast;
    }
    bool has_float = false;
    for (int i = 0; i < argc; i++) {
        if (mini_is_flonum(argv[i])) {
            has_float = true;
        } else if (!mini_is_integer(argv[i])) {
            vm_error(vm, "*: expected number");
            return mini_void();
        }
    }
    if (has_float) {
        double prod = 1.0;
        for (int i = 0; i < argc; i++) prod *= to_double_val(argv[i]);
        return mini_make_flonum(vm, prod);
    }
    Value prod = mini_make_fixnum(1);
    mini_push_root(vm, &prod);
    for (int i = 0; i < argc; i++) {
        prod = bigint_mul(vm, prod, argv[i]);
    }
    mini_pop_root(vm);
    return prod;
}

Value prim_div(MiniVM *vm, int argc, Value *argv) {
    if (argc == 0) {
        vm_error(vm, "/: requires at least 1 argument");
        return mini_void();
    }
    bool has_float = false;
    for (int i = 0; i < argc; i++) {
        if (mini_is_flonum(argv[i])) {
            has_float = true;
        } else if (!mini_is_integer(argv[i])) {
            vm_error(vm, "/: expected number");
            return mini_void();
        }
    }
    if (has_float) {
        if (argc == 1) {
            double v = to_double_val(argv[0]);
            if (v == 0.0) {
                vm_error(vm, "/: division by zero");
                return mini_void();
            }
            return mini_make_flonum(vm, 1.0 / v);
        }
        double quot = to_double_val(argv[0]);
        for (int i = 1; i < argc; i++) {
            double denom = to_double_val(argv[i]);
            if (denom == 0.0) {
                vm_error(vm, "/: division by zero");
                return mini_void();
            }
            quot /= denom;
        }
        return mini_make_flonum(vm, quot);
    }

    if (argc == 1) {
        if (bigint_cmp(argv[0], mini_make_fixnum(0)) == 0) {
            vm_error(vm, "/: division by zero");
            return mini_void();
        }
        if (bigint_cmp(argv[0], mini_make_fixnum(1)) == 0) {
            return mini_make_fixnum(1);
        }
        if (bigint_cmp(argv[0], mini_make_fixnum(-1)) == 0) {
            return mini_make_fixnum(-1);
        }
        return mini_make_flonum(vm, 1.0 / to_double_val(argv[0]));
    }

    /* quot is read again after the division allocated: rooted */
    Value quot = argv[0];
    mini_push_root(vm, &quot);
    for (int i = 1; i < argc; i++) {
        if (bigint_cmp(argv[i], mini_make_fixnum(0)) == 0) {
            mini_pop_root(vm);
            vm_error(vm, "/: division by zero");
            return mini_void();
        }
        Value q, r;
        if (!bigint_div_rem(vm, quot, argv[i], &q, &r)) {
            mini_pop_root(vm);
            return mini_void();
        }
        if (bigint_cmp(r, mini_make_fixnum(0)) == 0) {
            quot = q;
        } else {
            double dquot = to_double_val(quot) / to_double_val(argv[i]);
            for (int j = i + 1; j < argc; j++) {
                double denom = to_double_val(argv[j]);
                if (denom == 0.0) { mini_pop_root(vm); vm_error(vm, "/: division by zero"); return mini_void(); }
                dquot /= denom;
            }
            mini_pop_root(vm);
            return mini_make_flonum(vm, dquot);
        }
    }
    mini_pop_root(vm);
    return quot;
}

/* Fixnum fast path for quotient/remainder/modulo: no bignum temporaries.
 * Returns false (and raises) on division by zero. */
static bool fix_div(MiniVM *vm, const char *who, Value a, Value b, int64_t *q, int64_t *r) {
    int64_t x = mini_to_fixnum(a), y = mini_to_fixnum(b);
    if (y == 0) {
        vm_error(vm, "%s: division by zero", who);
        return false;
    }
    *q = x / y; /* fixnums are at most 61 bits, so no INT64_MIN / -1 */
    *r = x % y;
    return true;
}

Value prim_quotient(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_integer(argv[0]) || !mini_is_integer(argv[1])) {
        vm_error(vm, "quotient: expected 2 integers");
        return mini_void();
    }
    if (mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        int64_t fq, fr;
        if (!fix_div(vm, "quotient", argv[0], argv[1], &fq, &fr)) return mini_void();
        return bigint_from_int64(vm, fq); /* only MIN / -1 leaves fixnum range */
    }
    Value q, r;
    if (!bigint_div_rem(vm, argv[0], argv[1], &q, &r)) return mini_void();
    return q;
}

Value prim_remainder(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_integer(argv[0]) || !mini_is_integer(argv[1])) {
        vm_error(vm, "remainder: expected 2 integers");
        return mini_void();
    }
    if (mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        int64_t fq, fr;
        if (!fix_div(vm, "remainder", argv[0], argv[1], &fq, &fr)) return mini_void();
        return mini_make_fixnum(fr);
    }
    Value q, r;
    if (!bigint_div_rem(vm, argv[0], argv[1], &q, &r)) return mini_void();
    return r;
}

Value prim_modulo(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_integer(argv[0]) || !mini_is_integer(argv[1])) {
        vm_error(vm, "modulo: expected 2 integers");
        return mini_void();
    }
    if (mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        int64_t fq, fr;
        if (!fix_div(vm, "modulo", argv[0], argv[1], &fq, &fr)) return mini_void();
        if (fr != 0 && ((fr < 0) != (mini_to_fixnum(argv[1]) < 0))) fr += mini_to_fixnum(argv[1]);
        return mini_make_fixnum(fr);
    }
    Value q, r;
    if (!bigint_div_rem(vm, argv[0], argv[1], &q, &r)) return mini_void();
    if (bigint_cmp(r, mini_make_fixnum(0)) != 0) {
        int c_r = bigint_cmp(r, mini_make_fixnum(0));
        int c_b = bigint_cmp(argv[1], mini_make_fixnum(0));
        if ((c_r < 0 && c_b > 0) || (c_r > 0 && c_b < 0)) {
            r = bigint_add(vm, r, argv[1]);
        }
    }
    return r;
}

#define DEFINE_NUM_CMP(name, op_name, flt_op, int_cond) \
Value name(MiniVM *vm, int argc, Value *argv) { \
    if (two_fixnums(argc, argv)) return mini_make_bool((intptr_t)argv[0] flt_op (intptr_t)argv[1]); \
    if (argc < 2) return mini_true(); \
    for (int i = 0; i < argc - 1; i++) { \
        if (mini_is_flonum(argv[i]) || mini_is_flonum(argv[i + 1])) { \
            if (!(to_double_val(argv[i]) flt_op to_double_val(argv[i + 1]))) return mini_false(); \
        } else if (mini_is_integer(argv[i]) && mini_is_integer(argv[i + 1])) { \
            if (!(int_cond)) return mini_false(); \
        } else { \
            vm_error(vm, op_name ": expected numbers"); \
            return mini_void(); \
        } \
    } \
    return mini_true(); \
}

DEFINE_NUM_CMP(prim_lt, "<",  <,  bigint_cmp(argv[i], argv[i + 1]) < 0)
DEFINE_NUM_CMP(prim_gt, ">",  >,  bigint_cmp(argv[i], argv[i + 1]) > 0)
DEFINE_NUM_CMP(prim_le, "<=", <=, bigint_cmp(argv[i], argv[i + 1]) <= 0)
DEFINE_NUM_CMP(prim_ge, ">=", >=, bigint_cmp(argv[i], argv[i + 1]) >= 0)
DEFINE_NUM_CMP(prim_num_eq, "=", ==, bigint_cmp(argv[i], argv[i + 1]) == 0)

Value prim_sqrt(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_number(argv[0])) {
        vm_error(vm, "sqrt: expected number");
        return mini_void();
    }
    double v = to_double_val(argv[0]);
    if (v < 0.0) {
        vm_error(vm, "sqrt: negative number");
        return mini_void();
    }
    return mini_make_flonum(vm, sqrt(v));
}

#define DEFINE_UNARY_FLONUM(name, op_name, fn) \
Value name(MiniVM *vm, int argc, Value *argv) { \
    if (argc != 1 || !mini_is_number(argv[0])) { \
        vm_error(vm, op_name ": expected number"); \
        return mini_void(); \
    } \
    return mini_make_flonum(vm, fn(to_double_val(argv[0]))); \
}

DEFINE_UNARY_FLONUM(prim_sin, "sin", sin)
DEFINE_UNARY_FLONUM(prim_cos, "cos", cos)
DEFINE_UNARY_FLONUM(prim_tan, "tan", tan)
DEFINE_UNARY_FLONUM(prim_asin, "asin", asin)
DEFINE_UNARY_FLONUM(prim_acos, "acos", acos)
DEFINE_UNARY_FLONUM(prim_exp, "exp", exp)

Value prim_atan(MiniVM *vm, int argc, Value *argv) {
    if (argc == 1 && mini_is_number(argv[0])) {
        return mini_make_flonum(vm, atan(to_double_val(argv[0])));
    }
    if (argc == 2 && mini_is_number(argv[0]) && mini_is_number(argv[1])) {
        return mini_make_flonum(vm, atan2(to_double_val(argv[0]), to_double_val(argv[1])));
    }
    vm_error(vm, "atan: expected 1 or 2 numbers");
    return mini_void();
}

Value prim_log(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_number(argv[0])) {
        vm_error(vm, "log: expected number");
        return mini_void();
    }
    double v = to_double_val(argv[0]);
    if (v <= 0.0) {
        vm_error(vm, "log: expected positive number");
        return mini_void();
    }
    return mini_make_flonum(vm, log(v));
}

Value prim_expt(MiniVM *vm, int argc, Value *argv) {
    if (argc != 2 || !mini_is_number(argv[0]) || !mini_is_number(argv[1])) {
        vm_error(vm, "expt: expected 2 numbers");
        return mini_void();
    }
    if (mini_is_integer(argv[0]) && mini_is_integer(argv[1])) {
        int64_t exp_val;
        if (bigint_to_int64(argv[1], &exp_val) && exp_val >= 0) {
            Value base = argv[0];
            Value res = mini_make_fixnum(1);
            mini_push_root(vm, &base);
            mini_push_root(vm, &res);
            while (exp_val > 0) {
                if (exp_val & 1) {
                    res = bigint_mul(vm, res, base);
                }
                base = bigint_mul(vm, base, base);
                exp_val >>= 1;
            }
            mini_pop_root(vm);
            mini_pop_root(vm);
            return res;
        }
    }
    return mini_make_flonum(vm, pow(to_double_val(argv[0]), to_double_val(argv[1])));
}

Value prim_abs(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_number(argv[0])) {
        vm_error(vm, "abs: expected number");
        return mini_void();
    }
    if (mini_is_flonum(argv[0])) {
        return mini_make_flonum(vm, fabs(mini_to_flonum(argv[0])));
    }
    return bigint_abs(vm, argv[0]);
}

#define DEFINE_ROUND_FN(name, op_name, fn) \
Value name(MiniVM *vm, int argc, Value *argv) { \
    if (argc != 1 || !mini_is_number(argv[0])) { \
        vm_error(vm, op_name ": expected number"); \
        return mini_void(); \
    } \
    if (mini_is_integer(argv[0])) return argv[0]; \
    return mini_make_flonum(vm, fn(to_double_val(argv[0]))); \
}

DEFINE_ROUND_FN(prim_floor, "floor", floor)
DEFINE_ROUND_FN(prim_ceiling, "ceiling", ceil)
DEFINE_ROUND_FN(prim_round, "round", round)
DEFINE_ROUND_FN(prim_truncate, "truncate", trunc)

Value prim_exact_to_inexact(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_number(argv[0])) {
        vm_error(vm, "exact->inexact: expected number");
        return mini_void();
    }
    return mini_make_flonum(vm, to_double_val(argv[0]));
}

Value prim_inexact_to_exact(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !mini_is_number(argv[0])) {
        vm_error(vm, "inexact->exact: expected number");
        return mini_void();
    }
    if (mini_is_integer(argv[0])) return argv[0];
    double d = round(to_double_val(argv[0]));
    if (d != d || d - d != 0.0) {        /* NaN or infinite: no exact integer */
        vm_error(vm, "inexact->exact: no exact integer for %g", d);
        return mini_void();
    }
    if (fabs(d) < 9.0e18) return bigint_from_int64(vm, (int64_t)d);
    /* beyond int64: the double's integer value, 32 bits at a time (exact:
     * dividing by a power of two loses nothing) */
    uint32_t limbs[32];
    uint32_t n = 0;
    double m = fabs(d);
    while (m >= 1.0 && n < 32) {
        double q = floor(m / 4294967296.0);
        limbs[n++] = (uint32_t)(m - q * 4294967296.0);
        m = q;
    }
    return bigint_from_limbs(vm, d < 0 ? -1 : 1, limbs, n);
}

Value prim_integer_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1) return mini_false();
    return mini_make_bool(mini_is_integer(argv[0]));
}

Value prim_flonum_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1) return mini_false();
    return mini_make_bool(mini_is_flonum(argv[0]));
}

Value prim_exact_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1) return mini_false();
    return mini_make_bool(mini_is_integer(argv[0]));
}

Value prim_inexact_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc != 1) return mini_false();
    return mini_make_bool(mini_is_flonum(argv[0]));
}

Value prim_number_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc == 1 && mini_is_number(argv[0])) ? mini_true() : mini_false();
}

Value prim_min(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_error(vm, "min: requires at least 1 argument");
        return mini_void();
    }
    Value min_val = argv[0];
    if (!mini_is_number(min_val)) {
        vm_error(vm, "min: expected number");
        return mini_void();
    }
    for (int i = 1; i < argc; i++) {
        if (!mini_is_number(argv[i])) {
            vm_error(vm, "min: expected number");
            return mini_void();
        }
        Value cmp_args[2] = { argv[i], min_val };
        Value cmp_res = prim_lt(vm, 2, cmp_args);
        if (mini_to_bool(cmp_res)) {
            min_val = argv[i];
        }
    }
    return min_val;
}

Value prim_max(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1) {
        vm_error(vm, "max: requires at least 1 argument");
        return mini_void();
    }
    Value max_val = argv[0];
    if (!mini_is_number(max_val)) {
        vm_error(vm, "max: expected number");
        return mini_void();
    }
    for (int i = 1; i < argc; i++) {
        if (!mini_is_number(argv[i])) {
            vm_error(vm, "max: expected number");
            return mini_void();
        }
        Value cmp_args[2] = { argv[i], max_val };
        Value cmp_res = prim_gt(vm, 2, cmp_args);
        if (mini_to_bool(cmp_res)) {
            max_val = argv[i];
        }
    }
    return max_val;
}

