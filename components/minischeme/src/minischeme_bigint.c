/**
 * @file minischeme_bigint.c
 * @brief MiniScheme Arbitrary-Precision Integer (BigInt) Engine.
 *
 * Implements:
 *  - 32-bit limb little-endian arbitrary-precision integers.
 *  - Seamless fixnum <-> bigint overflow promotion and normalization.
 *  - Addition, subtraction, multiplication, truncating division, remainder, modulo.
 *  - Arbitrary-length decimal string parsing and formatting.
 *  - Leaf object storage (zero pointer fields) for instant Cheney GC and FASL relocation.
 *
 * Every allocation may collect and move the operands: an operand read after
 * an allocation is rooted (mini_push_root) and its digits are re-read from
 * the rooted Value; the add / mul / div kernels copy the digits into C
 * buffers before they allocate their result.
 */

#include "minischeme_internal.h"

/* Ensure limb digits are 32-bit */
typedef uint32_t Limb;
typedef uint64_t DoubleLimb;

/* ========================================================================= */
/* Allocation and Normalization                                              */
/* ========================================================================= */

Value bigint_alloc(MiniVM *vm, int32_t sign, uint32_t length) {
    if (length == 0) {
        return mini_make_fixnum(0);
    }
    size_t sz = sizeof(BigIntObj) + (length > 1 ? (size_t)(length - 1) * sizeof(Limb) : 0);
    BigIntObj *b = (BigIntObj *)gc_alloc(vm, sz, OBJ_BIGINT);
    if (!b) return mini_void();
    b->header.type = OBJ_BIGINT;
    b->header.size = (uint32_t)sz;
    b->sign = sign;
    b->length = length;
    memset(b->digits, 0, length * sizeof(Limb));
    return ((uintptr_t)b) | MINI_TAG_RECORD;
}

Value bigint_normalize(MiniVM *vm, Value v) {
    (void)vm;
    if (!mini_is_bigint(v)) return v;
    BigIntObj *b = (BigIntObj *)UNTAG_PTR(v);

    /* Trim leading zeros */
    uint32_t len = b->length;
    while (len > 0 && b->digits[len - 1] == 0) {
        len--;
    }
    b->length = len;

    if (len == 0 || b->sign == 0) {
        return mini_make_fixnum(0);
    }

    /* Check if fits in 61-bit Fixnum */
    if (len == 1) {
        int64_t val = (int64_t)b->digits[0] * b->sign;
        if (val >= MINI_FIXNUM_MIN && val <= MINI_FIXNUM_MAX) {
            return mini_make_fixnum(val);
        }
    } else if (len == 2) {
        uint64_t mag = ((uint64_t)b->digits[1] << 32) | b->digits[0];
        if (b->sign > 0 && mag <= (uint64_t)MINI_FIXNUM_MAX) {
            return mini_make_fixnum((int64_t)mag);
        }
        if (b->sign < 0 && mag <= (uint64_t)(-(int64_t)MINI_FIXNUM_MIN)) {
            return mini_make_fixnum(-(int64_t)mag);
        }
    }

    return v;
}

Value bigint_from_limbs(MiniVM *vm, int32_t sign, const uint32_t *limbs, uint32_t n) {
    while (n > 0 && limbs[n - 1] == 0) n--;
    if (n == 0) return mini_make_fixnum(0);
    Value v = bigint_alloc(vm, sign < 0 ? -1 : 1, n);
    if (mini_is_void(v)) return v;
    memcpy(((BigIntObj *)UNTAG_PTR(v))->digits, limbs, n * sizeof(Limb));
    return bigint_normalize(vm, v);
}

