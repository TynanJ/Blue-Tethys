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
#include <zephyr/drivers/spi.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>

#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)

#define NVS_KEY_DIFFICULTY   1  /* unique ID for difficulty entry */

#define LED1_NODE DT_ALIAS(led1) //green
#define LED2_NODE DT_ALIAS(led2) //blue
#define LED0_NODE DT_ALIAS(led0) //red

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct nvs_fs nvs;

#define DIFFICULTY_MAX_LEN 4
static char g_difficulty[DIFFICULTY_MAX_LEN] = "hrd";  /* default */


#define DEVICE_NAME		CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN		(sizeof(DEVICE_NAME) - 1)
LOG_MODULE_REGISTER(rc522, LOG_LEVEL_INF);

/* Device tree bindings */
#define RC522_NODE DT_NODELABEL(rc522)

static const struct spi_dt_spec rc522_spi = SPI_DT_SPEC_GET(RC522_NODE,
    SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));

static const struct gpio_dt_spec rc522_rst =
    GPIO_DT_SPEC_GET(DT_NODELABEL(rc522_rst), gpios);

/* RC522 Register Addresses */
#define RC522_REG_COMMAND       0x01
#define RC522_REG_COM_IEN       0x02
#define RC522_REG_COM_IRQ       0x04
#define RC522_REG_ERROR         0x06
#define RC522_REG_FIFO_DATA     0x09
#define RC522_REG_FIFO_LEVEL    0x0A
#define RC522_REG_CONTROL       0x0C
#define RC522_REG_BIT_FRAMING   0x0D
#define RC522_REG_MODE          0x11
#define RC522_REG_TX_CONTROL    0x14
#define RC522_REG_TX_ASK        0x15
#define RC522_REG_CRC_RESULT_H  0x21
#define RC522_REG_CRC_RESULT_L  0x22
#define RC522_REG_VERSION       0x37

/* RC522 Commands */
#define RC522_CMD_IDLE          0x00
#define RC522_CMD_MEM           0x01
#define RC522_CMD_CALC_CRC      0x03
#define RC522_CMD_TRANSMIT      0x04
#define RC522_CMD_RECEIVE       0x08
#define RC522_CMD_TRANSCEIVE    0x0C
#define RC522_CMD_SOFT_RESET    0x0F

#define MFRC522_REG_COMM_IE_N    0x02
#define MFRC522_REG_COMM_IRQ     0x04
#define MFRC522_REG_DIV_IRQ      0x05
#define MFRC522_REG_ERROR        0x06
#define MFRC522_REG_FIFO_DATA    0x09
#define MFRC522_REG_FIFO_LEVEL   0x0A
#define MFRC522_REG_CONTROL      0x0C
#define MFRC522_REG_BIT_FRAMING  0x0D
#define MFRC522_REG_COMMAND      0x01
#define MFRC522_REG_TX_CONTROL   0x14
#define MFRC522_REG_CRC_RESULT_L 0x22
#define MFRC522_REG_CRC_RESULT_M 0x21
#define MFRC522_MAX_LEN          16

#define PCD_IDLE                 0x00
#define PCD_AUTHENT              0x0E
#define PCD_TRANSCEIVE           0x0C
#define PCD_CALCCRC              0x03

#define PICC_REQIDL              0x26
#define PICC_ANTICOLL            0x93
#define PICC_HALT                0x50


uint8_t id[5];

typedef struct {
    uint8_t uid[4];
    const char *colour;
} rfid_card_t;

static const rfid_card_t known_cards[] = {
    { {0x4d, 0x4e, 0xc0, 0x01}, "purple" },
    { {0x4e, 0xe5, 0xc0, 0x01}, "green"  },
    { {0xb2, 0x7c, 0xc7, 0x01}, "yellow" },
};

