
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

#include <math.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define BT_STACK_SIZE   1024
#define BT_PRIORITY     5

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_console)

#define RECEIVE_BUFF_SIZE 512

#define UART_STACK_SIZE   2048
#define KALMAN_STACK_SIZE   1024
#define UART_PRIORITY     5
#define KALMAN_PRIORITY    4

/* Message queue to pass complete lines from ISR to UART thread */
#define MSG_QUEUE_SIZE     8
#define MSG_MAX_LEN        RECEIVE_BUFF_SIZE

#define MODE_SNIFFER  0
#define MODE_BASE     1

#define MAX_NODES 2

static atomic_t current_mode = ATOMIC_INIT(MODE_BASE);
static char rx_buf[RECEIVE_BUFF_SIZE];
static int rx_buf_pos = 0;
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);


LOG_MODULE_REGISTER(nus_central, LOG_LEVEL_INF);

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

/* new - for remove_beacon command */
struct rx_beacon_remove {
    const char *Command;
    const char *Mode;
    const char *mac;   /* identify beacon to remove by MAC */
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

// static void sm_thread(void *a, void *b, void *c)
// {
//     k_sem_take(&smf_ready, K_FOREVER);
//     int ret;
//     while (1) {
//         ret = smf_run_state(SMF_CTX(&node_ctx));
//         if (ret != 0) {
//             // printk("State machine error: %d\n", ret);
//             return;
//         }
//         k_msleep(100);
//     }
// }

// K_THREAD_DEFINE(sm_tid, BT_STACK_SIZE, sm_thread, NULL, NULL, NULL,
//                 BT_PRIORITY, 0, 0);


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
 * GATT Notification Callback
 * ========================================================================== */

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

/* ==========================================================================
 * NUS RX Handle Discovery
 *
 * After subscribing to TX notifications, we also discover the RX
 * characteristic so we can write data to the peripheral.
 * ========================================================================== */

// static struct bt_uuid_128 rx_discover_uuid;
// static struct bt_gatt_discover_params rx_discover_params;

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


int main(void)
{
    
    int err;

    err = bt_enable(NULL);

    // inital_beacons();
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }
    LOG_INF("Bluetooth initialised");


    /* Schedule periodic rescan in case nodes come online later */
    k_work_schedule(&scan_restart_work, K_MSEC(5000));
    smf_set_initial(SMF_CTX(&node_ctx), &states[STATE_BASE]);
    k_sem_give(&smf_ready);

    return 0;


}
