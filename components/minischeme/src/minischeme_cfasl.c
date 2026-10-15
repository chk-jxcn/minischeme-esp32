/**
 * @file minischeme_cfasl.c
 * @brief Compressed Chunked FASL (CFASL) & Demand-Paged Bytecode Engine.
 */

#include "minischeme_cfasl.h"
#include "minischeme_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================= */
/* 1. Fast Zero-Dependency LZSS / RLE Compression Engine                     */
/* ========================================================================= */

#define LZ_WINDOW_SIZE 4095   /* a distance is 12 bits: 1..4095 (0 is invalid) */
#define LZ_MAX_MATCH   18
#define LZ_MIN_MATCH   3

size_t cfasl_compress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    if (!src || !dst || src_len == 0 || dst_cap == 0) return 0;
    
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < src_len) {
        if (out_pos + 3 > dst_cap) return 0;   /* the largest token: a match, 3 bytes */

        /* Search for matching sequence in preceding window */
        size_t win_start = (in_pos > LZ_WINDOW_SIZE) ? (in_pos - LZ_WINDOW_SIZE) : 0;
        size_t best_len = 0;
        size_t best_dist = 0;

        size_t max_lookahead = src_len - in_pos;
        if (max_lookahead > LZ_MAX_MATCH) max_lookahead = LZ_MAX_MATCH;

        if (max_lookahead >= LZ_MIN_MATCH) {
            for (size_t s = win_start; s < in_pos; s++) {
                size_t match_len = 0;
                while (match_len < max_lookahead && src[s + match_len] == src[in_pos + match_len]) {
                    match_len++;
                }
                if (match_len > best_len) {
                    best_len = match_len;
                    best_dist = in_pos - s;
                    if (best_len == max_lookahead) break;
                }
            }
        }

        if (best_len >= LZ_MIN_MATCH) {
            /* Emit match token: 0x80 | (length - 3), followed by 2 bytes distance */
            dst[out_pos++] = (uint8_t)(0x80 | ((best_len - LZ_MIN_MATCH) & 0x0F));
            dst[out_pos++] = (uint8_t)(best_dist & 0xFF);
            dst[out_pos++] = (uint8_t)((best_dist >> 8) & 0x0F);
            in_pos += best_len;
        } else {
            /* Emit literal byte (if 0x80..0xFF, escape with 0xFF) */
            uint8_t lit = src[in_pos++];
            if (lit >= 0x80) {
                dst[out_pos++] = 0xFF;
                dst[out_pos++] = lit;
            } else {
                dst[out_pos++] = lit;
            }
        }
    }
    return out_pos;
}

size_t cfasl_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    if (!src || !dst || src_len == 0 || dst_cap == 0) return 0;

    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < src_len && out_pos < dst_cap) {
        uint8_t token = src[in_pos++];
        if (token == 0xFF) {
            /* Escaped literal >= 0x80 */
            if (in_pos >= src_len) return 0;
            dst[out_pos++] = src[in_pos++];
        } else if ((token & 0x80) != 0) {
            /* Match sequence */
            size_t match_len = (token & 0x0F) + LZ_MIN_MATCH;
            if (in_pos + 1 >= src_len) return 0;
            size_t dist_low = src[in_pos++];
            size_t dist_high = src[in_pos++];
            size_t dist = dist_low | ((dist_high & 0x0F) << 8);
            if (dist == 0 || dist > out_pos) return 0; /* Corrupt dist */

            size_t ref_pos = out_pos - dist;
            for (size_t m = 0; m < match_len; m++) {
                if (out_pos >= dst_cap) return 0;
                dst[out_pos++] = dst[ref_pos + m];
            }
        } else {
            /* Literal < 0x80 */
            dst[out_pos++] = token;
        }
    }
    return out_pos;
}

/* ========================================================================= */
/* 2. Demand-Paged Chunk Execution Engine                                    */
/* ========================================================================= */

/* Upper bound for one decompressed procedure. The scratch buffer is
 * malloc'd per call (mini_make_code copies the bytes into the VM heap), so
 * no static RAM is held when CFASL is unused. */
#define CFASL_SCRATCH_SIZE 32768