Value bigint_from_int64(MiniVM *vm, int64_t n) {
    if (n >= MINI_FIXNUM_MIN && n <= MINI_FIXNUM_MAX) {
        return mini_make_fixnum(n);
    }
    int32_t sign = (n < 0) ? -1 : 1;
    uint64_t mag;
    if (n == INT64_MIN) {
        mag = (uint64_t)INT64_MAX + 1ULL;
    } else if (n < 0) {
        mag = (uint64_t)(-n);
    } else {
        mag = (uint64_t)n;
    }

    uint32_t d0 = (uint32_t)(mag & 0xFFFFFFFFULL);
    uint32_t d1 = (uint32_t)(mag >> 32);
    uint32_t len = (d1 != 0) ? 2 : 1;

    Value v = bigint_alloc(vm, sign, len);
    if (mini_is_void(v)) return v;
    BigIntObj *b = (BigIntObj *)UNTAG_PTR(v);
    b->digits[0] = d0;
    if (len > 1) b->digits[1] = d1;
    return v;
}

bool bigint_to_int64(Value v, int64_t *out) {
    if (mini_is_fixnum(v)) {
        if (out) *out = mini_to_fixnum(v);
        return true;
    }
    if (!mini_is_bigint(v)) return false;
    BigIntObj *b = (BigIntObj *)UNTAG_PTR(v);
    if (b->length == 0) {
        if (out) *out = 0;
        return true;
    }
    if (b->length == 1) {
        if (out) *out = (int64_t)b->digits[0] * b->sign;
        return true;
    }
    if (b->length == 2) {
        uint64_t mag = ((uint64_t)b->digits[1] << 32) | b->digits[0];
        if (b->sign > 0 && mag <= (uint64_t)INT64_MAX) {
            if (out) *out = (int64_t)mag;
            return true;
        }
        if (b->sign < 0 && mag <= (uint64_t)INT64_MAX + 1ULL) {
            if (out) *out = -(int64_t)mag;
            return true;
        }
    }
    return false;
}

double bigint_to_double(Value v) {
    if (mini_is_fixnum(v)) return (double)mini_to_fixnum(v);
    if (!mini_is_bigint(v)) return 0.0;
    BigIntObj *b = (BigIntObj *)UNTAG_PTR(v);
    double res = 0.0;
    double base = 1.0;
    for (uint32_t i = 0; i < b->length; i++) {
        res += (double)b->digits[i] * base;
        base *= 4294967296.0; /* 2^32 */
    }
    return res * b->sign;
}

/* ========================================================================= */
/* Limb Level Unsigned Helpers                                               */
/* ========================================================================= */

static int cmp_limbs(const Limb *a, uint32_t la, const Limb *b, uint32_t lb) {
    while (la > 0 && a[la - 1] == 0) la--;
    while (lb > 0 && b[lb - 1] == 0) lb--;
    if (la != lb) return (la > lb) ? 1 : -1;
    for (int i = (int)la - 1; i >= 0; i--) {
        if (a[i] != b[i]) return (a[i] > b[i]) ? 1 : -1;
    }
    return 0;
}

static void add_limbs(const Limb *a, uint32_t la, const Limb *b, uint32_t lb, Limb *res, uint32_t *out_len) {
    uint32_t max_len = (la > lb) ? la : lb;
    DoubleLimb carry = 0;
    for (uint32_t i = 0; i < max_len; i++) {
        DoubleLimb da = (i < la) ? a[i] : 0;
        DoubleLimb db = (i < lb) ? b[i] : 0;
        DoubleLimb sum = da + db + carry;
        res[i] = (Limb)sum;
        carry = sum >> 32;
    }
    if (carry > 0) {
        res[max_len++] = (Limb)carry;
    }
    *out_len = max_len;
}

