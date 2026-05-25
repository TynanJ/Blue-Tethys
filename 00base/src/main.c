
/* main.c - BLE Central Node (NUS Client - vanilla Zephyr)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/smf.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/data/json.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/drivers/uart.h>

#include "../beacons/beacon_list.h"
#include "../filters/direct_ekf.h"

LOG_MODULE_REGISTER(nus_central, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define BT_STACK_SIZE   1024
#define BT_PRIORITY     5

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_console)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

#define RECEIVE_BUFF_SIZE 512
static char rx_buf[RECEIVE_BUFF_SIZE];
static int rx_buf_pos = 0;

#define UART_STACK_SIZE   2048
#define KALMAN_STACK_SIZE   1024
#define UART_PRIORITY     5
#define KALMAN_PRIORITY    4

/* Message queue to pass complete lines from ISR to UART thread */
#define MSG_QUEUE_SIZE     8
#define MSG_MAX_LEN        RECEIVE_BUFF_SIZE

#define MODE_SNIFFER  0
#define MODE_BASE     1

static atomic_t current_mode = ATOMIC_INIT(MODE_BASE);

// GLOBAL VARIABLES
BeaconList anchor_beacons;

#define MAX_NODES 2

struct node_conn {
    struct bt_conn *conn;
    uint16_t nus_rx_handle;
    char name[32];
    struct bt_uuid_128 discover_uuid;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
    struct bt_uuid_128 rx_discover_uuid;
    struct bt_gatt_discover_params rx_discover_params;
    struct bt_gatt_write_params write_params;
    struct bt_gatt_exchange_params mtu_exchange_params;
};

static struct node_conn nodes[MAX_NODES];

static void discover_nus_rx(struct bt_conn *conn);

static struct node_conn *get_free_node(void);
static struct node_conn *get_node(struct bt_conn *conn);

static int nus_send_to_node(const char *name, const uint8_t *data, uint16_t len);

static struct node_conn *get_node(struct bt_conn *conn)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].conn == conn) return &nodes[i];
    }
    return NULL;
}

static struct node_conn *get_free_node(void)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].conn == NULL) return &nodes[i];
    }
    return NULL;
}


// === POSITIONDATA ===
// Inner data struct declarations
// Note that floating point numbers are not suppored by JSON library, so 
// Need to mulyiply our numbers by a constant multiple of 10 to get rid of 
// decimalness
// Update: Looks like only int32_t is supported as an integer. Scam. 
struct position_data_message {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t vx;
    int32_t vy;
};

// Outermost message struct
struct gui_position_message {
    const char *MessageType;
    int Timestamp;
    struct position_data_message Data;
};

// === BEACON ===
struct beacon_data_message {
    int32_t Operation;
    const char *BLEName;
    const char *BLEMAC;
    int32_t BLEMajor;
    int32_t BLEMinor;
    int32_t x;
    int32_t y;
    int32_t z;
    const char* LeftNeighbour;
    const char* RightNeighbour;
};

// Outermost message struct
struct gui_beacon_message {
    const char *MessageType;
    int Timestamp;
    struct beacon_data_message Data;
};


// === SNIFFER ===
struct sniffer_data_message {
    const char *BLEMAC;
    int32_t BLEMajor;
    int32_t BLEMinor;
    int32_t RSSI;
};

// Outermost message struct
struct gui_sniffer_message {
    const char *MessageType;
    int Timestamp;
    struct sniffer_data_message Data;
};

    
// ===== DESCRIPTORS =====
// === PositionData ===
static const struct json_obj_descr position_data_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct position_data_message, x, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct position_data_message, y, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct position_data_message, z, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct position_data_message, vx, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct position_data_message, vy, JSON_TOK_NUMBER),
};

static const struct json_obj_descr gui_position_message_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct gui_position_message, MessageType, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct gui_position_message, Timestamp, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_OBJECT(struct gui_position_message, Data, position_data_descr),
};

// === Beacon ===
static const struct json_obj_descr beacon_data_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, Operation, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, BLEName, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, BLEMAC, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, BLEMajor, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, BLEMinor, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, x, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, y, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, z, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, LeftNeighbour, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct beacon_data_message, RightNeighbour, JSON_TOK_STRING),
};

static const struct json_obj_descr gui_beacon_message_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct gui_beacon_message, MessageType, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct gui_beacon_message, Timestamp, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_OBJECT(struct gui_beacon_message, Data, beacon_data_descr),
};

// === Sniffer ===
static const struct json_obj_descr sniffer_data_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct sniffer_data_message, BLEMAC, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct sniffer_data_message, BLEMajor, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sniffer_data_message, BLEMinor, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct sniffer_data_message, RSSI, JSON_TOK_NUMBER),
};

static const struct json_obj_descr gui_sniffer_message_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct gui_sniffer_message, MessageType, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct gui_sniffer_message, Timestamp, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_OBJECT(struct gui_sniffer_message, Data, sniffer_data_descr),
};

/* ========================================================================== */
/* Localisation - EKF integration                                             */
/* ========================================================================== */

#include "../filters/direct_ekf.h"  /* adjust path to match your tree */
#include <math.h>

#define PATH_LOSS_EXP   1.8     /* indoor path-loss exponent n       */
#define EKF_DT          0.1     /* assumed seconds between updates   */
#define EKF_Q_VAR       0.5     /* process noise variance            */
#define EKF_R_VAR       4.0     /* measurement noise variance (m^2)  */
#define EKF_P0          10.0    /* initial covariance diagonal       */

/* Persistent EKF state */
static double ekf_state[4]      = {0.0, 0.0, 0.0, 0.0}; /* x,y,vx,vy */
static double ekf_cov[4 * 4]   = {0};                    /* 4×4, row-major */
static bool   ekf_initialised   = false;

/**
 * Convert a windowed-average RSSI reading to a range estimate (metres)
 * using the log-distance path-loss model.
 */
static double rssi_to_range(int8_t rssi_ref, int8_t rssi_measured)
{
    double delta = (double)(rssi_ref - rssi_measured);
    return pow(10.0, delta / (10.0 * PATH_LOSS_EXP));
}

/**
 * Compute the mean of the RSSI window stored in a BeaconNode.
 * Returns the last_rssi_measure if window is empty/uninitialised.
 */
static int8_t beacon_mean_rssi(const BeaconNode *node)
{
    int32_t sum = 0;
    for (int i = 0; i < RSSI_WINDOW_SIZE; i++) {
        sum += node->rssi_measures[i];
    }
    return (int8_t)(sum / RSSI_WINDOW_SIZE);
}

