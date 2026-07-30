/*
 * Compile the shared GATT implementation from a source path that stays inside
 * each target's intermediate directory.
 *
 * Referencing ../ble_hci_test/mybtgatt-server.c directly from Build.mk makes
 * the build system place its object outside my-fnirs[-uv]-intermediate.  The
 * libevent and libuv targets then accidentally share one object despite using
 * different PIE flags. Keep this wrapper target-local as the shared GATT
 * implementation evolves (including concurrent growing-file synchronization,
 * cumulative ACK handling, tail retries, compact recording payloads, and the
 * final DONE acknowledgement).
 */
#include "../../../ble_hci_test/mybtgatt-server.c"