static void sub_limbs(const Limb *a, uint32_t la, const Limb *b, uint32_t lb, Limb *res, uint32_t *out_len) {
    /* Assumes magnitude A >= magnitude B */
    int64_t borrow = 0;
    for (uint32_t i = 0; i < la; i++) {
        int64_t da = (int64_t)a[i];
        int64_t db = (i < lb) ? (int64_t)b[i] : 0;
        int64_t diff = da - db - borrow;
        if (diff < 0) {
            diff += 0x100000000LL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        res[i] = (Limb)diff;
    }
    while (la > 0 && res[la - 1] == 0) la--;
    *out_len = la;
}

static void mul_limbs(const Limb *a, uint32_t la, const Limb *b, uint32_t lb, Limb *res, uint32_t *out_len) {
    uint32_t rlen = la + lb;
    memset(res, 0, rlen * sizeof(Limb));
    for (uint32_t i = 0; i < la; i++) {
        DoubleLimb carry = 0;
        for (uint32_t j = 0; j < lb; j++) {
            DoubleLimb cur = (DoubleLimb)res[i + j] + (DoubleLimb)a[i] * (DoubleLimb)b[j] + carry;
            res[i + j] = (Limb)cur;
            carry = cur >> 32;
        }
        res[i + lb] += (Limb)carry;
    }
    while (rlen > 0 && res[rlen - 1] == 0) rlen--;
    *out_len = rlen;
}

/* ========================================================================= */
/* Arithmetic Operations                                                     */
/* ========================================================================= */

int bigint_cmp(Value a, Value b) {
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        int64_t na = mini_to_fixnum(a);
        int64_t nb = mini_to_fixnum(b);
        if (na == nb) return 0;
        return (na > nb) ? 1 : -1;
    }

    int32_t sa = mini_is_fixnum(a) ? (mini_to_fixnum(a) < 0 ? -1 : (mini_to_fixnum(a) > 0 ? 1 : 0))
                                  : ((BigIntObj *)UNTAG_PTR(a))->sign;
    int32_t sb = mini_is_fixnum(b) ? (mini_to_fixnum(b) < 0 ? -1 : (mini_to_fixnum(b) > 0 ? 1 : 0))
                                  : ((BigIntObj *)UNTAG_PTR(b))->sign;
    if (sa != sb) return (sa > sb) ? 1 : -1;
    if (sa == 0) return 0;

    Limb a_buf[2], b_buf[2];
    const Limb *la_ptr; uint32_t la_len;
    const Limb *lb_ptr; uint32_t lb_len;

    if (mini_is_fixnum(a)) {
        int64_t n = mini_to_fixnum(a);
        uint64_t mag = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        a_buf[0] = (Limb)(mag & 0xFFFFFFFFULL);
        a_buf[1] = (Limb)(mag >> 32);
        la_ptr = a_buf;
        la_len = a_buf[1] ? 2 : 1;
    } else {
        BigIntObj *ba = (BigIntObj *)UNTAG_PTR(a);
        la_ptr = ba->digits;
        la_len = ba->length;
    }

    if (mini_is_fixnum(b)) {
        int64_t n = mini_to_fixnum(b);
        uint64_t mag = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        b_buf[0] = (Limb)(mag & 0xFFFFFFFFULL);
        b_buf[1] = (Limb)(mag >> 32);
        lb_ptr = b_buf;
        lb_len = b_buf[1] ? 2 : 1;
    } else {
        BigIntObj *bb = (BigIntObj *)UNTAG_PTR(b);
        lb_ptr = bb->digits;
        lb_len = bb->length;
    }

    int c = cmp_limbs(la_ptr, la_len, lb_ptr, lb_len);
    return (sa > 0) ? c : -c;
}

Value bigint_negate(MiniVM *vm, Value v) {
    if (mini_is_fixnum(v)) {
        int64_t n = mini_to_fixnum(v);
        if (n > MINI_FIXNUM_MIN) {
            return mini_make_fixnum(-n);
        }
        return bigint_from_int64(vm, -n);
    }
    if (!mini_is_bigint(v)) return v;
    BigIntObj *b = (BigIntObj *)UNTAG_PTR(v);
    if (b->length == 0 || b->sign == 0) return mini_make_fixnum(0);

    /* the allocation may move v (and put the result where it was): its
     * digits are read again through the rooted Value */
    mini_push_root(vm, &v);
    Value res = bigint_alloc(vm, -b->sign, b->length);
    mini_pop_root(vm);
    if (mini_is_void(res)) return res;
    b = (BigIntObj *)UNTAG_PTR(v);
    BigIntObj *rb = (BigIntObj *)UNTAG_PTR(res);
    memcpy(rb->digits, b->digits, b->length * sizeof(Limb));
    return bigint_normalize(vm, res);
}

Value bigint_abs(MiniVM *vm, Value v) {
    if (bigint_cmp(v, mini_make_fixnum(0)) < 0) {
        return bigint_negate(vm, v);
    }
    return v;
}