/**
 * Walk anchor_beacons, build the range measurement vector and matching
 * anchor coordinate array, then run one EKF step.
 *
 * Call this after every RSSI update in notify_func.
 */
/* Move these from local stack to static - safe because notify_func
 * is always called from the same BT RX thread, never re-entered    */
#define MAX_ANCHORS 16

static double ekf_z[MAX_ANCHORS];
static double ekf_anchors[MAX_ANCHORS * 2];
static double out_state[4];
static double out_cov[4 * 4];

K_MUTEX_DEFINE(beacon_mutex);

static void localisation_update(void)
{
    /* Count how many beacons have at least one real RSSI reading.
     * A beacon is considered "seen" when last_rssi_measure != 0.   */
    k_mutex_lock(&beacon_mutex, K_FOREVER);
    size_t n_anchors = 0;
    BeaconNode *node = anchor_beacons.head;
    while (node) {
        if (node->last_rssi_measure != 0)
            n_anchors++;
        node = node->next;
    }

    if (n_anchors > MAX_ANCHORS)
        n_anchors = MAX_ANCHORS;

    /* Build measurement vector and anchor coordinate array */
    size_t idx = 0;
    node = anchor_beacons.head;
    while (node && idx < MAX_ANCHORS) {
        if (node->last_rssi_measure != 0) {
            int8_t avg_rssi          = beacon_mean_rssi(node);
            ekf_z[idx]               = rssi_to_range(node->rssi_ref, avg_rssi);
            ekf_anchors[idx * 2]     = node->x;
            ekf_anchors[idx * 2 + 1] = node->y;
            idx++;
        }
        node = node->next;
    }

    DirectEKFResult result = {
        .states      = out_state,
        .covariances = out_cov,
    };

    /* On first call, seed x0 from the centroid of visible anchors */
    double x0[4] = {0.0, 0.0, 0.0, 0.0};
    if (!ekf_initialised) {
        for (size_t i = 0; i < n_anchors; i++) {
            x0[0] += ekf_anchors[i * 2];
            x0[1] += ekf_anchors[i * 2 + 1];
        }
        x0[0] /= (double)n_anchors;
        x0[1] /= (double)n_anchors;

        memset(ekf_cov, 0, sizeof(ekf_cov));
        for (int i = 0; i < 4; i++)
            ekf_cov[i * 4 + i] = EKF_P0;

        ekf_initialised = true;
    } else {
        memcpy(x0, ekf_state, sizeof(x0));
    }

    k_mutex_unlock(&beacon_mutex);

    run_direct_ekf(
        ekf_z,
        ekf_anchors,
        /*n_steps=*/1,
        n_anchors,
        EKF_DT,
        EKF_Q_VAR,
        EKF_R_VAR,
        EKF_P0,
        x0,
        &result
    );

    /* Persist state and covariance for the next update */
    memcpy(ekf_state, out_state, sizeof(ekf_state));
    memcpy(ekf_cov,   out_cov,   sizeof(ekf_cov));

    char     json[512];
    int      json_len;

    int device_mode = atomic_get(&current_mode);
    if (device_mode == MODE_BASE) {
        json_len = snprintf(json, sizeof(json),
                "{\"MessageType\":\"PositionData\","
                "\"Timestamp\":%lld,"
                "\"Data\":{" 
                "\"x\":%f,"
                "\"y\":%f,"
                "\"vx\":%f,"
                "\"vy\":%f}}",
                k_uptime_get(), ekf_state[0], ekf_state[1], ekf_state[2], ekf_state[3]);

        printk("%s\n", json);
    }
}

/* ------------------------------------------------------------------ */
/* Localisation thread - semaphore-driven                             */
/* ------------------------------------------------------------------ */

#define KALMAN_STACK_SIZE  4096
#define KALMAN_PRIORITY    5

K_SEM_DEFINE(localisation_sem, 0, 1);

static void localisation_thread(void *a, void *b, void *c)
{
    while (1) {
        /* Block until notify_func signals a new RSSI has arrived */
        k_sem_take(&localisation_sem, K_FOREVER);
        localisation_update();
    }
}

K_THREAD_DEFINE(localisation_tid, KALMAN_STACK_SIZE, localisation_thread,
                NULL, NULL, NULL, KALMAN_PRIORITY, 0, 0);




/* ========================================================================== */
/* UART                                                                       */
/* ========================================================================== */

// RECEIVING JSON STUFF
struct rx_data {
    const char* Command;
    const char* Mode;
};

static const struct json_obj_descr rx_data_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct rx_data, Command, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_data, Mode, JSON_TOK_STRING),
};

struct rx_beacon_add {
    const char *Command;
    const char *Mode;
    const char *name;
    const char *mac;
    int         major;
    int         minor;
    const char *left_name;
    const char *right_name;
    int      rssi_ref;
    int      X;
    int      Y;
};

static const struct json_obj_descr rx_beacon_add_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, Command,    JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, Mode,       JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, name,       JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, mac,        JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, major,      JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, minor,      JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, left_name,  JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, right_name, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, rssi_ref,   JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, X,          JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_add, Y,          JSON_TOK_NUMBER),
};

/* new - for remove_beacon command */
struct rx_beacon_remove {
    const char *Command;
    const char *Mode;
    const char *mac;   /* identify beacon to remove by MAC */
};

static const struct json_obj_descr rx_beacon_remove_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_remove, Command, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_remove, Mode,    JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_beacon_remove, mac,     JSON_TOK_STRING),
};

K_MSGQ_DEFINE(uart_msgq, MSG_MAX_LEN, MSG_QUEUE_SIZE, 4);

static void uart_cb(const struct device *dev, void *user_data)
{
    uint8_t c;

    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) return;

    while (uart_fifo_read(dev, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (rx_buf_pos > 0) {
                rx_buf[rx_buf_pos] = '\0';
                // printk("CB RAW (%d): %s\n", rx_buf_pos, rx_buf);
                if (k_msgq_put(&uart_msgq, rx_buf, K_NO_WAIT) != 0) {
                    printk("UART msgq full, dropping message\n");
                }
                rx_buf_pos = 0;
            }
        } else if (rx_buf_pos < (RECEIVE_BUFF_SIZE - 1)) {
            rx_buf[rx_buf_pos++] = c;
        }
    }
}

//THIS IS WHERE ALL THE JSON UART BULLSHIT IS BEING PARSED BTW

