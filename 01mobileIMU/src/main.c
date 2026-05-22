/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#define LED1_NODE DT_ALIAS(led1) //green
#define LED2_NODE DT_ALIAS(led2) //blue
#define LED0_NODE DT_ALIAS(led0) //red

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(LED0_NODE, gpios);


#define DEVICE_NAME		CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN		(sizeof(DEVICE_NAME) - 1)
LOG_MODULE_REGISTER(rc522, LOG_LEVEL_INF);


static void flash_colour(const char *colour)
{
    /* Set all off first */
    gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_INACTIVE);

    if (strcmp(colour, "purple") == 0) {
        gpio_pin_configure_dt(&led_red,  GPIO_OUTPUT_ACTIVE);
        gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_ACTIVE);
    } else if (strcmp(colour, "green") == 0) {
        gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_ACTIVE);
    } else if (strcmp(colour, "yellow") == 0) {
        gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_ACTIVE);
        gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_ACTIVE);
    } else if (strcmp(colour, "white") == 0) {
        gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_ACTIVE);
        gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_ACTIVE);
        gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_ACTIVE);
    }

    k_msleep(1000);

    gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_INACTIVE);
}


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
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
                    uint8_t adv_type, struct net_buf_simple *buf)
{
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

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

static void adv_restart_work_fn(struct k_work *work)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        printk("Failed to restart advertising: %d\n", err);
    } else {
        printk("Advertising restarted\n");
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

    gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_ACTIVE);
    k_msleep(500);
    gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);

    while (1) {
        // Do nothing for now
        k_msleep(200);
}

return 0;
}