Value bigint_add(MiniVM *vm, Value a, Value b) {
    /* Fast path: Fixnum + Fixnum without 61-bit overflow */
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        int64_t na = mini_to_fixnum(a);
        int64_t nb = mini_to_fixnum(b);
        int64_t sum = na + nb;
        if ((nb > 0 && sum > na && sum <= MINI_FIXNUM_MAX) ||
            (nb <= 0 && sum <= na && sum >= MINI_FIXNUM_MIN)) {
            return mini_make_fixnum(sum);
        }
    }

    /* Convert operands to limbs */
    Limb a_buf[2], b_buf[2];
    const Limb *la; uint32_t la_len; int32_t sa;
    const Limb *lb; uint32_t lb_len; int32_t sb;

    if (mini_is_fixnum(a)) {
        int64_t n = mini_to_fixnum(a);
        sa = (n < 0) ? -1 : (n > 0 ? 1 : 0);
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        a_buf[0] = (Limb)m; a_buf[1] = (Limb)(m >> 32);
        la = a_buf; la_len = a_buf[1] ? 2 : (a_buf[0] ? 1 : 0);
    } else {
        BigIntObj *ba = (BigIntObj *)UNTAG_PTR(a);
        la = ba->digits; la_len = ba->length; sa = ba->sign;
    }

    if (mini_is_fixnum(b)) {
        int64_t n = mini_to_fixnum(b);
        sb = (n < 0) ? -1 : (n > 0 ? 1 : 0);
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        b_buf[0] = (Limb)m; b_buf[1] = (Limb)(m >> 32);
        lb = b_buf; lb_len = b_buf[1] ? 2 : (b_buf[0] ? 1 : 0);
    } else {
        BigIntObj *bb = (BigIntObj *)UNTAG_PTR(b);
        lb = bb->digits; lb_len = bb->length; sb = bb->sign;
    }

    if (sa == 0) return b;
    if (sb == 0) return a;

    uint32_t max_len = (la_len > lb_len ? la_len : lb_len) + 1;
    Limb *temp = (Limb *)malloc(max_len * sizeof(Limb));
    if (!temp) return mini_void();

    int32_t sres;
    uint32_t rlen;

    if (sa == sb) {
        add_limbs(la, la_len, lb, lb_len, temp, &rlen);
        sres = sa;
    } else {
        int c = cmp_limbs(la, la_len, lb, lb_len);
        if (c == 0) {
            free(temp);
            return mini_make_fixnum(0);
        } else if (c > 0) {
            sub_limbs(la, la_len, lb, lb_len, temp, &rlen);
            sres = sa;
        } else {
            sub_limbs(lb, lb_len, la, la_len, temp, &rlen);
            sres = sb;
        }
    }

    Value res = bigint_alloc(vm, sres, rlen);
    if (!mini_is_void(res)) {
        BigIntObj *rb = (BigIntObj *)UNTAG_PTR(res);
        memcpy(rb->digits, temp, rlen * sizeof(Limb));
    }
    free(temp);
    return bigint_normalize(vm, res);
}

Value bigint_sub(MiniVM *vm, Value a, Value b) {
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        int64_t na = mini_to_fixnum(a);
        int64_t nb = mini_to_fixnum(b);
        int64_t diff = na - nb;
        if ((nb < 0 && diff > na && diff <= MINI_FIXNUM_MAX) ||
            (nb >= 0 && diff <= na && diff >= MINI_FIXNUM_MIN)) {
            return mini_make_fixnum(diff);
        }
    }
    /* a stays rooted while -b is allocated (a collection there moves it) */
    mini_push_root(vm, &a);
    Value neg_b = bigint_negate(vm, b);
    mini_pop_root(vm);
    return bigint_add(vm, a, neg_b);
}

