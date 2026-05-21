/**
 * beacon_list.c
 * Doubly-linked list storing BLE beacon node information.
 */

#include "beacon_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal: allocate and populate one node
 * ---------------------------------------------------------------------- */
static BeaconNode *make_node(const char *name,  const char *mac,
                              uint16_t    major, uint16_t   minor,
                              double      x,     double     y,
                              int8_t      rssi_ref,
                              const char *left_name,
                              const char *right_name)
{
    BeaconNode *n = (BeaconNode *)malloc(sizeof(BeaconNode));
    if (!n) return NULL;

    strncpy(n->name,       name,       BEACON_NAME_LEN - 1);
    strncpy(n->mac,        mac,        BEACON_MAC_LEN  - 1);
    strncpy(n->left_name,  left_name,  BEACON_NAME_LEN - 1);
    strncpy(n->right_name, right_name, BEACON_NAME_LEN - 1);
    n->name[BEACON_NAME_LEN - 1]       = '\0';
    n->mac[BEACON_MAC_LEN  - 1]        = '\0';
    n->left_name[BEACON_NAME_LEN - 1]  = '\0';
    n->right_name[BEACON_NAME_LEN - 1] = '\0';
    
    n->major    = major;
    n->minor    = minor;
    n->x        = x;
    n->y        = y;
    n->rssi_ref = rssi_ref;

    n->last_rssi_measure = 0;

    // Linked list shit
    n->prev     = NULL;
    n->next     = NULL;
    return n;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void beacon_list_init(BeaconList *list)
{
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}

BeaconNode *beacon_list_push_back(BeaconList *list,
                                   const char *name,  const char *mac,
                                   uint16_t    major, uint16_t   minor,
                                   double      x,     double     y,
                                   int8_t      rssi_ref,
                                   const char *left_name,
                                   const char *right_name)
{
    BeaconNode *n = make_node(name, mac, major, minor, x, y,
                               rssi_ref, left_name, right_name);
    if (!n) return NULL;

    n->prev = list->tail;
    n->next = NULL;

    if (list->tail)
        list->tail->next = n;
    else
        list->head = n;         /* list was empty */

    list->tail = n;
    ++list->size;
    return n;
}

BeaconNode *beacon_list_push_front(BeaconList *list,
                                    const char *name,  const char *mac,
                                    uint16_t    major, uint16_t   minor,
                                    double      x,     double     y,
                                    int8_t      rssi_ref,
                                    const char *left_name,
                                    const char *right_name)
{
    BeaconNode *n = make_node(name, mac, major, minor, x, y,
                               rssi_ref, left_name, right_name);
    if (!n) return NULL;

    n->prev = NULL;
    n->next = list->head;

    if (list->head)
        list->head->prev = n;
    else
        list->tail = n;         /* list was empty */

    list->head = n;
    ++list->size;
    return n;
}

void beacon_list_remove(BeaconList *list, BeaconNode *node)
{
    if (!node) return;

    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;    /* node was head */

    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;    /* node was tail */

    free(node);
    --list->size;
}

BeaconNode *beacon_list_find_mac(const BeaconList *list, const char *mac)
{
    for (BeaconNode *n = list->head; n; n = n->next)
        if (strcmp(n->mac, mac) == 0)
            return n;
    return NULL;
}

BeaconNode *beacon_list_find_name(const BeaconList *list, const char *name)
{
    for (BeaconNode *n = list->head; n; n = n->next)
        if (strcmp(n->name, name) == 0)
            return n;
    return NULL;
}

void beacon_list_print(const BeaconList *list)
{
    printf("BeaconList (%zu nodes):\n", list->size);
    printf("  %-10s %-19s %6s %6s %6s %6s %8s  %-12s %-12s\n",
           "Name", "MAC", "Major", "Minor", "X", "Y", "RSSIref",
           "Left", "Right");
    printf("  %s\n", "--------------------------------------------------------------"
                     "-------------------------------");
    for (BeaconNode *n = list->head; n; n = n->next) {
        printf("  %-10s %-19s %6u %6u %6.1f %6.1f %+8d  %-12s %-12s\n",
               n->name, n->mac, n->major, n->minor,
               n->x, n->y, (int)n->rssi_ref,
               n->left_name[0]  ? n->left_name  : "(none)",
               n->right_name[0] ? n->right_name : "(none)");
    }
}

void beacon_list_add_new_rssi(BeaconNode *node, int8_t new_rssi)
{
    node->rssi_measures[node->last_rssi_measure] = new_rssi;
    node->last_rssi_measure = (node->last_rssi_measure + 1) % RSSI_WINDOW_SIZE;
}

void beacon_list_destroy(BeaconList *list)
{
    BeaconNode *cur = list->head;
    while (cur) {
        BeaconNode *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    beacon_list_init(list);
}
