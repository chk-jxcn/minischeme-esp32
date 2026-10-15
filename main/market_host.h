/**
 * @file market_host.h
 * @brief The board's host for the app market engine (minischeme_market.h).
 */

#ifndef MARKET_HOST_H
#define MARKET_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Hands the engine its HTTP, file and config hooks; call once at boot. */
void market_host_init(void);

/** @brief Once the boot mode is known: installs run only in the system
 *  settings; auto_update restarts into them for an automatic update;
 *  builtin gives the copy of a system app built into the firmware. */
void market_host_start(bool settings_mode, void (*auto_update)(void),
                       const uint8_t *(*builtin)(const char *id, size_t *len));

#endif /* MARKET_HOST_H */