static void uart_rx_thread(void *a, void *b, void *c)
{
    char msg[MSG_MAX_LEN];

    if (!device_is_ready(uart_dev)) {
        printk("UART device not ready\n");
        return;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    uart_irq_rx_enable(uart_dev);
    printk("UART receiver thread started\n");

    while (1) {
        /* Block until a complete message arrives */
        k_msgq_get(&uart_msgq, msg, K_FOREVER);

        char msg_copy[512];
        strncpy(msg_copy, msg, sizeof(msg_copy));

        struct rx_data decoded_msg = {0};
        int ret = json_obj_parse(msg, strlen(msg), rx_data_descr, ARRAY_SIZE(rx_data_descr),
                                 &decoded_msg);

        // printk("MSG Recieved");

        if (ret < 0 || !(ret & BIT(0))) {
            printk("JSON Parse Error: %d\n", ret);
            continue;
        } else {
            // printk("Command: %s\n", decoded_msg.Command);
            // printk("Entered");
        }

        if (strcmp(decoded_msg.Command, "zero") == 0) {
            int err = nus_send_to_node("IMU", (const uint8_t *)"zero", 4);
        if (err) {
            LOG_ERR("Failed to send zero to IMU (err %d)", err);
        } else {
            LOG_INF("Sent zero command to IMU");
        }
        }

        if (strcmp(decoded_msg.Command, "collision") == 0) {
            int err = nus_send_to_node("IMU", (const uint8_t *)"collision", 7);
        if (err) {
            LOG_ERR("Failed to send collision to IMU (err %d)", err);
        } else {
            LOG_INF("Sent collision command to IMU");
        }
        }

        if (strcmp(decoded_msg.Command, "difficulty") == 0) {

            if (strcmp(decoded_msg.Mode, "easy") == 0) {

                int err = nus_send_to_node("MAKING-WAVES-RFID", (const uint8_t *)"esy", 3);

                if (err) {
                LOG_ERR("Failed to send collision to RFID (err %d)", err);
            } else {
                LOG_INF("Sent difficulty setting to RFID");
            }

            } else if (strcmp(decoded_msg.Mode, "medium") == 0) {

                int err = nus_send_to_node("MAKING-WAVES-RFID", (const uint8_t *)"med", 3);

                if (err) {
                LOG_ERR("Failed to send collision to RFID (err %d)", err);
            } else {
                LOG_INF("Sent difficulty setting to RFID");
            }

            } else if (strcmp(decoded_msg.Mode, "hard") == 0){

                int err = nus_send_to_node("MAKING-WAVES-RFID", (const uint8_t *)"hrd", 3);

                if (err) {
                LOG_ERR("Failed to send collision to RFID (err %d)", err);
            } else {
                LOG_INF("Sent difficulty setting to RFID");
            }

            }
        }

        // if (strcmp(decoded_msg.Command, "list_beacons") == 0) {
        //     // printk("list size: %d\n", anchor_beacons.size);
        //     // beacon_list_print(&anchor_beacons);

        //     // Package into a little JSON packet
        //     char beacon_packet[256];
        //     int json_len2;

        //     BeaconNode *current_beacon = anchor_beacons.head;
            
        //     // printk("I am about to print some beacons\n");
            
        //     // for (int i = 0; i < anchor_beacons.size; i++) {
        //     k_msleep(100);
        //     while (current_beacon) {
        //         // printk("This is gonna be a beacon\n");
                
        //         json_len2 = snprintf(beacon_packet, sizeof(beacon_packet),
        //                 "{\"MessageType\":\"SavedBeacon\","
        //                 "\"Data\":{"
        //                 "\"BLEName\":\"%s\","
        //                 "\"BLEMAC\":\"%s\","
        //                 "\"BLEMajor\":\"%u\","
        //                 "\"BLEMinor\":\"%u\"}}\r\n",
        //                 // "\"X\":\"%f\","
        //                 // "\"Y\":\"%f\","
        //                 // "\"RSSICal\":\"%d\","
        //                 // "\"LeftNeighbour\":\"%s\","
        //                 // "\"RightNeighbour\":\"%s\"}}\r\n",
        //             current_beacon->name, current_beacon->mac, current_beacon->major, current_beacon->minor); 
        //             // current_beacon->x, current_beacon->y, current_beacon->rssi_ref, 
        //             // current_beacon->left_name[0] ? current_beacon->left_name : "(none)",
        //             // current_beacon->right_name[0] ? current_beacon->right_name : "(none)");
                    
        //         printk("%s", beacon_packet);
        //         current_beacon = current_beacon->next;
        //         k_msleep(20);
                    
        //     }

        // } else if (strcmp(decoded_msg.Command, "add_beacon") == 0) {
        //     // printk("RAW: %s\n", msg);
        //     struct rx_beacon_add add_msg = {0};
        //     ret = json_obj_parse(msg_copy, strlen(msg_copy), rx_beacon_add_descr,
        //                  ARRAY_SIZE(rx_beacon_add_descr), &add_msg);
        //     // printk("add ret=%d expected=%d\n", ret, (1 << ARRAY_SIZE(rx_beacon_add_descr)) - 1);
        //     // printk("Command=%s Mode=%s name=%s mac=%s\n",
        //         //    add_msg.Command ? add_msg.Command : "NULL", add_msg.Mode ? add_msg.Mode : "NULL",
        //         //    add_msg.name ? add_msg.name : "NULL", add_msg.mac ? add_msg.mac : "NULL");
        //     // printk("major=%d minor=%d rssi=%d\n", add_msg.major, add_msg.minor, add_msg.rssi_ref);
        //     // printk("left=%s right=%s\n", add_msg.left_name ? add_msg.left_name : "NULL",
        //         //    add_msg.right_name ? add_msg.right_name : "NULL");
        //     if (ret < 0) {
        //         // printk("add_beacon parse error: %d\n", ret);
        //     } else {
        //         BeaconNode *n = beacon_list_push_back(
        //             &anchor_beacons, add_msg.name, add_msg.mac, (uint16_t)add_msg.major,
        //             (uint16_t)add_msg.minor, add_msg.X / 100, add_msg.Y / 100, /* x, y - add to JSON if needed */
        //             (int8_t)add_msg.rssi_ref, add_msg.left_name, add_msg.right_name);
        //         if (n) {
        //             // printk("{\"status\":\"ok\",\"msg\":\"beacon added: %s\"}\n", add_msg.name);
        //         } else {
        //             // printk("{\"status\":\"error\",\"msg\":\"failed to add beacon\"}\n");
        //         }
        //     }

        // } else if (strcmp(decoded_msg.Command, "remove_beacon") == 0) {
        //     // printk("Entered");
        //     // printk("RAW: %s\n", msg);
        //     struct rx_beacon_remove rem_msg = {0};
        //     ret = json_obj_parse(msg_copy, strlen(msg_copy), rx_beacon_remove_descr,
        //                          ARRAY_SIZE(rx_beacon_remove_descr), &rem_msg);
        //     if (ret < 0) {
        //         // printk("remove_beacon parse error: %d\n", ret);
        //     } else {
        //         BeaconNode *node = beacon_list_find_mac(&anchor_beacons, rem_msg.mac);
        //         if (node) {
        //             beacon_list_remove(&anchor_beacons, node);
        //             // printk("{\"status\":\"ok\",\"msg\":\"beacon removed: %s\"}\n", rem_msg.mac);
        //         } else {
        //             // printk("{\"status\":\"error\",\"msg\":\"beacon not found: %s\"}\n",
        //                 //    rem_msg.mac);
        //         }
        //     }

        // } else if (strcmp(decoded_msg.Command, "set_mode") == 0) {
        //     // printk("Entered");

        //     if (strcmp(decoded_msg.Mode, "base") == 0) {
        //         atomic_set(&current_mode, MODE_BASE);
        //         // printk("{\"status\":\"ok\",\"msg\":\"switched to base mode\"}\n");
        //     } else if (strcmp(decoded_msg.Mode, "sniffer") == 0) {
        //         atomic_set(&current_mode, MODE_SNIFFER);
        //         // printk("{\"status\":\"ok\",\"msg\":\"switched to sniffer mode\"}\n");
        //     } else {
        //         // printk("{\"status\":\"error\",\"msg\":\"unknown mode\"}\n");
        //     }
        // }
    }
}

K_THREAD_DEFINE(uart_rx_tid, UART_STACK_SIZE, uart_rx_thread,
                NULL, NULL, NULL, UART_PRIORITY, 0, 0);


/* ========================================================================== */
/* State Machine                                                              */
/* ========================================================================== */

enum node_state {
    STATE_BASE,
    STATE_SNIFFER,
};

static const char *const state_names[] = {
    [STATE_BASE]    = "BASE",
    [STATE_SNIFFER] = "SNIFFER",
};
static const struct bt_data ad[] = {
	
};

struct node_ctx {
    struct smf_ctx ctx;
};

static struct node_ctx node_ctx;
static void start_scan(void);

static void base_entry(void *obj)
{
    // printk("State: %s\n", state_names[STATE_BASE]);
    start_scan();
}

static enum smf_state_result base_run(void *obj)
{
    return SMF_EVENT_HANDLED;
}

static void sniffer_entry(void *obj)
{
    // printk("State: %s\n", state_names[STATE_SNIFFER]);
    start_scan();
}

static enum smf_state_result sniffer_run(void *obj)
{
    return SMF_EVENT_HANDLED;
}

static const struct smf_state states[] = {
    [STATE_BASE]    = SMF_CREATE_STATE(base_entry, base_run, NULL, NULL, NULL),
    [STATE_SNIFFER] = SMF_CREATE_STATE(sniffer_entry, sniffer_run, NULL, NULL, NULL),
};

/* ========================================================================== */
/* State Machine Thread                                                       */
/* ========================================================================== */

static K_SEM_DEFINE(smf_ready, 0, 1);

static void sm_thread(void *a, void *b, void *c)
{
    k_sem_take(&smf_ready, K_FOREVER);
    int ret;
    while (1) {
        ret = smf_run_state(SMF_CTX(&node_ctx));
        if (ret != 0) {
            // printk("State machine error: %d\n", ret);
            return;
        }
        k_msleep(100);
    }
}

K_THREAD_DEFINE(sm_tid, BT_STACK_SIZE, sm_thread, NULL, NULL, NULL,
                BT_PRIORITY, 0, 0);


/* ==========================================================================
 * NUS UUIDs
 *
 * Zephyr provides these via <zephyr/bluetooth/services/nus.h>:
 *   BT_UUID_NUS_SRV_VAL   - NUS Service         (6e400001-...)
 *   BT_UUID_NUS_TX_CHAR_VAL - NUS TX Characteristic (6e400003-...)
 *   BT_UUID_NUS_RX_CHAR_VAL - NUS RX Characteristic (6e400002-...)
 *
 * From the peripheral's perspective:
 *   TX char = peripheral sends notifications to us (central subscribes here)
 *   RX char = peripheral receives writes from us  (central writes here)
 * ========================================================================== */

/* ==========================================================================
 * State
 * ========================================================================== */

static struct bt_conn *default_conn;

static struct bt_uuid_128 discover_uuid;
static struct bt_gatt_discover_params discover_params;
// static struct bt_gatt_subscribe_params subscribe_params;

static uint16_t nus_rx_handle; /* Handle for writing to peripheral's RX */

/* ==========================================================================
 * GATT Notification Callback
 * ========================================================================== */
// TYNAN THIS IS THERE THE KALMAN STUFF WILL GO DO NOT LOSE WHERE THIS FUNCTION IS BECAUSE IT SUCKS BALLS TO KEEP SCROLLING THROUGH CODE FOR 20 MINUTES RATHER THAN USE CONTROL F LIKE A NORMAL PERSON

// Struct for incoming mobile node data
struct rx_mobile_node_data {
    const char* TYPE;
    const char* BLEMAC;
    int32_t BLEMajor;
    int32_t BLEMinor;
    int32_t RSSI;
};

static const struct json_obj_descr rx_mobile_node_data_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct rx_mobile_node_data, TYPE, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_mobile_node_data, BLEMAC, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct rx_mobile_node_data, BLEMajor, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct rx_mobile_node_data, BLEMinor, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct rx_mobile_node_data, RSSI, JSON_TOK_NUMBER),
};