/* -----------------------------------------------------------------------
 * Low-level SPI read/write
 * --------------------------------------------------------------------- */

 static void nvs_init(void)
{
    struct flash_pages_info info;
    int err;

    nvs.flash_device = NVS_PARTITION_DEVICE;
    if (!device_is_ready(nvs.flash_device)) {
        LOG_ERR("NVS flash device not ready");
        return;
    }

    nvs.offset = NVS_PARTITION_OFFSET;
    err = flash_get_page_info_by_offs(nvs.flash_device, nvs.offset, &info);
    if (err) {
        LOG_ERR("NVS flash page info error: %d", err);
        return;
    }

    nvs.sector_size  = info.size;
    nvs.sector_count = 2;

    err = nvs_mount(&nvs);
    if (err) {
        LOG_ERR("NVS mount failed: %d", err);
        return;
    }

    LOG_INF("NVS mounted");
}

static void save_difficulty(const char *mode)
{
    int err = nvs_write(&nvs, NVS_KEY_DIFFICULTY, mode, strlen(mode) + 1);
    if (err < 0) {
        LOG_ERR("Failed to save difficulty: %d", err);
    } else {
        LOG_INF("Difficulty saved: %s", mode);
    }
}

static void load_difficulty(char *buf, size_t len)
{
    int err = nvs_read(&nvs, NVS_KEY_DIFFICULTY, buf, len);
    if (err < 0) {
        LOG_WRN("No saved difficulty, defaulting to hard");
        strncpy(buf, "hrd", len);
    } else {
        LOG_INF("Loaded difficulty: %s", buf);
    }
}

static int rc522_write_reg(uint8_t reg, uint8_t val)
{
    /* Address byte: MSB=0 (write), bits[6:1]=addr, LSB=0 */
    uint8_t tx_buf[2] = { (reg << 1) & 0x7E, val };
    struct spi_buf tx[] = {{ .buf = tx_buf, .len = 2 }};
    struct spi_buf_set tx_set = { .buffers = tx, .count = 1 };

    return spi_write_dt(&rc522_spi, &tx_set);
}

static int rc522_read_reg(uint8_t reg, uint8_t *val)
{
    /* Address byte: MSB=1 (read), bits[6:1]=addr, LSB=0 */
    uint8_t tx_buf[2] = { ((reg << 1) & 0x7E) | 0x80, 0x00 };
    uint8_t rx_buf[2] = { 0 };
    struct spi_buf tx[] = {{ .buf = tx_buf, .len = 2 }};
    struct spi_buf rx[] = {{ .buf = rx_buf, .len = 2 }};
    struct spi_buf_set tx_set = { .buffers = tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = rx, .count = 1 };

    int err = spi_transceive_dt(&rc522_spi, &tx_set, &rx_set);
    *val = rx_buf[1];
    return err;
}

static int rc522_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t val;
    int err = rc522_read_reg(reg, &val);
    if (err) return err;
    return rc522_write_reg(reg, val | mask);
}

static int rc522_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t val;
    int err = rc522_read_reg(reg, &val);
    if (err) return err;
    return rc522_write_reg(reg, val & ~mask);
}

/* -----------------------------------------------------------------------
 * Hardware reset
 * --------------------------------------------------------------------- */