Value cfasl_make_procedure_stub(MiniVM *vm,
                                const uint8_t *comp_bytes,
                                size_t comp_size,
                                size_t uncomp_code_bytes,
                                int num_params,
                                int max_stack,
                                int is_variadic,
                                int has_inner_lambdas,
                                Value constants_vec) {
    if (!vm || !comp_bytes || comp_size == 0) return mini_nil();

    /* Store compressed bytes in a Scheme String */
    Value comp_str = mini_make_string_len(vm, (const char *)comp_bytes, comp_size);

    /* Allocate vector metadata stub:
     * #( 'cfasl-stub comp_str uncomp_bytes num_params max_stack is_variadic has_inner const_vec )
     */
    Value meta = mini_make_vector(vm, 8, mini_nil());
    mini_vector_set(vm, meta, 0, mini_make_symbol(vm, "cfasl-stub"));
    mini_vector_set(vm, meta, 1, comp_str);
    mini_vector_set(vm, meta, 2, mini_make_fixnum((int64_t)uncomp_code_bytes));
    mini_vector_set(vm, meta, 3, mini_make_fixnum((int64_t)num_params));
    mini_vector_set(vm, meta, 4, mini_make_fixnum((int64_t)max_stack));
    mini_vector_set(vm, meta, 5, mini_make_bool(is_variadic != 0));
    mini_vector_set(vm, meta, 6, mini_make_bool(has_inner_lambdas != 0));
    mini_vector_set(vm, meta, 7, constants_vec);

    return meta;
}

Value cfasl_call_chunk(MiniVM *vm, Value stub, int argc, Value *argv) {
    if (!mini_is_vector(stub) || mini_vector_length(stub) < 8) {
        vm_error(vm, "cfasl_call_chunk: invalid chunk stub");
        return mini_void();
    }

    Value tag = mini_vector_ref(vm, stub, 0);
    if (!mini_is_symbol(tag) || strcmp(mini_symbol_name(tag), "cfasl-stub") != 0) {
        vm_error(vm, "cfasl_call_chunk: expected cfasl-stub object");
        return mini_void();
    }

    Value comp_str = mini_vector_ref(vm, stub, 1);
    if (!mini_is_string(comp_str)) {
        vm_error(vm, "cfasl_call_chunk: corrupt compressed data");
        return mini_void();
    }

    for (size_t i = 2; i <= 4; i++) {
        if (!mini_is_fixnum(mini_vector_ref(vm, stub, i)) || mini_to_fixnum(mini_vector_ref(vm, stub, i)) < 0) {
            vm_error(vm, "cfasl-call: corrupt stub (field %u)", (unsigned)i);
            return mini_void();
        }
    }
    size_t uncomp_bytes = (size_t)mini_to_fixnum(mini_vector_ref(vm, stub, 2));
    uint16_t num_params = (uint16_t)mini_to_fixnum(mini_vector_ref(vm, stub, 3));
    uint16_t max_stack  = (uint16_t)mini_to_fixnum(mini_vector_ref(vm, stub, 4));
    uint8_t is_variadic = mini_to_bool(mini_vector_ref(vm, stub, 5)) ? 1 : 0;
    uint8_t has_inner   = mini_to_bool(mini_vector_ref(vm, stub, 6)) ? 1 : 0;
    Value const_vec     = mini_vector_ref(vm, stub, 7);

    /* the procedure's arity: num_params, or num_params - 1 and more when variadic */
    int fixed = is_variadic ? (int)num_params - 1 : (int)num_params;
    if (argc < fixed || (!is_variadic && argc > fixed)) {
        vm_error(vm, "cfasl-call: expected %s%d argument%s, got %d", is_variadic ? "at least " : "", fixed,
                 fixed == 1 ? "" : "s", argc);
        return mini_void();
    }
    if (uncomp_bytes == 0) {
        vm_error(vm, "cfasl-call: empty procedure");
        return mini_void();
    }

    if (uncomp_bytes > CFASL_SCRATCH_SIZE) {
        vm_error(vm, "cfasl_call_chunk: procedure chunk exceeds scratch buffer limit (%zu > %d)",
                 uncomp_bytes, CFASL_SCRATCH_SIZE);
        return mini_void();
    }

    /* Decompress ONLY this procedure's instructions into the scratch buffer */
    const uint8_t *comp_data = (const uint8_t *)mini_string_data(comp_str);
    size_t comp_len = mini_string_len(comp_str);

    uint8_t *scratch = (uint8_t *)malloc(uncomp_bytes ? uncomp_bytes : 1);
    if (!scratch) {
        vm_error(vm, "cfasl_call_chunk: out of memory for %zu-byte chunk", uncomp_bytes);
        return mini_void();
    }
    size_t decomp_len = cfasl_decompress(comp_data, comp_len, scratch, uncomp_bytes);
    if (decomp_len != uncomp_bytes) {
        free(scratch);
        vm_error(vm, "cfasl_call_chunk: decompression failure (%zu != %zu)", decomp_len, uncomp_bytes);
        return mini_void();
    }

    /* Extract constant pool array */
    size_t num_consts = 0;
    Value *consts_buf = NULL;
    if (mini_is_vector(const_vec)) {
        num_consts = mini_vector_length(const_vec);
        if (num_consts > 0) {
            VectorObj *vobj = (VectorObj *)UNTAG_PTR(const_vec);
            consts_buf = vobj->data;
        }
    }

    /* Construct transient CodeObj and Closure for demand-paged execution */
    Value code_val = mini_make_code(vm, (uint32_t)uncomp_bytes, scratch,
                                    num_params, max_stack, is_variadic, has_inner,
                                    (uint16_t)num_consts, consts_buf);
    free(scratch);
    if (mini_is_void(code_val)) return mini_void();
    Value clo = mini_make_closure(vm, code_val, mini_nil());

    /* Execute the transient procedure */
    return mini_vm_run(vm, clo, argc, argv);
}