static uint8_t notify_func(struct bt_conn *conn,
               struct bt_gatt_subscribe_params *params,
               const void *data, uint16_t length)
{

    //printk("notify_func called length=%u\n", length);
    if (!data) {
        LOG_WRN("Unsubscribed \n");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    //LOG_INF("Received %u bytes: \n", length);
    //LOG_HEXDUMP_INF(data, length, "NUS RX \n");

    // char addr[BT_ADDR_LE_STR_LEN];
    // bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    // LOG_INF("[%s] %.*s", addr, length, (const char *)data);

    // /* Also print as string if it looks like text */
    // const uint8_t *bytes = data; // Was originally const but fucked that off
    // bool printable = true;

    // for (uint16_t i = 0; i < length; i++) {
    //     if (bytes[i] < 0x20 && bytes[i] != '\n' && bytes[i] != '\r' &&
    //         bytes[i] != '\t') {
    //         printable = false;
    //         break;
    //     }
    // }

    // if (printable && length > 0) {
    //     LOG_INF("  \"%.*s\"", length, (const char *)data);
    // }

    // uint8_t data_string[length];
	// memcpy(data_string, data, length);

    // LOG_INF("SecondGo  \"%.*s\"", length, data_string);

     /* Get sender name */
    struct node_conn *node = get_node(conn);
    const char *name = (node && node->name[0]) ? node->name : "unknown";

    /* Copy to null-terminated buffer */
    char data_string[256];
    uint16_t len = MIN(length, sizeof(data_string) - 1);
    memcpy(data_string, data, len);
    data_string[len] = '\0';

    LOG_INF("[%s] %s", name, data_string);

    printk("%s", data_string);

    /* Check if it's an RFID packet */
    if (strstr(data_string, "\"TYPE\":\"rfid\"") != NULL) {
        LOG_INF("RFID packet received, skipping beacon parse");
        return BT_GATT_ITER_CONTINUE;
    }

    /* Forward IMU steering data to GUI */
if (strstr(data_string, "\"SteeringAngle\"") != NULL) {
    printk("%s\n", data_string);  /* Python GUI reads this */
    return BT_GATT_ITER_CONTINUE;
}

    // // DECODE JSON
    struct rx_mobile_node_data decoded_ble_data;
    int ret = json_obj_parse(data_string, length, rx_mobile_node_data_descr,
                        ARRAY_SIZE(rx_mobile_node_data_descr), &decoded_ble_data);

    if (ret < 0) {
        LOG_INF("JSON Parse Error: %d\n", ret);
    } else {
        //LOG_INF("BLEMAC: %s, RSSI: %d\n", decoded_ble_data.BLEMAC, decoded_ble_data.RSSI);
    }

    // Check if in sniffer mode
    int device_mode = atomic_get(&current_mode);
    if (device_mode == MODE_SNIFFER) {
        int ret;
        char buffer[256];

        struct sniffer_data_message newbledevice = {
            .BLEMAC = decoded_ble_data.BLEMAC,
            .BLEMajor = decoded_ble_data.BLEMinor,
            .BLEMinor = decoded_ble_data.BLEMajor,
            .RSSI = decoded_ble_data.RSSI
        };
        
        struct gui_sniffer_message transmission = {.MessageType = "Sniffer", .Timestamp = k_uptime_get(), .Data = newbledevice};
        ret = json_obj_encode_buf(gui_sniffer_message_descr, ARRAY_SIZE(gui_sniffer_message_descr),
                                &transmission,
                                buffer, sizeof(buffer));
        
        if (ret < 0) {
            // printk("Error encoding JSON");
        } else {
            printk("%s\n", buffer);
        }
    }

    // //Hoping and praying it works, we will find the beacon node and update it's RSSI measures
    // LOG_INF("Looking for MAC: '%s'", decoded_ble_data.BLEMAC);

    BeaconNode *dbg = anchor_beacons.head;
    // while (dbg) {
    //     // LOG_INF("  Stored MAC: '%s'", dbg->mac);
    //     dbg = dbg->next;
    // }
    BeaconNode* beacon_data = beacon_list_find_mac(&anchor_beacons, decoded_ble_data.BLEMAC);

    // LOG_INF("Before");
    // If not null, update
    if (beacon_data) {
        k_mutex_lock(&beacon_mutex, K_FOREVER);
        beacon_list_add_new_rssi(beacon_data, decoded_ble_data.RSSI);
        k_mutex_unlock(&beacon_mutex);
        k_sem_give(&localisation_sem);
    }

        return BT_GATT_ITER_CONTINUE;
}

/* ==========================================================================
 * GATT Discovery Callback
 *
 * Discovery proceeds in three stages:
 *   1. Discover NUS primary service
 *   2. Discover the TX characteristic (the one we subscribe to)
 *   3. Discover the CCC descriptor and subscribe to notifications
 * ========================================================================== */

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;
    struct node_conn *node = get_node(conn);
    if (!node) {
        LOG_ERR("discover_func: no node for conn");
        return BT_GATT_ITER_STOP;
    }

    if (!attr) {
        LOG_WRN("Discovery complete (no more attributes)");
        memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    /* Stage 1: Found NUS Service -> discover TX characteristic */
    if (!bt_uuid_cmp(node->discover_params.uuid,
                     BT_UUID_DECLARE_128(BT_UUID_NUS_SRV_VAL))) {

        memcpy(&node->discover_uuid,
               BT_UUID_DECLARE_128(BT_UUID_NUS_TX_CHAR_VAL),
               sizeof(node->discover_uuid));
        node->discover_params.uuid = &node->discover_uuid.uuid;
        node->discover_params.start_handle = attr->handle + 1;
        node->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        err = bt_gatt_discover(conn, &node->discover_params);
        if (err) {
            LOG_ERR("TX char discover failed (err %d)", err);
        }

    /* Stage 2: Found TX characteristic -> discover CCC descriptor */
    } else if (!bt_uuid_cmp(node->discover_params.uuid,
                            BT_UUID_DECLARE_128(BT_UUID_NUS_TX_CHAR_VAL))) {

        node->subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);

        memcpy(&node->discover_uuid, BT_UUID_GATT_CCC,
               sizeof(struct bt_uuid_16));
        node->discover_params.uuid = &node->discover_uuid.uuid;
        node->discover_params.start_handle = attr->handle + 2;
        node->discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

        err = bt_gatt_discover(conn, &node->discover_params);
        if (err) {
            LOG_ERR("CCC discover failed (err %d)", err);
        }

    /* Stage 3: Found CCC -> subscribe */
    } else {
        LOG_INF("CCC found, subscribing to notifications");

        node->subscribe_params.notify = notify_func;
        node->subscribe_params.value = BT_GATT_CCC_NOTIFY;
        node->subscribe_params.ccc_handle = attr->handle;

        err = bt_gatt_subscribe(conn, &node->subscribe_params);  /* use node-> */
        if (err && err != -EALREADY) {
            LOG_ERR("Subscribe failed (err %d)", err);
        } else {
            LOG_INF("Subscribed to NUS TX notifications");
            /* Now discover RX */
            discover_nus_rx(conn);
        }
    }

    return BT_GATT_ITER_STOP;
}

