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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// NUS Related
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/bluetooth/hci.h>

// Other
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mobileIMU, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0) //red
#define LED1_NODE DT_ALIAS(led1) //green
#define LED2_NODE DT_ALIAS(led2) //blue

// #define PI 3.1415926543

#define MAX_Y_HEAD_THRESHOLD M_PI/4
#define MIN_Y_HEAD_THRESHOLD -M_PI/4

/* Maximum steering angle in degrees (saturation limit).
 * A typical ship wheel may do 3–5 full turns lock-to-lock.
 * 3 turns = 1080° */
#define MAX_STEERING_ANGLE_DEG   1080.0f

/* Sample interval in milliseconds */
#define SAMPLE_INTERVAL_MS       10      /* 100 Hz */

/* Drift correction: weight given to accel-derived angle vs integrated gyro.
 * Range 0.0 (gyro only) to 1.0 (accel only).
 * Small value = slow but stable correction. */
#define DRIFT_CORRECTION_ALPHA   0.005f

/* Gyro low-pass filter coefficient (0 = no filter, 1 = frozen) */
#define GYRO_LPF_ALPHA           0.2f

/* Sign of gyro Z relative to desired positive steering direction.
 * Flip to -1.0f if wheel angle increases in the wrong direction. */
#define GYRO_Z_SIGN              1.0f

/* Stationary threshold: gyro readings below this (deg/s) are zeroed
 * to prevent integration of noise while the wheel is still. */
#define GYRO_DEADBAND_DEG_S      0.3f

/* How often to log the angle (every N samples) */
#define LOG_EVERY_N_SAMPLES      50

static int print_samples;
static int lsm6dsl_trig_cnt;

static struct sensor_value accel_x_out, accel_y_out, accel_z_out;
static struct sensor_value gyro_x_out, gyro_y_out, gyro_z_out;

static float g_cumulative_angle_deg = 0.0f;   /* Integrated, unclamped   */
static float g_steering_angle_deg   = 0.0f;   /* Clamped output          */
static float g_gyro_z_filtered      = 0.0f;   /* Low-pass filtered gyro  */


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

		gyro_x_out = gyro_x;
		gyro_y_out = gyro_y;
		gyro_z_out = gyro_z;
	}

}
#endif

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(LED0_NODE, gpios);


#define DEVICE_NAME		CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN		(sizeof(DEVICE_NAME) - 1)

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

	LOG_INF("%s() - %s\n", __func__, (enabled ? "Enabled" : "Disabled"));
}

static void received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);

	LOG_INF("%s() - Len: %d, Message: %.*s\n", __func__, len, len, (char *)data);
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
        LOG_INF("MTU exchange done, MTU: %d\n", bt_gatt_get_mtu(conn));
    } else {
        LOG_INF("MTU exchange failed: %d\n", err);
    }
}

static struct bt_gatt_exchange_params exchange_params = {
    .func = exchange_func,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_INF("Connection failed: %d\n", err);
        return;
    }
    LOG_INF("Connected\n");

    err = bt_gatt_exchange_mtu(conn, &exchange_params);
    if (err) {
        LOG_INF("MTU exchange failed to start: %d\n", err);
    }
}

static void adv_restart_work_fn(struct k_work *work)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_INF("Failed to restart advertising: %d\n", err);
    } else {
        LOG_INF("Advertising restarted\n");
    }
}

static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_fn);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected, reason: %d\n", reason);
    k_work_schedule(&adv_restart_work, K_MSEC(500));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected,
    .disconnected = disconnected,
};


/* == Wheel Angle Sensing ========================================================== */

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}



/**
 * Convert a sensor_value to float.
 */
static inline float sensor_val_to_float(const struct sensor_value *val)
{
    return (float)val->val1 + (float)val->val2 * 1e-6f;
}


/**
 * Derive the within-one-revolution angle from the accelerometer.
 *
 * With the IMU flat (wheel face vertical, rotation axis = Z):
 *   accel X and Y rotate as the wheel turns.
 *   atan2(ax, ay) gives the absolute angle within [-180°, 180°].
 *
 * NOTE: This only provides a reference within ONE revolution.
 * It is used only for slow drift correction, not as the primary source.
 *
 * Returns angle in degrees, or NAN if the accelerometer vector is too
 * small to be reliable (e.g., strong vibration).
 */
static float accel_reference_angle_deg(float ax, float ay)
{
    float mag = sqrtf(ax * ax + ay * ay);
 
    if (mag < 0.1f) {   /* ~0.1 g minimum — unreliable */
        return NAN;
    }
 
    return atan2f(ax, ay) * (180.0f / (float) M_PI);
}
 
/**
 * Shortest angular difference between two angles (within ±180°).
 * Used to compute the correction delta without wrap-around artifacts.
 */
