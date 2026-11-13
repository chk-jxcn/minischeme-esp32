/**
 * @file ble.h
 * @brief Bluetooth Low Energy for apps (research branch feature/bluetooth): the NimBLE host,
 *        BLE only, as the Scheme procedures of the `ble` API extension (ble.c).
 *
 * Off unless config.ini [bluetooth] bt_enabled = 1: the controller's reserved memory then
 * goes back to the heap at boot (ble_release_memory). On, the stack starts before the VM
 * heap is sized, so its RAM comes out of the VM's share, not out of the system's reserve.
 */

#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include <stddef.h>
#include "minischeme.h"

/** @brief Not used this boot: the controller's memory goes to the heap. */
void ble_release_memory(void);

/** @brief Starts the controller and the host (name: the advertised device name);
 *  false when it cannot (the memory is then released as far as possible). */
bool ble_start(const char *name);

/** @brief The stack runs. */
bool ble_running(void);

/** @brief Heap the stack took when it started (bytes), 0 when off. */
size_t ble_heap_used(void);

/** @brief The `ble-...` procedures into a VM (they answer #f / raise while the stack is off). */
void ble_register(MiniVM *vm);

/** @brief The main loop: hands queued events to the app's (app-ble-event ev) hook when it
 *  defines one (else they wait for (ble-event)). */
void ble_poll(MiniVM *vm);

/** @brief {"enabled":..,"state":..,"address":..,...} into out; returns the length. */
size_t ble_status_json(char *out, size_t cap);

#endif /* BLE_H */