static int rc522_hw_reset(void)
{
    if (!gpio_is_ready_dt(&rc522_rst)) {
        LOG_ERR("RST GPIO not ready");
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(&rc522_rst, GPIO_OUTPUT_ACTIVE);
    if (err) return err;

    gpio_pin_set_dt(&rc522_rst, 0);  /* assert reset */
    k_msleep(10);
    gpio_pin_set_dt(&rc522_rst, 1);  /* release reset */
    k_msleep(50);                     /* wait for oscillator to stabilise */

    return 0;
}

/* -----------------------------------------------------------------------
 * Full init sequence
 * --------------------------------------------------------------------- */

int rc522_init(void)
{
    if (!spi_is_ready_dt(&rc522_spi)) {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    rc522_hw_reset();

    /* Software reset */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_SOFT_RESET);
    k_msleep(50);

    /* Timer config matching working Arduino version */
    rc522_write_reg(0x2A, 0x80);
    rc522_write_reg(0x2B, 0xA9);
    rc522_write_reg(0x2C, 0xE8);
    rc522_write_reg(0x2D, 0x03);

    /* Modulation */
    rc522_write_reg(RC522_REG_TX_ASK, 0x40);
    rc522_write_reg(RC522_REG_MODE, 0x3D);

    /* Antenna on */
    rc522_set_bits(RC522_REG_TX_CONTROL, 0x03);

    /* Read version just for info, don't fail on it */
    uint8_t version;
    rc522_read_reg(RC522_REG_VERSION, &version);
    LOG_INF("RC522 version: 0x%02x", version);

    LOG_INF("RC522 initialised successfully");
    return 0;
}

/* Send REQA command and check for response */
static int rc522_detect_card(void)
{
    uint8_t buffer[2];
    uint8_t buffer_size = sizeof(buffer);

    /* Prepare for transmission */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    rc522_write_reg(RC522_REG_COM_IRQ, 0x7F);           /* clear interrupts */
    rc522_write_reg(RC522_REG_FIFO_LEVEL, 0x80);        /* flush FIFO */
    rc522_write_reg(RC522_REG_FIFO_DATA, 0x26);         /* REQA command */
    rc522_write_reg(RC522_REG_BIT_FRAMING, 0x07);       /* 7 bits */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);
    rc522_set_bits(RC522_REG_BIT_FRAMING, 0x80);        /* start transmission */

    /* Wait for response with timeout */
    uint8_t irq;
    int timeout = 50;
    do {
        rc522_read_reg(RC522_REG_COM_IRQ, &irq);
        k_msleep(1);
    } while (--timeout && !(irq & 0x30));  /* wait for RxIRq or IdleIRq */

    if (timeout == 0) {
        return -ETIMEDOUT;  /* no card */
    }

    /* Check for errors */
    uint8_t error;
    rc522_read_reg(RC522_REG_ERROR, &error);
    if (error & 0x1B) {
        return -EIO;
    }

    /* Check FIFO has data */
    uint8_t fifo_level;
    rc522_read_reg(RC522_REG_FIFO_LEVEL, &fifo_level);
    if (fifo_level == 0) {
        return -ENODATA;
    }

    return 0;  /* card detected */
}

static void rc522_calculateCRC(uint8_t *pIndata, uint8_t len, uint8_t *pOutData)
{
    uint8_t i, n;

    rc522_clear_bits(MFRC522_REG_DIV_IRQ, 0x04);
    rc522_set_bits(MFRC522_REG_FIFO_LEVEL, 0x80);

    for (i = 0; i < len; i++) {
        rc522_write_reg(MFRC522_REG_FIFO_DATA, *(pIndata + i));
    }
    rc522_write_reg(MFRC522_REG_COMMAND, PCD_CALCCRC);

    i = 0xFF;
    do {
        rc522_read_reg(MFRC522_REG_DIV_IRQ, &n);
        i--;
    } while ((i != 0) && !(n & 0x04));

    rc522_read_reg(MFRC522_REG_CRC_RESULT_L, &pOutData[0]);
    rc522_read_reg(MFRC522_REG_CRC_RESULT_M, &pOutData[1]);
}