Value bigint_mul(MiniVM *vm, Value a, Value b) {
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        int64_t na = mini_to_fixnum(a);
        int64_t nb = mini_to_fixnum(b);
        if (na == 0 || nb == 0) return mini_make_fixnum(0);
        int64_t prod = na * nb;
        if (prod / na == nb && prod >= MINI_FIXNUM_MIN && prod <= MINI_FIXNUM_MAX) {
            return mini_make_fixnum(prod);
        }
    }

    Limb a_buf[2], b_buf[2];
    const Limb *la; uint32_t la_len; int32_t sa;
    const Limb *lb; uint32_t lb_len; int32_t sb;

    if (mini_is_fixnum(a)) {
        int64_t n = mini_to_fixnum(a);
        sa = (n < 0) ? -1 : (n > 0 ? 1 : 0);
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        a_buf[0] = (Limb)m; a_buf[1] = (Limb)(m >> 32);
        la = a_buf; la_len = a_buf[1] ? 2 : (a_buf[0] ? 1 : 0);
    } else {
        BigIntObj *ba = (BigIntObj *)UNTAG_PTR(a);
        la = ba->digits; la_len = ba->length; sa = ba->sign;
    }

    if (mini_is_fixnum(b)) {
        int64_t n = mini_to_fixnum(b);
        sb = (n < 0) ? -1 : (n > 0 ? 1 : 0);
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        b_buf[0] = (Limb)m; b_buf[1] = (Limb)(m >> 32);
        lb = b_buf; lb_len = b_buf[1] ? 2 : (b_buf[0] ? 1 : 0);
    } else {
        BigIntObj *bb = (BigIntObj *)UNTAG_PTR(b);
        lb = bb->digits; lb_len = bb->length; sb = bb->sign;
    }

    if (sa == 0 || sb == 0) return mini_make_fixnum(0);

    uint32_t rlen = la_len + lb_len;
    Limb *temp = (Limb *)calloc(rlen, sizeof(Limb));
    if (!temp) return mini_void();

    uint32_t actual_len;
    mul_limbs(la, la_len, lb, lb_len, temp, &actual_len);

    Value res = bigint_alloc(vm, sa * sb, actual_len);
    if (!mini_is_void(res)) {
        BigIntObj *rb = (BigIntObj *)UNTAG_PTR(res);
        memcpy(rb->digits, temp, actual_len * sizeof(Limb));
    }
    free(temp);
    return bigint_normalize(vm, res);
}

/* ========================================================================= */
/* Division and Remainder                                                    */
/* ========================================================================= */

/* Bitwise shift-and-subtract division for arbitrary limb count */
static void div_rem_limbs(const Limb *a, uint32_t la, const Limb *b, uint32_t lb,
                          Limb *q, uint32_t *out_ql, Limb *r, uint32_t *out_rl) {
    while (la > 0 && a[la - 1] == 0) la--;
    while (lb > 0 && b[lb - 1] == 0) lb--;

    if (lb == 0) {
        *out_ql = 0; *out_rl = 0;
        return;
    }

    if (cmp_limbs(a, la, b, lb) < 0) {
        *out_ql = 0;
        memcpy(r, a, la * sizeof(Limb));
        *out_rl = la;
        return;
    }

    /* Fast path: single limb divisor */
    if (lb == 1) {
        DoubleLimb rem = 0;
        Limb divisor = b[0];
        for (int i = (int)la - 1; i >= 0; i--) {
            DoubleLimb cur = (rem << 32) | a[i];
            q[i] = (Limb)(cur / divisor);
            rem = cur % divisor;
        }
        uint32_t ql = la;
        while (ql > 0 && q[ql - 1] == 0) ql--;
        *out_ql = ql;
        r[0] = (Limb)rem;
        *out_rl = (rem != 0) ? 1 : 0;
        return;
    }

    /* Multi-limb: bit-by-bit division */
    memset(q, 0, la * sizeof(Limb));
    memset(r, 0, (la + 1) * sizeof(Limb));
    uint32_t rl = 0;

    int total_bits = (int)la * 32;
    for (int bit = total_bits - 1; bit >= 0; bit--) {
        /* Shift R left by 1 */
        DoubleLimb carry = 0;
        for (uint32_t i = 0; i < rl; i++) {
            DoubleLimb cur = ((DoubleLimb)r[i] << 1) | carry;
            r[i] = (Limb)cur;
            carry = cur >> 32;
        }
        if (carry > 0) {
            r[rl++] = (Limb)carry;
        }

        /* Insert bit from A into R */
        int limb_idx = bit / 32;
        int bit_idx = bit % 32;
        if ((a[limb_idx] >> bit_idx) & 1) {
            r[0] |= 1;
            if (rl == 0) rl = 1;
        }

        /* If R >= B: R = R - B, set bit in Q */
        if (cmp_limbs(r, rl, b, lb) >= 0) {
            uint32_t new_rl;
            sub_limbs(r, rl, b, lb, r, &new_rl);
            rl = new_rl;
            q[limb_idx] |= ((Limb)1 << bit_idx);
        }
    }

    uint32_t ql = la;
    while (ql > 0 && q[ql - 1] == 0) ql--;
    *out_ql = ql;
    while (rl > 0 && r[rl - 1] == 0) rl--;
    *out_rl = rl;
}

