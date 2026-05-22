/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/bluetooth/hci.h>

#define DEVICE_NAME		CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN		(sizeof(DEVICE_NAME) - 1)

/* == Advertising data ===================================================== */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

/* == NUS callbacks ======================================================== */
static void notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);

	printk("%s() - %s\n", __func__, (enabled ? "Enabled" : "Disabled"));
}

static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	printk("%s() - Len: %d, Message: %.*s\n", __func__, len, len, (char *)data);
}

struct bt_nus_cb nus_listener = {
	.notif_enabled = notif_enabled,
	.received = received,
};

/* == Scan callback ======================================================== */
#define APPLE_COMPANY_ID     0x004C
#define IBEACON_TYPE         0x02
#define IBEACON_LENGTH       0x15

static bool extract_ibeacon(struct net_buf_simple *buf,
                             uint8_t *uuid,      /* 16 bytes out */
                             uint16_t *major,
                             uint16_t *minor)
{
    *major    = 0;
    *minor    = 0;

    struct net_buf_simple_state state;
    net_buf_simple_save(buf, &state);

    while (buf->len > 1) {
        uint8_t len  = net_buf_simple_pull_u8(buf);
        if (len == 0 || len > buf->len) break;

        uint8_t type = net_buf_simple_pull_u8(buf);
        len--;

        if (type == BT_DATA_MANUFACTURER_DATA && len >= 25) {
            /* Company ID - little-endian uint16 */
            uint16_t company = net_buf_simple_pull_le16(buf);
            len -= 2;

            if (company == APPLE_COMPANY_ID) {
                uint8_t ib_type   = net_buf_simple_pull_u8(buf);
                uint8_t ib_length = net_buf_simple_pull_u8(buf);
                len -= 2;

                if (ib_type == IBEACON_TYPE && ib_length == IBEACON_LENGTH && len >= 21) {
                    /* 16-byte UUID */
                    memcpy(uuid, net_buf_simple_pull_mem(buf, 16), 16);

                    /* Major / minor are big-endian in iBeacon */
                    *major    = net_buf_simple_pull_be16(buf);
                    *minor    = net_buf_simple_pull_be16(buf);

                    net_buf_simple_restore(buf, &state);
                    return true;
                }
            }
        }

        /* Skip remainder of this AD structure */
        if (len > 0) {
            net_buf_simple_pull_mem(buf, len);
        }
    }

    net_buf_simple_restore(buf, &state);
    return false;
}


static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t adv_type, struct net_buf_simple *buf)
{
    char     addr_str[BT_ADDR_LE_STR_LEN];
    uint8_t  uuid[16] = {0};
    uint16_t major    = 0;
    uint16_t minor    = 0;
    char     json[512];
    int      json_len;

    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

    if (extract_ibeacon(buf, uuid, &major, &minor)) {
        /* iBeacon */
        json_len = snprintf(json, sizeof(json),
            "{\"TYPE\":\"ibeacon\","
             "\"BLEMAC\":\"%s\","
             "\"BLEMajor\":%u,"
             "\"BLEMinor\":%u,"
             "\"RSSI\":%d}\r\n",
            addr_str, major, minor, rssi);
    } else {
        // /* Generic BLE device */
        // json_len = snprintf(json, sizeof(json),
        //     "{\"TYPE\":\"ble\","
        //      "\"BLEMAC\":\"%s\","
        //      "\"BLEMajor\":0,"
        //      "\"BLEMinor\":0,"
        //      "\"RSSI\":%d}\r\n",
        //     addr_str, rssi);
        return;
    }

    printk("%s", json);

    if (json_len > 0 && json_len < sizeof(json)) {
        int err = bt_nus_send(NULL, (uint8_t *)json, (uint16_t)json_len);
        if (err < 0 && err != -EAGAIN && err != -ENOTCONN) {
            printk("bt_nus_send failed: %d\n", err);
        }

    }
}

/* == Scan parameters ====================================================== */
static struct bt_le_scan_param scan_params = {
    .type     = BT_LE_SCAN_TYPE_PASSIVE,
    .options  = BT_LE_SCAN_OPT_NONE,
    .interval = BT_GAP_SCAN_FAST_INTERVAL,
    .window   = BT_GAP_SCAN_FAST_WINDOW,
};

static void exchange_func(struct bt_conn *conn, uint8_t err,
                          struct bt_gatt_exchange_params *params)
{
    if (!err) {
        printk("MTU exchange done, MTU: %d\n", bt_gatt_get_mtu(conn));
    } else {
        printk("MTU exchange failed: %d\n", err);
    }
}

static struct bt_gatt_exchange_params exchange_params = {
    .func = exchange_func,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed: %d\n", err);
        return;
    }
    printk("Connected\n");

    err = bt_gatt_exchange_mtu(conn, &exchange_params);
    if (err) {
        printk("MTU exchange failed to start: %d\n", err);
    }
}

static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_fn);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected, reason: %d\n", reason);
    k_work_schedule(&adv_restart_work, K_MSEC(500));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected,
    .disconnected = disconnected,
};


/* == Entry point ========================================================== */
int main(void)
{
	int err;

	printk("Sample - Bluetooth Scanner + Peripheral NUS\n");

    /* == NUS callback registration ============ */
	err = bt_nus_cb_register(&nus_listener, NULL);
	if (err) {
		printk("Failed to register NUS callback: %d\n", err);
		return err;
	}

    /* == Bluetooth init =================================================== */
	err = bt_enable(NULL);
	if (err) {
		printk("Failed to enable bluetooth: %d\n", err);
		return err;
	}

    /* == Advertise so a NUS central can connect =========================== */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Failed to start advertising: %d\n", err);
		return err;
	}

    /* == Start scanning simultaneously ==================================== */
    err = bt_le_scan_start(&scan_params, scan_cb);
    if (err) {
        printk("Failed to start scanning: %d\n", err);
        return err;
    }

	printk("Initialization complete\n");
    
	while (true) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