static bool rc522_toCard(uint8_t command, uint8_t *sendData, uint8_t sendLen,
                         uint8_t *backData, uint16_t *backLen)
{
    bool status = false;
    uint8_t irqEn = 0x00;
    uint8_t waitIRq = 0x00;
    uint8_t lastBits, n;
    uint16_t i;

    switch (command) {
    case PCD_AUTHENT:
        irqEn = 0x12;
        waitIRq = 0x10;
        break;
    case PCD_TRANSCEIVE:
        irqEn = 0x77;
        waitIRq = 0x30;
        break;
    default:
        break;
    }

    rc522_write_reg(MFRC522_REG_COMM_IE_N, irqEn | 0x80);
    rc522_clear_bits(MFRC522_REG_COMM_IRQ, 0x80);
    rc522_set_bits(MFRC522_REG_FIFO_LEVEL, 0x80);
    rc522_write_reg(MFRC522_REG_COMMAND, PCD_IDLE);

    for (i = 0; i < sendLen; i++) {
        rc522_write_reg(MFRC522_REG_FIFO_DATA, sendData[i]);
    }

    rc522_write_reg(MFRC522_REG_COMMAND, command);
    if (command == PCD_TRANSCEIVE) {
        rc522_set_bits(MFRC522_REG_BIT_FRAMING, 0x80);
    }

    i = 100;
    do {
        rc522_read_reg(MFRC522_REG_COMM_IRQ, &n);
        i--;
    } while ((i != 0) && !(n & 0x01) && !(n & waitIRq));

    rc522_clear_bits(MFRC522_REG_BIT_FRAMING, 0x80);

    if (i != 0) {
        uint8_t err;
        rc522_read_reg(MFRC522_REG_ERROR, &err);
        if (!(err & 0x1B)) {
            status = true;
            if (n & irqEn & 0x01) {
                status = false;
            }

            if (command == PCD_TRANSCEIVE) {
                uint8_t fifo_level;
                rc522_read_reg(MFRC522_REG_FIFO_LEVEL, &fifo_level);
                n = fifo_level;
                lastBits = 0;
                rc522_read_reg(MFRC522_REG_CONTROL, &lastBits);
                lastBits &= 0x07;

                if (lastBits) {
                    *backLen = (n - 1) * 8 + lastBits;
                } else {
                    *backLen = n * 8;
                }

                if (n == 0) n = 1;
                if (n > MFRC522_MAX_LEN) n = MFRC522_MAX_LEN;

                for (i = 0; i < n; i++) {
                    rc522_read_reg(MFRC522_REG_FIFO_DATA, &backData[i]);
                }

                if (fifo_level == 4) {
                    LOG_INF("Card data: %02x %02x %02x %02x",
                        backData[0], backData[1], backData[2], backData[3]);
                }
                return status;
            }
        } else {
            LOG_ERR("RC522 toCard error");
            status = false;
        }
    }

    return status;
}

static bool rc522_request(uint8_t reqMode, uint8_t *tagType)
{
    uint16_t backBits;

    rc522_write_reg(MFRC522_REG_BIT_FRAMING, 0x07);
    tagType[0] = reqMode;

    bool status = rc522_toCard(PCD_TRANSCEIVE, tagType, 1, tagType, &backBits);
    if (!status || backBits != 0x10) {
        status = false;
    }
    return status;
}

static bool rc522_antiColl(uint8_t *serNum)
{
    uint16_t unLen;
    uint8_t serNumCheck = 0;

    rc522_write_reg(MFRC522_REG_BIT_FRAMING, 0x00);
    serNum[0] = PICC_ANTICOLL;
    serNum[1] = 0x20;

    bool status = rc522_toCard(PCD_TRANSCEIVE, serNum, 2, serNum, &unLen);

    if (status) {
        uint8_t i;
        for (i = 0; i < 4; i++) {
            serNumCheck ^= serNum[i];
        }
        if (serNumCheck != serNum[i]) {
            status = false;
        }
    }
    return status;
}

static void rc522_halt(void)
{
    uint16_t unLen;
    uint8_t buff[4];

    buff[0] = PICC_HALT;
    buff[1] = 0;
    rc522_calculateCRC(buff, 2, &buff[2]);
    rc522_toCard(PCD_TRANSCEIVE, buff, 4, buff, &unLen);
}

