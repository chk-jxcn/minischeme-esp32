/**
 * @file matrix.c
 * @brief The `matrix` API extension: the 8 x 8 RGB LED matrix of the Waveshare
 *        ESP32-S3-Matrix as Scheme procedures (driver: minischeme_board_matrix.c).
 *
 * Pixels are (x, y) with (0, 0) at the top left, colours 0xRRGGBB. matrix-set and matrix-fill
 * change a buffer; matrix-show sends it. matrix-draw, matrix-clear and matrix-brightness send
 * at once.
 */

#include "matrix.h"

#include "minischeme_internal.h"
#include "minischeme_matrix.h"

static bool arg_int(Value v, long long lo, long long hi, long long *out) {
    if (!mini_is_fixnum(v)) return false;
    long long n = (long long)mini_to_fixnum(v);
    if (n < lo || n > hi) return false;
    *out = n;
    return true;
}

static Value prim_size(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_cons(vm, mini_make_fixnum(MINI_MATRIX_W), mini_make_fixnum(MINI_MATRIX_H));
}

static Value prim_set(MiniVM *vm, int argc, Value *argv) {
    long long x, y, c;
    if (argc != 3 || !arg_int(argv[0], 0, MINI_MATRIX_W - 1, &x) || !arg_int(argv[1], 0, MINI_MATRIX_H - 1, &y) ||
        !arg_int(argv[2], 0, 0xffffff, &c)) {
        vm_error(vm, "matrix-set: expected x (0..%d), y (0..%d) and a colour (0..#xffffff)", MINI_MATRIX_W - 1,
                 MINI_MATRIX_H - 1);
        return mini_void();
    }
    mini_matrix_set((int)x, (int)y, (uint32_t)c);
    return mini_true();
}

static Value prim_get(MiniVM *vm, int argc, Value *argv) {
    long long x, y;
    if (argc != 2 || !arg_int(argv[0], 0, MINI_MATRIX_W - 1, &x) || !arg_int(argv[1], 0, MINI_MATRIX_H - 1, &y)) {
        vm_error(vm, "matrix-get: expected x (0..%d) and y (0..%d)", MINI_MATRIX_W - 1, MINI_MATRIX_H - 1);
        return mini_void();
    }
    return mini_make_fixnum((int64_t)mini_matrix_get((int)x, (int)y));
}

static Value prim_fill(MiniVM *vm, int argc, Value *argv) {
    long long c;
    if (argc != 1 || !arg_int(argv[0], 0, 0xffffff, &c)) {
        vm_error(vm, "matrix-fill: expected a colour (0..#xffffff)");
        return mini_void();
    }
    mini_matrix_fill((uint32_t)c);
    return mini_true();
}

static Value prim_show(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_bool(mini_matrix_show());
}

static Value prim_clear(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    mini_matrix_fill(0);
    return mini_make_bool(mini_matrix_show());
}

/* (matrix-draw colours): a vector or a list of 64 colours, row by row from the top left */
static Value prim_draw(MiniVM *vm, int argc, Value *argv) {
    const int n = MINI_MATRIX_W * MINI_MATRIX_H;
    uint32_t px[MINI_MATRIX_W * MINI_MATRIX_H];
    int count = 0;
    bool ok = argc == 1;
    if (ok && mini_is_vector(argv[0])) {
        size_t len = mini_vector_length(argv[0]);
        ok = len == (size_t)n;
        for (int i = 0; ok && i < n; i++) {
            long long c;
            ok = arg_int(mini_vector_ref(vm, argv[0], (size_t)i), 0, 0xffffff, &c);
            if (ok) px[count++] = (uint32_t)c;
        }
    } else if (ok) {
        Value l = argv[0];
        while (ok && mini_is_pair(l) && count < n) {
            long long c;
            ok = arg_int(mini_car(l), 0, 0xffffff, &c);
            if (ok) px[count++] = (uint32_t)c;
            l = mini_cdr(l);
        }
        ok = ok && count == n && l == mini_nil();
    }
    if (!ok) {
        vm_error(vm, "matrix-draw: expected a vector or a list of %d colours (0..#xffffff)", n);
        return mini_void();
    }
    for (int i = 0; i < n; i++) mini_matrix_set(i % MINI_MATRIX_W, i / MINI_MATRIX_W, px[i]);
    return mini_make_bool(mini_matrix_show());
}

/* (matrix-brightness [percent]) */
static Value prim_brightness(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1) {
        long long p;
        if (!arg_int(argv[0], 0, 100, &p)) {
            vm_error(vm, "matrix-brightness: expected a percentage (0..100)");
            return mini_void();
        }
        mini_matrix_set_brightness((int)p);
        mini_matrix_show();
    }
    return mini_make_fixnum(mini_matrix_get_brightness());
}

void matrix_register(MiniVM *vm) {
    static const MiniPrimEntry PRIMS[] = {
        { "matrix-size", prim_size }, { "matrix-set", prim_set }, { "matrix-get", prim_get },
        { "matrix-fill", prim_fill }, { "matrix-show", prim_show }, { "matrix-clear", prim_clear },
        { "matrix-draw", prim_draw }, { "matrix-brightness", prim_brightness },
    };
    mini_register_prim_table(vm, PRIMS, sizeof(PRIMS) / sizeof(PRIMS[0]));
}