/* ==========================================================================
 * GATT Write (for sending data to peripheral's RX characteristic)
 * ========================================================================== */

static void write_func(struct bt_conn *conn, uint8_t err,
               struct bt_gatt_write_params *params)
{
    if (err) {
        LOG_ERR("Write failed (err %u)", err);
    } else {
        LOG_INF("Write complete (%u bytes)", params->length);
    }
}

static struct bt_gatt_write_params write_params;

static int nus_send(const uint8_t *data, uint16_t len)
{
    if (!default_conn) {
        LOG_ERR("Not connected");
        return -ENOTCONN;
    }

    if (nus_rx_handle == 0) {
        LOG_ERR("NUS RX handle not discovered");
        return -EINVAL;
    }

    write_params.func = write_func;
    write_params.handle = nus_rx_handle;
    write_params.offset = 0;
    write_params.data = data;
    write_params.length = len;

    return bt_gatt_write(default_conn, &write_params);
}

/* ==========================================================================
 * NUS RX Handle Discovery
 *
 * After subscribing to TX notifications, we also discover the RX
 * characteristic so we can write data to the peripheral.
 * ========================================================================== */

static struct bt_uuid_128 rx_discover_uuid;
static struct bt_gatt_discover_params rx_discover_params;

static uint8_t rx_discover_func(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                struct bt_gatt_discover_params *params)
{
    if (!attr) {
        LOG_WRN("NUS RX characteristic not found");
        memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    struct node_conn *node = get_node(conn);  /* <-- find the right node */
    if (!node) return BT_GATT_ITER_STOP;

    node->nus_rx_handle = bt_gatt_attr_value_handle(attr);  /* <-- per-node handle */
    LOG_INF("NUS RX handle discovered for '%s': %u", node->name, node->nus_rx_handle);

    return BT_GATT_ITER_STOP;
}

static void discover_nus_rx(struct bt_conn *conn)
{
    int err;
    struct node_conn *node = get_node(conn);
    if (!node) return;

    memcpy(&node->rx_discover_uuid,                          /* <-- per-node params */
           BT_UUID_DECLARE_128(BT_UUID_NUS_RX_CHAR_VAL),
           sizeof(node->rx_discover_uuid));

    node->rx_discover_params.uuid         = &node->rx_discover_uuid.uuid;
    node->rx_discover_params.func         = rx_discover_func;
    node->rx_discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    node->rx_discover_params.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    node->rx_discover_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;

    err = bt_gatt_discover(conn, &node->rx_discover_params);
    if (err) {
        LOG_ERR("RX discover failed (err %d)", err);
    }
}

/* ========================================================================== */
/* Scanner                                                                    */
/* ========================================================================== */

static void start_scan(void);

static bool parse_ad_for_nus(struct bt_data *data, void *user_data)
{
    bool *found = user_data;

    if (data->type == BT_DATA_UUID128_ALL ||
        data->type == BT_DATA_UUID128_SOME) {
        /* Each entry is 16 bytes (128-bit UUID) */
        if (data->data_len % 16 != 0) {
            return true;
        }

        for (uint16_t i = 0; i < data->data_len; i += 16) {
            struct bt_uuid_128 uuid;

            if (!bt_uuid_create(&uuid.uuid, &data->data[i], 16)) {
                continue;
            }

            if (!bt_uuid_cmp(&uuid.uuid,
                     BT_UUID_DECLARE_128(
                        BT_UUID_NUS_SRV_VAL))) {
                *found = true;
                return false; /* Stop parsing */
            }
        }
    }

    return true; /* Continue parsing */
}

static const char *target_names[] = { "MAKING-WAVES-RFID", "IMU"};

struct parse_name_result {
    bool found;
    char name[32];
};

static bool parse_name_cb(struct bt_data *data, void *user_data)
{
    struct parse_name_result *result = user_data;
    if (data->type == BT_DATA_NAME_COMPLETE) {
        for (int i = 0; i < ARRAY_SIZE(target_names); i++) {
            size_t len = strlen(target_names[i]);
            if (data->data_len == len &&
                memcmp(data->data, target_names[i], len) == 0) {
                result->found = true;
                memcpy(result->name, data->data, len);
                result->name[len] = '\0';
                return false;
            }
        }
    }
    return true;
}

// static bool parse_name_cb(struct bt_data *data, void *user_data)
// {
//     bool *found = user_data;
//     const char *target = "46387008";
//     const size_t target_len = strlen(target);

//     if (data->type == BT_DATA_NAME_COMPLETE &&
//         data->data_len == target_len &&
//         memcmp(data->data, target, target_len) == 0) {
//         *found = true;
//         return false;
//     }
//     return true;
// }

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                          struct net_buf_simple *ad)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    int err;

    if (smf_get_current_executing_state(SMF_CTX(&node_ctx)) == &states[STATE_BASE]) {

        /* Check free slot first */
        struct node_conn *node = get_free_node();
        if (!node) {
            return;  /* all slots full, stop looking */
        }

        if (type != BT_GAP_ADV_TYPE_ADV_IND &&
            type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
            type != BT_GAP_ADV_TYPE_SCAN_RSP) {
            return;
        }

        struct parse_name_result result = { .found = false, .name = {0} };
        bt_data_parse(ad, parse_name_cb, &result);
        if (!result.found) {
            return;
        }
    

bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
LOG_INF("NUS peripheral found: %s (%s) RSSI %d", addr_str, result.name, rssi);

for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].conn != NULL) {
        char existing[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(bt_conn_get_dst(nodes[i].conn),
                          existing, sizeof(existing));
        if (strcmp(existing, addr_str) == 0) {
            return;
        }
    }
}

        /* Stop scan, connect, then restart scan in connected() */
        if (bt_le_scan_stop()) {
            return;
        }

        err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                    BT_LE_CONN_PARAM_DEFAULT, &node->conn);
        if (err) {
            LOG_ERR("Create conn to %s failed (%d)", addr_str, err);
            node->conn = NULL;
            start_scan();
        }   else {
                strncpy(node->name, result.name, sizeof(node->name) - 1);  /* <-- add this */
                node->name[sizeof(node->name) - 1] = '\0';
                LOG_INF("Connection initiated to %s (%s)", addr_str, node->name);
        }   

    } else if (smf_get_current_executing_state(SMF_CTX(&node_ctx)) == &states[STATE_SNIFFER]) {
        bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
        LOG_INF("Device found: %s (RSSI %d)", addr_str, rssi);
    }
}