bool bigint_div_rem(MiniVM *vm, Value a, Value b, Value *out_quot, Value *out_rem) {
    if (bigint_cmp(b, mini_make_fixnum(0)) == 0) {
        vm_error(vm, "division by zero");
        return false;
    }

    Limb a_buf[2], b_buf[2];
    const Limb *la; uint32_t la_len; int32_t sa;
    const Limb *lb; uint32_t lb_len; int32_t sb;

    if (mini_is_fixnum(a)) {
        int64_t n = mini_to_fixnum(a);
        sa = (n < 0) ? -1 : (n > 0 ? 1 : 0);
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        a_buf[0] = (Limb)m; a_buf[1] = (Limb)(m >> 32);
        la = a_buf; la_len = a_buf[1] ? 2 : (a_buf[0] ? 1 : 0);
    } else {
        BigIntObj *ba = (BigIntObj *)UNTAG_PTR(a);
        la = ba->digits; la_len = ba->length; sa = ba->sign;
    }

    if (mini_is_fixnum(b)) {
        int64_t n = mini_to_fixnum(b);
        sb = (n < 0) ? -1 : (n > 0 ? 1 : 0);
        uint64_t m = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
        b_buf[0] = (Limb)m; b_buf[1] = (Limb)(m >> 32);
        lb = b_buf; lb_len = b_buf[1] ? 2 : (b_buf[0] ? 1 : 0);
    } else {
        BigIntObj *bb = (BigIntObj *)UNTAG_PTR(b);
        lb = bb->digits; lb_len = bb->length; sb = bb->sign;
    }

    Limb *q_limbs = (Limb *)calloc(la_len + 1, sizeof(Limb));
    Limb *r_limbs = (Limb *)calloc(la_len + 2, sizeof(Limb));
    uint32_t ql, rl;

    div_rem_limbs(la, la_len, lb, lb_len, q_limbs, &ql, r_limbs, &rl);

    /* the quotient stays rooted while the remainder is allocated */
    Value quot = mini_void(), rem = mini_void();
    mini_push_root(vm, &quot);
    if (out_quot) {
        int32_t sq = (ql == 0) ? 1 : (sa * sb);
        quot = bigint_alloc(vm, sq, ql);
        if (!mini_is_void(quot)) {
            BigIntObj *qb = (BigIntObj *)UNTAG_PTR(quot);
            memcpy(qb->digits, q_limbs, ql * sizeof(Limb));
            quot = bigint_normalize(vm, quot);
        }
    }

    if (out_rem) {
        int32_t sr = (rl == 0) ? 1 : sa; /* Scheme remainder has sign of dividend */
        rem = bigint_alloc(vm, sr, rl);
        if (!mini_is_void(rem)) {
            BigIntObj *rb = (BigIntObj *)UNTAG_PTR(rem);
            memcpy(rb->digits, r_limbs, rl * sizeof(Limb));
            rem = bigint_normalize(vm, rem);
        }
    }
    mini_pop_root(vm);
    if (out_quot) *out_quot = quot;
    if (out_rem) *out_rem = rem;

    free(q_limbs);
    free(r_limbs);
    return true;
}

/* ========================================================================= */
/* String Conversions                                                        */
/* ========================================================================= */

