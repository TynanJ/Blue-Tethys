/*
 * Copyright (c) 2024 Croxel, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>

// IMU Related
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/util.h>
#include <math.h>

// NUS Related
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/bluetooth/hci.h>

// Other
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#define LED0_NODE DT_ALIAS(led0) //red
#define LED1_NODE DT_ALIAS(led1) //green
#define LED2_NODE DT_ALIAS(led2) //blue

#define PI 3.1415926543

#define MAX_Y_HEAD_THRESHOLD PI/4
#define MIN_Y_HEAD_THRESHOLD -PI/4

static int print_samples;
static int lsm6dsl_trig_cnt;

static struct sensor_value accel_x_out, accel_y_out, accel_z_out;

#ifdef CONFIG_LSM6DSL_TRIGGER
static void lsm6dsl_trigger_handler(const struct device *dev,
				    const struct sensor_trigger *trig)
{
	static struct sensor_value accel_x, accel_y, accel_z;
	static struct sensor_value gyro_x, gyro_y, gyro_z;
	lsm6dsl_trig_cnt++;

	sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, &accel_x);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, &accel_y);
	sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, &accel_z);

	/* lsm6dsl gyro */
	sensor_sample_fetch_chan(dev, SENSOR_CHAN_GYRO_XYZ);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_X, &gyro_x);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_Y, &gyro_y);
	sensor_channel_get(dev, SENSOR_CHAN_GYRO_Z, &gyro_z);

	if (print_samples) {
		print_samples = 0;

		accel_x_out = accel_x;
		accel_y_out = accel_y;
		accel_z_out = accel_z;
	}

}
#endif

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
    char out_str[64];
	struct sensor_value odr_attr;
	const struct device *const lsm6dsl_dev = DEVICE_DT_GET_ONE(st_lsm6dsl);
	double current_accel_x = 0;
	double current_accel_y = 0;
	double current_accel_z = 0;

	double current_head_yz = 0;
	double current_head_xz = 0;

	printk("IMU with Bluetooth output\n");

    /* == LED Initialization ============ */
	if (!gpio_is_ready_dt(&led_red)) {
		return 0;
    }

	if (!gpio_is_ready_dt(&led_green)) {
		return 0;
    }

	if (!gpio_is_ready_dt(&led_blue)) {
		return 0;
    }

    /* == IMU Registration ============ */
	if (!device_is_ready(lsm6dsl_dev)) {
		printk("sensor: device not ready.\n");
		return 0;
	}

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

    // Configure LED pins
	err = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_ACTIVE);
    if (err < 0) {
        return 0;
    }

	err = gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_ACTIVE);
    if (err < 0) {
        return 0;
    }

	err = gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_ACTIVE);
    if (err < 0) {
        return 0;
    }

	/* set accel/gyro sampling frequency to 104 Hz */
	odr_attr.val1 = 104;
	odr_attr.val2 = 0;

	if (sensor_attr_set(lsm6dsl_dev, SENSOR_CHAN_ACCEL_XYZ,
			    SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr) < 0) {
		printk("Cannot set sampling frequency for accelerometer.\n");
		return 0;
	}

	if (sensor_attr_set(lsm6dsl_dev, SENSOR_CHAN_GYRO_XYZ,
			    SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr) < 0) {
		printk("Cannot set sampling frequency for gyro.\n");
		return 0;
	}

#ifdef CONFIG_LSM6DSL_TRIGGER
	struct sensor_trigger trig;

	trig.type = SENSOR_TRIG_DATA_READY;
	trig.chan = SENSOR_CHAN_ACCEL_XYZ;

	if (sensor_trigger_set(lsm6dsl_dev, &trig, lsm6dsl_trigger_handler) != 0) {
		printk("Could not set sensor type and channel\n");
		return 0;
	}
#endif

	if (sensor_sample_fetch(lsm6dsl_dev) < 0) {
		printk("Sensor sample update error\n");
		return 0;
	}

	// Reset LEDs
	gpio_pin_set_dt(&led_red, 0);
	gpio_pin_set_dt(&led_green, 0);
	gpio_pin_set_dt(&led_blue, 0);

	printk("Initialization complete\n");


	while (1) {
		// Read accelerometer headings
		current_accel_x = sensor_value_to_double(&accel_x_out);
		current_accel_y = sensor_value_to_double(&accel_y_out);
		current_accel_z = sensor_value_to_double(&accel_z_out);

		// Convert accels to heading
		current_head_yz = atan(current_accel_y / sqrt(current_accel_x*current_accel_x + current_accel_z*current_accel_z));
		current_head_xz = atan(current_accel_x / sqrt(current_accel_y*current_accel_y + current_accel_z*current_accel_z));
		
		// Print current heading
		sprintf(out_str, "Current heading: %f",
						current_head_yz);
		printk("%s\n", out_str);

		// Change LED Based on Y axis
		if (current_head_xz > MAX_Y_HEAD_THRESHOLD) {
			gpio_pin_set_dt(&led_green, 1);
		} else if (current_head_xz < MIN_Y_HEAD_THRESHOLD) {
			gpio_pin_set_dt(&led_red, 1);
		} else {
			gpio_pin_set_dt(&led_green, 0);
			gpio_pin_set_dt(&led_red, 0);
		}

		// Looping stuff
		print_samples = 1;
		k_sleep(K_MSEC(10));
	}
}