static void start_scan(void)
{
    static const struct bt_le_scan_param scan_params = {
        .type     = BT_LE_SCAN_TYPE_ACTIVE,
        .options  = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
        .window   = BT_GAP_SCAN_FAST_WINDOW,
    };

    int err = bt_le_scan_start(&scan_params, device_found);
    if (err && err != -EALREADY) {  /* ignore -EALREADY */
        printk("Scanning failed to start (err %d)\n", err);
        return;
    }
    printk("Scanning started\n");
}

static int nus_send_to_node(const char *name, const uint8_t *data, uint16_t len)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].conn == NULL) continue;
        if (strcmp(nodes[i].name, name) != 0) continue;

        if (nodes[i].nus_rx_handle == 0) {
            LOG_ERR("NUS RX handle not yet discovered for %s", name);
            return -EINVAL;
        }

        nodes[i].write_params.func   = write_func;
        nodes[i].write_params.handle = nodes[i].nus_rx_handle;
        nodes[i].write_params.offset = 0;
        nodes[i].write_params.data   = data;
        nodes[i].write_params.length = len;

        return bt_gatt_write(nodes[i].conn, &nodes[i].write_params);
    }

    LOG_ERR("Node '%s' not connected", name);
    return -ENOENT;
}

/* ==========================================================================
 * Data Length Extension and MTU Exchange
 *
 * These are two separate negotiations:
 *   1. DLE  -- over-the-air packet size (bt_conn_le_data_len_update)
 *   2. MTU  -- GATT payload size        (bt_gatt_exchange_mtu)
 * Both must be requested; each result is min(ours, theirs).
 * ========================================================================== */