static int char_to_digit(char c, int radix) {
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    if (d >= 0 && d < radix) return d;
    return -1;
}

Value bigint_from_str(MiniVM *vm, const char *str, int radix) {
    if (!str || *str == '\0' || radix < 2 || radix > 36) return mini_false();

    const char *p = str;
    int32_t sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (char_to_digit(*p, radix) < 0) return mini_false();

    /* Start with BigInt 0 */
    Value acc = mini_make_fixnum(0);
    Value radix_val = mini_make_fixnum(radix);

    while (*p) {
        int digit = char_to_digit(*p, radix);
        if (digit < 0) return mini_false();
        acc = bigint_mul(vm, acc, radix_val);
        acc = bigint_add(vm, acc, mini_make_fixnum(digit));
        p++;
    }

    if (sign < 0) {
        acc = bigint_negate(vm, acc);
    }
    return acc;
}

char *bigint_to_str(Value v, int radix) {
    if (radix < 2 || radix > 36) return NULL;
    if (mini_is_fixnum(v)) {
        char buf[128];
        int64_t val = (int64_t)mini_to_fixnum(v);
        if (radix == 10) {
            snprintf(buf, sizeof(buf), "%lld", (long long)val);
            return vm_strdup(buf);
        } else if (radix == 16) {
            if (val < 0) snprintf(buf, sizeof(buf), "-%llx", (unsigned long long)(-val));
            else snprintf(buf, sizeof(buf), "%llx", (unsigned long long)val);
            return vm_strdup(buf);
        } else if (radix == 8) {
            if (val < 0) snprintf(buf, sizeof(buf), "-%llo", (unsigned long long)(-val));
            else snprintf(buf, sizeof(buf), "%llo", (unsigned long long)val);
            return vm_strdup(buf);
        } else if (radix == 2) {
            char bbuf[70];
            int pos = 0;
            uint64_t uval = (val < 0) ? (uint64_t)(-val) : (uint64_t)val;
            if (uval == 0) return vm_strdup("0");
            while (uval > 0) {
                bbuf[pos++] = (uval & 1) ? '1' : '0';
                uval >>= 1;
            }
            char res[80];
            int rpos = 0;
            if (val < 0) res[rpos++] = '-';
            for (int i = pos - 1; i >= 0; i--) res[rpos++] = bbuf[i];
            res[rpos] = '\0';
            return vm_strdup(res);
        }
    }
    if (radix != 10) return NULL;
    if (!mini_is_bigint(v)) return NULL;

    BigIntObj *b = (BigIntObj *)UNTAG_PTR(v);
    if (b->length == 0 || b->sign == 0) {
        return vm_strdup("0");
    }

    /* Allocate working copy of limbs */
    uint32_t len = b->length;
    Limb *limbs = (Limb *)malloc(len * sizeof(Limb));
    if (!limbs) return NULL;
    memcpy(limbs, b->digits, len * sizeof(Limb));

    /* Worst case decimal digits: len * 10 + 2 */
    size_t cap = len * 10 + 16;
    char *digits = (char *)malloc(cap);
    if (!digits) { free(limbs); return NULL; }
    size_t dpos = 0;

    /* Repeatedly divide by 10^9 */
    const Limb base10_9 = 1000000000U;
    while (len > 0) {
        DoubleLimb rem = 0;
        for (int i = (int)len - 1; i >= 0; i--) {
            DoubleLimb cur = (rem << 32) | limbs[i];
            limbs[i] = (Limb)(cur / base10_9);
            rem = cur % base10_9;
        }
        while (len > 0 && limbs[len - 1] == 0) len--;

        uint32_t chunk = (uint32_t)rem;
        if (len > 0) {
            /* Need 9 full digits with leading zeros */
            for (int k = 0; k < 9; k++) {
                digits[dpos++] = '0' + (chunk % 10);
                chunk /= 10;
            }
        } else {
            /* Highest chunk: no leading zeros needed */
            if (chunk == 0 && dpos == 0) {
                digits[dpos++] = '0';
            } else {
                while (chunk > 0) {
                    digits[dpos++] = '0' + (chunk % 10);
                    chunk /= 10;
                }
            }
        }
    }
    free(limbs);

    /* Construct output with optional '-' sign and reversed digits */
    size_t out_len = dpos + (b->sign < 0 ? 1 : 0);
    char *out = (char *)malloc(out_len + 1);
    if (!out) { free(digits); return NULL; }

    size_t opos = 0;
    if (b->sign < 0) out[opos++] = '-';
    for (int i = (int)dpos - 1; i >= 0; i--) {
        out[opos++] = digits[i];
    }
    out[opos] = '\0';
    free(digits);
    return out;
}