/* ========================================================================= */
/* 3. Scheme Primitives for CFASL                                            */
/* ========================================================================= */

static Value prim_cfasl_compress(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1 && mini_is_string(argv[0])) {
        const uint8_t *src = (const uint8_t *)mini_string_data(argv[0]);
        size_t src_len = mini_string_len(argv[0]);
        size_t max_out = src_len * 2 + 16;   /* worst case: every byte an escaped literal */
        uint8_t *tmp = (uint8_t *)malloc(max_out);
        if (!tmp) return mini_nil();
        size_t out_len = cfasl_compress(src, src_len, tmp, max_out);
        if (out_len == 0) {
            free(tmp);
            return mini_nil();
        }
        Value res = mini_make_string_len(vm, (const char *)tmp, out_len);
        free(tmp);
        return res;
    }
    return mini_nil();
}

static Value prim_cfasl_decompress(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 2 && mini_is_string(argv[0]) && mini_is_fixnum(argv[1])) {
        const uint8_t *src = (const uint8_t *)mini_string_data(argv[0]);
        size_t src_len = mini_string_len(argv[0]);
        size_t target_size = (size_t)mini_to_fixnum(argv[1]);

        uint8_t *tmp = (uint8_t *)malloc(target_size + 1);
        if (!tmp) return mini_nil();
        size_t out_len = cfasl_decompress(src, src_len, tmp, target_size);
        if (out_len != target_size) {
            free(tmp);
            return mini_nil();
        }
        Value res = mini_make_string_len(vm, (const char *)tmp, out_len);
        free(tmp);
        return res;
    }
    return mini_nil();
}

static Value prim_cfasl_make_stub(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 7 && mini_is_string(argv[0]) && mini_is_fixnum(argv[1])) {
        const uint8_t *comp_data = (const uint8_t *)mini_string_data(argv[0]);
        size_t comp_size = mini_string_len(argv[0]);
        size_t uncomp_bytes = (size_t)mini_to_fixnum(argv[1]);
        int num_params = (int)mini_to_fixnum(argv[2]);
        int max_stack = (int)mini_to_fixnum(argv[3]);
        int is_variadic = mini_to_bool(argv[4]) ? 1 : 0;
        int has_inner = mini_to_bool(argv[5]) ? 1 : 0;
        Value consts_vec = argv[6];

        return cfasl_make_procedure_stub(vm, comp_data, comp_size, uncomp_bytes,
                                         num_params, max_stack, is_variadic, has_inner,
                                         consts_vec);
    }
    return mini_nil();
}

static Value prim_cfasl_call(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1) {
        Value stub = argv[0];
        int sub_argc = argc - 1;
        Value *sub_argv = (sub_argc > 0) ? &argv[1] : NULL;
        return cfasl_call_chunk(vm, stub, sub_argc, sub_argv);
    }
    return mini_void();
}

static Value prim_cfasl_stub_p(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1 && mini_is_vector(argv[0]) && mini_vector_length(argv[0]) >= 8) {
        Value tag = mini_vector_ref(vm, argv[0], 0);
        if (mini_is_symbol(tag) && strcmp(mini_symbol_name(tag), "cfasl-stub") == 0) {
            return mini_true();
        }
    }
    return mini_false();
}

void mini_register_cfasl_lib(MiniVM *vm) {
    if (!vm) return;
    static const MiniPrimEntry funcs[] = {
        { "cfasl-compress",   prim_cfasl_compress },
        { "cfasl-decompress", prim_cfasl_decompress },
        { "cfasl-make-stub",  prim_cfasl_make_stub },
        { "cfasl-call",       prim_cfasl_call },
        { "cfasl-stub?",      prim_cfasl_stub_p },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}