bool rc522_checkCard(uint8_t *id)
{
    bool status = rc522_request(PICC_REQIDL, id);
    if (status) {
        status = rc522_antiColl(id);
    }
    rc522_halt();
    return status;
}

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

    if (len == 3 && strncmp((const char *)data, "esy", 3) == 0) {
        strncpy(g_difficulty, "esy", sizeof(g_difficulty));
        save_difficulty("esy");
        LOG_INF("Difficulty set to easy");
        flash_colour("yellow");
        return;
    }

    if (len == 3 && strncmp((const char *)data, "med", 3) == 0) {
        strncpy(g_difficulty, "med", sizeof(g_difficulty));
        save_difficulty("med");
        LOG_INF("Difficulty set to medium");
        flash_colour("yellow");
        flash_colour("green");
        return;
    }

    if (len == 3 && strncmp((const char *)data, "hrd", 3) == 0) {
        strncpy(g_difficulty, "hrd", sizeof(g_difficulty));
        save_difficulty("hrd");
        LOG_INF("Difficulty set to hard");
        flash_colour("yellow");
        flash_colour("green");
        flash_colour("purple");
        return;
    }
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

    // if (extract_ibeacon(buf, uuid, &major, &minor)) {
    //     /* iBeacon */
    //     json_len = snprintf(json, sizeof(json),
    //         "{\"TYPE\":\"ibeacon\","
    //          "\"BLEMAC\":\"%s\","
    //          "\"BLEMajor\":%u,"
    //          "\"BLEMinor\":%u,"
    //          "\"RSSI\":%d}\r\n",
    //         addr_str, major, minor, rssi);
    // } else {
    //     // /* Generic BLE device */
    //     // json_len = snprintf(json, sizeof(json),
    //     //     "{\"TYPE\":\"ble\","
    //     //      "\"BLEMAC\":\"%s\","
    //     //      "\"BLEMajor\":0,"
    //     //      "\"BLEMinor\":0,"
    //     //      "\"RSSI\":%d}\r\n",
    //     //     addr_str, rssi);
    //     return;
    // }

    // printk("%s", json);

    // if (json_len > 0 && json_len < sizeof(json)) {
    //     int err = bt_nus_send(NULL, (uint8_t *)json, (uint16_t)json_len);
    //     if (err < 0 && err != -EAGAIN && err != -ENOTCONN) {
    //         printk("bt_nus_send failed: %d\n", err);
    //     }

    // }
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

static void flash_difficulty_led(void)
{
    if (strcmp(g_difficulty, "esy") == 0) {
        flash_colour("yellow");
    } else if (strcmp(g_difficulty, "med") == 0) {
        flash_colour("yellow");
        flash_colour("green");
    } else{
        flash_colour("yellow");
        flash_colour("green");
        flash_colour("purple");
    }
}


/* == Entry point ========================================================== */
int main(void)
{

    //  k_msleep(2000);

     int err;

	printk("RFID Scanner Peripheral\n");

    nvs_init();
    load_difficulty(g_difficulty, sizeof(g_difficulty));
    LOG_INF("Starting with difficulty: %s", g_difficulty);

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

    char json[128];

    if (rc522_init() != 0) {
        LOG_ERR("RC522 init failed, halting");
        gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_ACTIVE);
        return -1;
    }

    printk("RC522 ready, waiting for card...\n");
    flash_difficulty_led();

        while (1) {
    if (rc522_checkCard(id)) {
        const char *colour = NULL;
        for (int i = 0; i < ARRAY_SIZE(known_cards); i++) {
            if (memcmp(id, known_cards[i].uid, 4) == 0) {
                colour = known_cards[i].colour;

                int json_len = snprintf(json, sizeof(json),
                    "{\"TYPE\":\"rfid\","
                    "\"SCANNED\":\"%s\"}\r\n",
                    colour);
                printk("%s", json);
                if (json_len > 0 && json_len < sizeof(json)) {
                    err = bt_nus_send(NULL, (uint8_t *)json, (uint16_t)json_len);
                    if (err < 0 && err != -EAGAIN && err != -ENOTCONN) {
                        printk("bt_nus_send failed: %d\n", err);
                    }
                }
                break;
            }
        }

        /* Determine flash colour based on difficulty */
        const char *flash = "white";  /* default — unknown card */
        if (colour != NULL) {
            if (strcmp(g_difficulty, "esy") == 0) {
                /* easy: only yellow flashes colour */
                if (strcmp(colour, "yellow") == 0) {
                    flash = "yellow";
                } else {
                    flash = "white";
                }
            } else if (strcmp(g_difficulty, "med") == 0) {
                /* medium: yellow and green flash colour */
                if (strcmp(colour, "yellow") == 0 ||
                    strcmp(colour, "green")  == 0) {
                    flash = colour;
                } else {
                    flash = "white";
                }
            } else {
                /* hard: all cards flash their colour */
                flash = colour;
            }
        }

        flash_colour(flash);
        k_msleep(100);
    }
    k_msleep(100);
}

return 0;
}
