/**
 * @file minischeme_api.h
 * @brief The device API's version, its table and the checks built on it.
 *
 * The table (minischeme_api_table.h) is generated from api/spec by
 * tools/gen_api.py; docs/api/ holds the reference. The firmware provides one
 * API version (major.minor.patch); an app image declares the version it needs
 * in its metadata ([app] api = 1.0) and runs when the major versions are
 * equal and its minor version is not higher. Hardware-specific procedures
 * form extension APIs ("esp32"), versioned on their own and present only on
 * the devices that set them with mini_api_set_extensions().
 */

#ifndef MINISCHEME_API_H
#define MINISCHEME_API_H

#include "minischeme.h"
#include <stdbool.h>
#include <stddef.h>

/** @brief The API version this build provides, "major.minor.patch". */
const char *mini_api_version(void);

/** @brief The extensions present on this device: bit i for extension i of the
 *  table (1u << 0 is "esp32"). Set by the host before apps run; default none. */
void mini_api_set_extensions(unsigned mask);
unsigned mini_api_get_extensions(void);
/** @brief The bit of an extension for mini_api_set_extensions ("esp32"), 0 if unknown. */
unsigned mini_api_extension_bit(const char *id);

/** @brief Whether a requirement "M[.m[.p]]" is met by `provided` (same
 *  major, minor not higher; the patch level never matters). False when the
 *  text is not a version. */
bool mini_api_version_ok(const char *required, const char *provided);

/** @brief Checks an app's metadata ([app] api = 1.0, extensions = esp32 1.0)
 *  against this firmware. Returns true when it may run (no api key: any 1.x
 *  firmware); else false with the reason in err ("app needs API 2.0, the
 *  firmware provides 1.0.0"). */
bool mini_api_check_meta(const char *meta, char *err, size_t cap);

/** @brief Registers the api-* procedures and the VM hooks: older names
 *  resolve to their current procedure, and an unbound reserved or extension
 *  name says so in the error. Idempotent. */
void mini_register_api_lib(MiniVM *vm);

#endif /* MINISCHEME_API_H */
