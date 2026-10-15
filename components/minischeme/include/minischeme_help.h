/**
 * @file minischeme_help.h
 * @brief Interactive Help System & Autocompletion Engine for MiniScheme.
 *        Can be completely disabled via -DMINISCHEME_NO_HELP for release/embedded builds.
 */

#ifndef MINISCHEME_HELP_H
#define MINISCHEME_HELP_H

#include "minischeme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register help & autocompletion primitives into the VM global environment.
 * Under -DMINISCHEME_NO_HELP, registers zero-overhead lightweight stubs.
 */
void mini_register_help_lib(MiniVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_HELP_H */