static void update_data_length(struct bt_conn *conn)
{
    int err;
    struct bt_conn_le_data_len_param dl_param = {
        .tx_max_len = BT_GAP_DATA_LEN_MAX,
        .tx_max_time = BT_GAP_DATA_TIME_MAX,
    };

    err = bt_conn_le_data_len_update(conn, &dl_param);
    if (err) {
        LOG_ERR("Data length update failed (err %d)", err);
    }
}

static void on_le_data_len_updated(struct bt_conn *conn,
                   struct bt_conn_le_data_len_info *info)
{
    // LOG_INF("Data length updated: TX %u bytes (%u us), RX %u bytes (%u us)",
    //     info->tx_max_len, info->tx_max_time,
    //     info->rx_max_len, info->rx_max_time);
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
                struct bt_gatt_exchange_params *params)
{
    if (err) {
        LOG_ERR("MTU exchange failed (err %u)", err);
    } else {
        uint16_t payload_mtu = bt_gatt_get_mtu(conn) - 3;

        LOG_INF("MTU exchange successful: ATT MTU %u, payload %u bytes",
            bt_gatt_get_mtu(conn), payload_mtu);
    }
}

static struct bt_gatt_exchange_params mtu_exchange_params = {
    .func = mtu_exchange_cb,
};

