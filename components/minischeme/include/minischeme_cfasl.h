/**
 * @file minischeme_cfasl.h
 * @brief Compressed Chunked FASL (CFASL) & Demand-Paged Bytecode Engine.
 */

#ifndef MINISCHEME_CFASL_H
#define MINISCHEME_CFASL_H

#include "minischeme.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFASL_MAGIC 0x4C534643 /* 'CFSL' */

/* Fast zero-dependency LZSS/RLE Chunk Compression */
size_t cfasl_compress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);
size_t cfasl_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

/* Create a compressed procedure stub in Scheme */
Value cfasl_make_procedure_stub(MiniVM *vm,
                                const uint8_t *comp_bytes,
                                size_t comp_size,
                                size_t uncomp_code_bytes,
                                int num_params,
                                int max_stack,
                                int is_variadic,
                                int has_inner_lambdas,
                                Value constants_vec);

/* Execute a demand-paged compressed procedure chunk */
Value cfasl_call_chunk(MiniVM *vm, Value stub, int argc, Value *argv);

/* Register CFASL Scheme Primitives */
void mini_register_cfasl_lib(MiniVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_CFASL_H */