/* ========================================================================= */
/* Bitwise Logic Helpers                                                     */
/* ========================================================================= */

Value bigint_bitwise_and(MiniVM *vm, Value a, Value b) {
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        return mini_make_fixnum(mini_to_fixnum(a) & mini_to_fixnum(b));
    }
    int64_t na, nb;
    if (bigint_to_int64(a, &na) && bigint_to_int64(b, &nb)) {
        return bigint_from_int64(vm, na & nb);
    }
    vm_error(vm, "bitwise-and: arguments too large for 64-bit mask");
    return mini_void();
}

Value bigint_bitwise_ior(MiniVM *vm, Value a, Value b) {
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        return mini_make_fixnum(mini_to_fixnum(a) | mini_to_fixnum(b));
    }
    int64_t na, nb;
    if (bigint_to_int64(a, &na) && bigint_to_int64(b, &nb)) {
        return bigint_from_int64(vm, na | nb);
    }
    vm_error(vm, "bitwise-ior: arguments too large for 64-bit mask");
    return mini_void();
}

Value bigint_bitwise_xor(MiniVM *vm, Value a, Value b) {
    if (mini_is_fixnum(a) && mini_is_fixnum(b)) {
        return mini_make_fixnum(mini_to_fixnum(a) ^ mini_to_fixnum(b));
    }
    int64_t na, nb;
    if (bigint_to_int64(a, &na) && bigint_to_int64(b, &nb)) {
        return bigint_from_int64(vm, na ^ nb);
    }
    vm_error(vm, "bitwise-xor: arguments too large for 64-bit mask");
    return mini_void();
}

Value bigint_bitwise_not(MiniVM *vm, Value a) {
    if (mini_is_fixnum(a)) {
        return mini_make_fixnum(~mini_to_fixnum(a));
    }
    int64_t na;
    if (bigint_to_int64(a, &na)) {
        return bigint_from_int64(vm, ~na);
    }
    vm_error(vm, "bitwise-not: argument too large for 64-bit mask");
    return mini_void();
}

Value bigint_shift(MiniVM *vm, Value a, int64_t shift) {
    if (shift == 0) return a;
    /* 2^|shift|, built while a stays rooted (each step allocates) */
    int64_t n = shift > 0 ? shift : -shift;
    Value factor = mini_make_fixnum(1), step = mini_void();
    mini_push_root(vm, &a);
    mini_push_root(vm, &factor);
    mini_push_root(vm, &step);
    while (n >= 60) {
        step = bigint_from_int64(vm, (int64_t)1 << 60);
        factor = bigint_mul(vm, factor, step);
        n -= 60;
    }
    if (n > 0) factor = bigint_mul(vm, factor, mini_make_fixnum((int64_t)1 << n));
    Value res = a;
    if (shift > 0) {
        res = bigint_mul(vm, a, factor);           /* a * 2^shift */
    } else {
        Value rem;
        bigint_div_rem(vm, a, factor, &res, &rem); /* a / 2^-shift */
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return res;
}

/* ========================================================================= */
/* Public API Export Implementations                                         */
/* ========================================================================= */

Value mini_make_bigint_from_int64(MiniVM *vm, int64_t n) {
    return bigint_from_int64(vm, n);
}

Value mini_make_bigint_from_str(MiniVM *vm, const char *str, int radix) {
    return bigint_from_str(vm, str, radix);
}

char *mini_bigint_to_str(Value v, int radix) {
    return bigint_to_str(v, radix);
}

bool mini_bigint_to_int64(Value v, int64_t *out) {
    return bigint_to_int64(v, out);
}
