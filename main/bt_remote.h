/**
 * @file bt_remote.h
 * @brief A Bluetooth LE remote (page turner, media or selfie remote, any BLE
 *        HID keyboard) as a second key: a BLE HID host on NimBLE.
 *
 * Research branch feature/bluetooth-remote. Started only with config.ini
 * [bluetooth] bt_remote = 1. Without a paired remote it scans for a HID
 * device in pairing mode and pairs the strongest one; the bond and the
 * address stay in NVS, and the remote is reconnected whenever it advertises
 * again (a key press wakes most remotes). Classic Bluetooth (BR/EDR) remotes
 * are not supported: that needs the Bluedroid stack, too large for this board.
 */

#ifndef BT_REMOTE_H
#define BT_REMOTE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BT_KEY_NONE = 0,
    BT_KEY_NEXT,     /* right, down, page down, space, volume down, next track */
    BT_KEY_PREV,     /* left, up, page up, volume up, previous track */
    BT_KEY_OK,       /* enter, play / pause, menu */
    BT_KEY_BACK      /* escape, AC back */
} BtRemoteKey;

/** @brief Not used this boot: the controller's reserved DRAM goes to the heap. */
void bt_remote_release_memory(void);

/** @brief Starts the stack and the connection task; false when it cannot. */
bool bt_remote_start(void);

/** @brief The next key pressed on the remote (BT_KEY_NONE: none); main loop only. */
BtRemoteKey bt_remote_poll(void);

/** @brief Forgets the paired remote (bond and address) and scans again. */
void bt_remote_forget(void);

/** @brief The name of a key ("next", "prev", "ok", "back"). */
const char *bt_remote_key_name(BtRemoteKey key);

/** @brief {"enabled":..,"state":..,"name":..,"address":..,"battery":..,
 *         "last":..,"seen":[..]} into out; returns the length. */
size_t bt_remote_status_json(char *out, size_t cap);

#endif /* BT_REMOTE_H */