/* ==========================================================================
 * Connection Callbacks
 * ========================================================================== */


 static void scan_restart_work_fn(struct k_work *work)
{
    if (get_free_node()) {
        start_scan();
    }
}

 static K_WORK_DELAYABLE_DEFINE(scan_restart_work, scan_restart_work_fn);

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    printk("connected() called err=%u\n", conn_err);
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (err %u)", addr, conn_err);
        struct node_conn *node = get_node(conn);
        if (node) {
            bt_conn_unref(node->conn);
            memset(node, 0, sizeof(*node));
        }
        start_scan();
        return;
    }

    /* Count active connections */
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].conn != NULL) count++;
    }

    printk("Connected to %s (%d/%d active)\n", addr, count, MAX_NODES);

    LOG_INF("Connected: %s", addr);

    struct node_conn *node = get_node(conn);
    if (!node) {
        LOG_ERR("No node slot found for connection");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    update_data_length(conn);

    node->mtu_exchange_params.func = mtu_exchange_cb;
    bt_gatt_exchange_mtu(conn, &node->mtu_exchange_params);

    memcpy(&node->discover_uuid,
           BT_UUID_DECLARE_128(BT_UUID_NUS_SRV_VAL),
           sizeof(node->discover_uuid));
    node->discover_params.uuid = &node->discover_uuid.uuid;
    node->discover_params.func = discover_func;
    node->discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    node->discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    node->discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    bt_gatt_discover(conn, &node->discover_params);

    discover_nus_rx(conn);

    /* Keep scanning for more nodes */
    if (get_free_node()) {
        k_work_schedule(&scan_restart_work, K_MSEC(1000));
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

    struct node_conn *node = get_node(conn);
    if (!node) return;

    bt_conn_unref(node->conn);
    node->conn = NULL;          /* explicitly NULL after unref */
    node->nus_rx_handle = 0;
    memset(node, 0, sizeof(*node));  /* clear everything */

    k_work_schedule(&scan_restart_work, K_MSEC(500));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected,
    .disconnected = disconnected,
};

/* ========================================================================== */
/* Main                                                                       */
/* ========================================================================== */

void inital_beacons() {
    beacon_list_init(&anchor_beacons);

    /*
     * beacon_list_push_back(list,
     *     name,  mac,  major, minor,  x,    y,   rssi_ref,
     *     left_name, right_name)
     *
     * x/y: update with real survey coordinates (metres).
     * rssi_ref: replace -65 with your per-node calibrated value (dBm).
     */
    beacon_list_push_back(&anchor_beacons,
        "4011-A", "F5:75:FE:85:34:67 (random)",  2753, 32998,  3.4, 0, -52,
        "",        "4011-B");

    beacon_list_push_back(&anchor_beacons,
        "4011-B", "E5:73:87:06:1E:86 (random)", 32975, 20959,  1.7, 0, -52,
        "4011-A",  "4011-C");

    beacon_list_push_back(&anchor_beacons,
        "4011-C", "CA:99:9E:FD:98:B1 (random)", 26679, 40363,  0.0, 0.0, -58,
        "4011-B",  "4011-D");

    beacon_list_push_back(&anchor_beacons,
        "4011-D", "CB:1B:89:82:FF:FE (random)", 41747, 38800,  0.0, 1.5, -59,
        "4011-C",  "4011-E");

    beacon_list_push_back(&anchor_beacons,
        "4011-E", "D4:D2:A0:A4:5C:AC (random)", 30679, 51963,  0.0, 3.88, -58,
        "4011-D",  "4011-F");

    beacon_list_push_back(&anchor_beacons,
        "4011-F", "C1:13:27:E9:B7:7C (random)",  6195, 18394,  0.0, 5.44, -52,
        "4011-E",  "4011-G");

    beacon_list_push_back(&anchor_beacons,
        "4011-G", "F1:04:48:06:39:A0 (random)", 30525, 30544,  0.0, 6.99, -57,
        "4011-F",  "4011-H");

    beacon_list_push_back(&anchor_beacons,
        "4011-H", "CA:0C:E0:DB:CE:60 (random)", 57395, 28931,  1.7, 6.99, -49,
        "4011-G",  "4011-I");

    // BEACON I WAS DEAD WHEN WE CAL'D
    beacon_list_push_back(&anchor_beacons,
        "4011-I", "D4:7F:D4:7C:20:13 (random)", 60345, 49995,  3.4, 6.99, -45,
        "4011-H",  "4011-J");
    // BEACON I WAS DEAD WHEN WE CAL'D

    beacon_list_push_back(&anchor_beacons,
        "4011-J", "F7:0B:21:F1:C8:E1 (random)", 12249, 30916,  3.4, 5.44, -62,
        "4011-I",  "4011-K");

    beacon_list_push_back(&anchor_beacons,
        "4011-K", "FD:E0:8D:FA:3E:4A (random)", 36748, 11457,  3.4, 3.88, -60,
        "4011-J",  "4011-L");

    beacon_list_push_back(&anchor_beacons,
        "4011-L", "EE:32:F7:28:FA:AC (random)", 27564, 27589,  3.4, 1.5, -62,
        "4011-K",  "4011-M");

    beacon_list_push_back(&anchor_beacons,
        "4011-M", "F7:3B:46:A8:D7:2C (random)", 49247, 52925,  1.7, 3.88, -63,
        "4011-L",  "");

    // beacon_list_push_back(&anchor_beacons,
    //     "4011-N", "HELLO HELLO HELLO", 49247, 52925,  1.7, 3.88, -69,
    //     "4011-L",  "");

    /* Print the full list */
    // beacon_list_print(&anchor_beacons);

    // /* Find by MAC */
    // puts("\n--- Find by MAC: D4:D2:A0:A4:5C:AC ---");
    // BeaconNode *n = beacon_list_find_mac(&list, "D4:D2:A0:A4:5C:AC");
    // if (n)
    //     printf("  Found: %s  rssi_ref=%d dBm\n", n->name, (int)n->rssi_ref);

    // /* Find by name */
    // puts("\n--- Find by name: 4011-G ---");
    // BeaconNode *m = beacon_list_find_name(&list, "4011-G");
    // if (m)
    //     printf("  Found: %s  mac=%s  left=%s  right=%s  rssi_ref=%d dBm\n",
    //            m->name, m->mac, m->left_name, m->right_name, (int)m->rssi_ref);

    // beacon_list_destroy(&list);
    // return 0;
}


int main(void)
{
    int ret;    
    char buffer[256];
    
    // ====== UART Receiving ======
    // Set up UART interface for rx commands
    // if (!device_is_ready(uart_dev)) {
    //     printk("UART device not ready\n");
    //     return 0;
    // }

    // // Register the callback
    // uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);
    // // Enable receiving
    // uart_irq_rx_enable(uart_dev);

    // printk("UART receiver started\n");

    // while (1) {
    //     if (msg_received) {
    //         struct rx_data decoded_msg;
            
    //         // Parse the rx_buf
    //         int ret = json_obj_parse(rx_buf, rx_buf_pos, rx_data_descr,
    //                                ARRAY_SIZE(rx_data_descr), &decoded_msg);

    //         if (ret < 0) {
    //             printk("JSON Parse Error: %d\n", ret);
    //         } else {
    //             printk("Command: %s, Mode: %d\n", decoded_msg.Command, decoded_msg.Mode);
    //         }

    //         // Reset for next message
    //         rx_buf_pos = 0;
    //         msg_received = false;
    //     }
    //     k_sleep(K_MSEC(10));
    // }

    //return 0;
    
    // ====== UART TXing ======
    // struct position_data_message mobile_coords = {.x = 1, .y = 2, .z = 3};
    // struct gui_position_message transmission = {.MessageType = "PositionData", .Timestamp = k_uptime_get(), .Data = mobile_coords};

    // struct beacon_data_message newbeacon = {
    //     .Operation = 0,
    //     .BLEName = "4011-Z",
    //     .BLEMAC = "12:34:56:78:ab",
    //     .BLEMajor = 12345,
    //     .BLEMinor = 6789,
    //     .x = 1,
    //     .y = 2,
    //     .z = 3,
    //     .LeftNeighbour = "4011-Y",
    //     .RightNeighbour = "4011-X"
    // };

    // struct gui_beacon_message transmission_2 = {.MessageType = "Beacon", .Timestamp = k_uptime_get(), .Data = newbeacon};

    // struct sniffer_data_message newbledevice = {
    //     .BLEName = "BingBong",
    //     .BLEMAC = "12:34:56:78:ab",
    //     .RSSI = 67
    // };
    
    // struct gui_sniffer_message transmission_3 = {.MessageType = "Sniffer", .Timestamp = k_uptime_get(), .Data = newbledevice};


    // ret = json_obj_encode_buf(gui_position_message_descr, ARRAY_SIZE(gui_position_message_descr),
    //                           &transmission,
    //                           buffer, sizeof(buffer));

    // ret = json_obj_encode_buf(gui_beacon_message_descr, ARRAY_SIZE(gui_beacon_message_descr),
    //                           &transmission_2,
    //                           buffer, sizeof(buffer));

    // ret = json_obj_encode_buf(gui_sniffer_message_descr, ARRAY_SIZE(gui_sniffer_message_descr),
    //                           &transmission_3,
    //                           buffer, sizeof(buffer));


    // if (ret < 0) {
    //     printk("Error encoding JSON");
    //     return 1;
    // }


    // while (true) {
    //     printk("%s\n", buffer);
    //     //k_msleep(1000);
    // }

    // return 0;

    // ====== Everything that was here before i deepy apologise for commenting it out and leaving it in grey but it is currently 3am and i want to go to bed and enjoy a nice sleep any maybe dream of a sheep or a cow and then wake up and continue coding until this thing can localise to the exact atom of the universe the bluetooth receiver is sitting on and have an interface that looks like it came from the Louvre and would replace Leonardo DaVinci's Mona Lisa ======
    
    int err;

    err = bt_enable(NULL);

    inital_beacons();
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }
    LOG_INF("Bluetooth initialised");


    /* Schedule periodic rescan in case nodes come online later */
    k_work_schedule(&scan_restart_work, K_MSEC(5000));
    smf_set_initial(SMF_CTX(&node_ctx), &states[STATE_BASE]);
    k_sem_give(&smf_ready);

    // Send some test message to the IMU NUS peripheral every 5 seconds
    // while (true) {
    //     k_msleep(5000);
    //     int err = nus_send_to_node("IMU", (const uint8_t *)"zero", 4);
    //     if (err) {
    //         LOG_ERR("Failed to send zero to IMU (err %d)", err);
    //     } else {
    //         LOG_INF("Sent zero command to IMU");
    //     }
    // }


    // TESTING GRAVEYARD
    // int device_mode;
    // while (true) {
    //     device_mode = atomic_get(&current_mode);
    //     if (device_mode == MODE_BASE) {
    //             printk("Base mode!\n");
    //     } else if (device_mode == MODE_SNIFFER) {
    //             printk("Sniffer mode!\n");
    //     }

    //     k_msleep(100);
    // }

    return 0;


}
