/**
 * @file setup_portal.h
 * @brief Setup mode of the system settings: an access point with a random
 *        SSID and 8-digit numeric password next to the station (APSTA) and
 *        a DNS server that answers every name with the access point's address,
 *        so phones open the settings page (served by the HTTP API at /) by themselves.
 */
#ifndef SETUP_PORTAL_H
#define SETUP_PORTAL_H

#include <stdbool.h>

/** @brief Starts the access point and the captive DNS once; false on failure. */
bool setup_portal_start(void);
/** @brief Switches to the next SSID and password preset. */
bool setup_portal_switch(void);
/** @brief The access point runs. */
bool setup_portal_active(void);
/** @brief Its name ("" before the start). */
const char *setup_portal_ssid(void);
/** @brief Its password (8 random digits). */
const char *setup_portal_pass(void);
/** @brief Its address ("192.168.4.1"). */
const char *setup_portal_ip(void);
/** @brief Stations joined to it. */
int setup_portal_clients(void);

#endif /* SETUP_PORTAL_H */
