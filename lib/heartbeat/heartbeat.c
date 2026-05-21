#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <lib/heartbeat.h>

LOG_MODULE_REGISTER(heartbeat, CONFIG_HEARTBEAT_LOG_LEVEL);

#if !DT_NODE_EXISTS(DT_ALIAS(heartbeat_gpio))
#error "No heartbeat gpio alias found in the device tree"
#endif

#define HEARTBEAT_NODE DT_ALIAS(heartbeat_gpio)

static const struct gpio_dt_spec hb = GPIO_DT_SPEC_GET(HEARTBEAT_NODE, gpios);

struct k_timer heartbeat_timer;
static k_timeout_t timer_duration = K_MSEC(CONFIG_HEARTBEAT_DEFAULT_DURATION);
static bool timer_initialised = false;

extern void heartbeat_trigger(struct k_timer *timer_id)
{
    if (gpio_pin_toggle_dt(&hb)) {
        LOG_ERR("Failed to toggle heartbeat");
        return;
    }

    LOG_DBG("Heartbeat");
    k_timer_start(&heartbeat_timer, timer_duration, timer_duration);
}

int heartbeat_init_start(k_timeout_t duration)
{
    int err;

    timer_duration = duration;

    if (timer_initialised) {
        goto start;
    }

    if (!gpio_is_ready_dt(&hb)) {
        LOG_ERR("Heartbeat GPIO is not ready.");
        return -ENXIO;
    }

    err = gpio_pin_configure_dt(&hb, GPIO_OUTPUT_ACTIVE);
    if (err < 0) {
        return err;
    }

    k_timer_init(&heartbeat_timer, &heartbeat_trigger, NULL);
    timer_initialised = true;

start:
    k_timer_start(&heartbeat_timer, timer_duration, timer_duration);
    return 0;
}

void heartbeat_stop(void)
{
    k_timer_stop(&heartbeat_timer);
}