static float angle_diff_deg(float target, float current)
{
    float d = fmodf(target - current, 360.0f);
 
    if (d >  180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
 
    return d;
}


/* == Entry point ========================================================== */
int main(void)
{
    int err;
    char out_str[64];
	struct sensor_value odr_attr;
	const struct device *const lsm6dsl_dev = DEVICE_DT_GET_ONE(st_lsm6dsl);
    int sample_count = 0;

	double ax_g = 0;
	double ay_g = 0;
	double az_g = 0;

	double gx_rs = 0;
	double gy_rs = 0;
	double gz_rs = 0;

	LOG_INF("IMU with Bluetooth output\n");

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
		LOG_INF("sensor: device not ready.\n");
		return 0;
	}

    /* == NUS callback registration ============ */
	err = bt_nus_cb_register(&nus_listener, NULL);
	if (err) {
		LOG_INF("Failed to register NUS callback: %d\n", err);
		return err;
	}

    /* == Bluetooth init =================================================== */
	err = bt_enable(NULL);
	if (err) {
		LOG_INF("Failed to enable bluetooth: %d\n", err);
		return err;
	}

    /* == Advertise so a NUS central can connect =========================== */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_INF("Failed to start advertising: %d\n", err);
		return err;
	}

    /* == Start scanning simultaneously ==================================== */
    err = bt_le_scan_start(&scan_params, scan_cb);
    if (err) {
        LOG_INF("Failed to start scanning: %d\n", err);
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
		LOG_INF("Cannot set sampling frequency for accelerometer.\n");
		return 0;
	}

	if (sensor_attr_set(lsm6dsl_dev, SENSOR_CHAN_GYRO_XYZ,
			    SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr) < 0) {
		LOG_INF("Cannot set sampling frequency for gyro.\n");
		return 0;
	}

     
    LOG_INF("Steering wheel IMU tracker started");
    LOG_INF("Saturation limit: ±%.1f degrees", (double)MAX_STEERING_ANGLE_DEG);


#ifdef CONFIG_LSM6DSL_TRIGGER
	struct sensor_trigger trig;

	trig.type = SENSOR_TRIG_DATA_READY;
	trig.chan = SENSOR_CHAN_ACCEL_XYZ;

	if (sensor_trigger_set(lsm6dsl_dev, &trig, lsm6dsl_trigger_handler) != 0) {
		LOG_INF("Could not set sensor type and channel\n");
		return 0;
	}
#endif

	if (sensor_sample_fetch(lsm6dsl_dev) < 0) {
		LOG_INF("Sensor sample update error\n");
		return 0;
	}

	// Reset LEDs
	gpio_pin_set_dt(&led_red, 0);
	gpio_pin_set_dt(&led_green, 0);
	gpio_pin_set_dt(&led_blue, 0);

	LOG_INF("Initialization complete\n");

    const float dt_s = SAMPLE_INTERVAL_MS / 1000.0f;

	while (1) {
        int64_t t_start = k_uptime_get();


		// Read accelerometer headings
		ax_g = sensor_value_to_double(&accel_x_out) / 9.81f;
		ay_g = sensor_value_to_double(&accel_y_out) / 9.81f;
		az_g = sensor_value_to_double(&accel_z_out) / 9.81f;

        // Read gyroscope headings
        gx_rs = sensor_value_to_double(&gyro_x_out);
        gy_rs = sensor_value_to_double(&gyro_y_out);
        gz_rs = sensor_value_to_double(&gyro_z_out);


        /* Convert gyro Z to degrees/s */
        float gz_ds = gz_rs * (180.0f / (float)M_PI) * GYRO_Z_SIGN;
 
        /* ── Low-pass filter on gyro Z ─────────────────────── */
        g_gyro_z_filtered = GYRO_LPF_ALPHA * gz_ds
                          + (1.0f - GYRO_LPF_ALPHA) * g_gyro_z_filtered;
 
        /* ── Dead-band: suppress integration noise at rest ──── */
        float gz_effective = (fabsf(g_gyro_z_filtered) > GYRO_DEADBAND_DEG_S)
                             ? g_gyro_z_filtered : 0.0f;
 
        /* ── Integrate gyro Z → cumulative angle ───────────── */
        g_cumulative_angle_deg += gz_effective * dt_s;


        /* ── Accelerometer drift correction ────────────────── */
        /*
         * Use the accel-derived within-revolution angle to nudge
         * the cumulative angle toward its true value.
         * Only the fractional-revolution component is corrected.
         */
        float accel_abs_deg = accel_reference_angle_deg(ax_g, ay_g);
        if (!isnan(accel_abs_deg)) {
            /* Current fractional angle (within one revolution) */
            float frac_deg = fmodf(g_cumulative_angle_deg, 360.0f);
 
            /* Shortest-path correction */
            float correction = angle_diff_deg(accel_abs_deg, frac_deg);
 
            /* Apply a small correction each sample (complementary filter) */
            g_cumulative_angle_deg += DRIFT_CORRECTION_ALPHA * correction;
        }
 
        /* ── Saturate output ───────────────────────────────── */
        g_steering_angle_deg = clampf(g_cumulative_angle_deg,
                                      -MAX_STEERING_ANGLE_DEG,
                                       MAX_STEERING_ANGLE_DEG);
 
        /* ── Periodic logging ──────────────────────────────── */
        if (++sample_count >= LOG_EVERY_N_SAMPLES) {
            sample_count = 0;
            LOG_INF("Steering angle: %8.2f deg  (raw: %8.2f)  gz: %6.2f deg/s",
                    (double)g_steering_angle_deg,
                    (double)g_cumulative_angle_deg,
                    (double)gz_ds);
        }


		// // Convert accels to heading
		// current_head_yz = atan(current_accel_y / sqrt(current_accel_x*current_accel_x + current_accel_z*current_accel_z));
		// current_head_xz = atan(current_accel_x / sqrt(current_accel_y*current_accel_y + current_accel_z*current_accel_z));
		
		// // Print current heading
		// sprintf(out_str, "Current heading: %f",
		// 				current_head_yz);
		// LOG_INF("%s\n", out_str);

		// // Change LED Based on Y axis
		// if (current_head_xz > MAX_Y_HEAD_THRESHOLD) {
		// 	gpio_pin_set_dt(&led_green, 1);
		// } else if (current_head_xz < MIN_Y_HEAD_THRESHOLD) {
		// 	gpio_pin_set_dt(&led_red, 1);
		// } else {
		// 	gpio_pin_set_dt(&led_green, 0);
		// 	gpio_pin_set_dt(&led_red, 0);
		// }

		// Looping stuff
		print_samples = 1;
		k_sleep(K_MSEC(10));
	}
}
