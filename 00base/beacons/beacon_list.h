/**
 * beacon_list.h
 * Doubly-linked list storing BLE beacon node information.
 *
 * All nodes share the same hardware config:
 *   TX Power:             0 dBm
 *   Advertising interval: 100 ms
 *
 * rssi_ref is the measured RSSI (dBm) at exactly 1 metre from the beacon.
 * It is used by path-loss models (e.g. log-distance) to convert a live
 * RSSI reading into an estimated range.
 */

#ifndef BEACON_LIST_H
#define BEACON_LIST_H

#include <stdint.h>
#include <stddef.h>

#define BEACON_NAME_LEN  32     /* max chars incl. NUL               */
#define BEACON_MAC_LEN   27     /* "AA:BB:CC:DD:EE:FF\0" = 18 chars  */
#define RSSI_WINDOW_SIZE 30     // 40 was pretty good

typedef struct BeaconNode {
    /* Beacon identity */
    char     name[BEACON_NAME_LEN];
    char     mac[BEACON_MAC_LEN];
    uint16_t major;
    uint16_t minor;

    /* Physical location (metres, set from site survey) */
    double   x;
    double   y;

    /* RF parameter */
    int8_t   rssi_ref;          /* reference RSSI at 1 m (dBm)       */

    /* Neighbour names */
    char     left_name[BEACON_NAME_LEN];
    char     right_name[BEACON_NAME_LEN];
    
    /* RSSI Measurements */
    int8_t   rssi_measures[RSSI_WINDOW_SIZE];
    int8_t   last_rssi_measure;

    /* List pointers */
    struct BeaconNode *prev;
    struct BeaconNode *next;
} BeaconNode;

typedef struct {
    BeaconNode *head;
    BeaconNode *tail;
    size_t      size;
} BeaconList;

/** Initialise an empty list. */
void beacon_list_init(BeaconList *list);

/**
 * Append a new beacon to the tail of the list.
 * Returns the new node, or NULL on allocation failure.
 */
BeaconNode *beacon_list_push_back(BeaconList *list,
                                   const char *name,  const char *mac,
                                   uint16_t    major, uint16_t   minor,
                                   double      x,     double     y,
                                   int8_t      rssi_ref,
                                   const char *left_name,
                                   const char *right_name);

/**
 * Prepend a new beacon to the head of the list.
 * Returns the new node, or NULL on allocation failure.
 */
BeaconNode *beacon_list_push_front(BeaconList *list,
                                    const char *name,  const char *mac,
                                    uint16_t    major, uint16_t   minor,
                                    double      x,     double     y,
                                    int8_t      rssi_ref,
                                    const char *left_name,
                                    const char *right_name);

/** Unlink and free a node. Do not use the pointer after this call. */
void        beacon_list_remove(BeaconList *list, BeaconNode *node);

/** O(n) search by MAC address string. Returns NULL if not found. */
BeaconNode *beacon_list_find_mac(const BeaconList *list, const char *mac);

/** O(n) search by beacon name. Returns NULL if not found. */
BeaconNode *beacon_list_find_name(const BeaconList *list, const char *name);

/** Print every node to stdout. */
void        beacon_list_print(const BeaconList *list);

/** Free all nodes and reset the list to empty. */
void        beacon_list_destroy(BeaconList *list);

void beacon_list_add_new_rssi(BeaconNode *node, int8_t new_rssi);

#endif /* BEACON_LIST_H */
