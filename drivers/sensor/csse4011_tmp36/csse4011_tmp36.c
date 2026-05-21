#define DT_DRV_COMPAT csse4011_tmp36

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

/* Reads all 4 io-channels from DT, selects one via 'channel' property */
#define TMP36_ADC_CHANNEL_COUNT 4

struct tmp36_config {
    const struct adc_dt_spec adc_channels[TMP36_ADC_CHANNEL_COUNT];
    uint8_t channel_index; /* which of the 4 to use, from DT 'channel' */
};

struct tmp36_data {
    atomic_t raw_value; /* stores last ADC raw reading */
    int16_t sample_buf; /* DMA-safe buffer for adc_read() */
};


static int tmp36_init(const struct device *dev)
{
    const struct tmp36_config *cfg = dev->config;
    const struct adc_dt_spec *spec = &cfg->adc_channels[cfg->channel_index];

    if (!adc_is_ready_dt(spec)) {
        return -ENODEV;
    }

    return adc_channel_setup_dt(spec);
}

static int tmp36_sample_fetch(const struct device *dev,
                               enum sensor_channel chan)
{
    const struct tmp36_config *cfg = dev->config;
    struct tmp36_data *data = dev->data;

    if (chan != SENSOR_CHAN_AMBIENT_TEMP && chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    /* Select the ADC channel specified by the DT 'channel' property */
    const struct adc_dt_spec *spec = &cfg->adc_channels[cfg->channel_index];

    /* Build a sequence reading a single sample into our buffer */
    struct adc_sequence sequence = {
        .buffer      = &data->sample_buf,
        .buffer_size = sizeof(data->sample_buf),
    };

    /* Populate channel_id, gain, reference, resolution from DT spec */
    int ret = adc_sequence_init_dt(spec, &sequence);
    if (ret < 0) {
        return ret;
    }

    ret = adc_read_dt(spec, &sequence);
    if (ret < 0) {
        return ret;
    }

    /* Store raw result atomically so other threads can safely read it */
    atomic_set(&data->raw_value, (atomic_val_t)data->sample_buf);

    return 0;
}

static int tmp36_channel_get(const struct device *dev,
                              enum sensor_channel chan,
                              struct sensor_value *val)
{
    const struct tmp36_config *cfg = dev->config;
    struct tmp36_data *data = dev->data;
    const struct adc_dt_spec *spec = &cfg->adc_channels[cfg->channel_index];
    int32_t raw = (int32_t)atomic_get(&data->raw_value);
    int32_t mv = raw;

    int ret = adc_raw_to_millivolts_dt(spec, &mv);
    if (ret < 0) {
        return ret;
    }

    if (chan == SENSOR_CHAN_VOLTAGE) {
        /* Return raw millivolts */
        val->val1 = mv / 1000;           /* whole volts */
        val->val2 = (mv % 1000) * 1000;  /* microvolts remainder */
        return 0;
    } else if (chan == SENSOR_CHAN_AMBIENT_TEMP) {
        int32_t temp_mdegc = (mv - 500) * 10;
        val->val1 = temp_mdegc / 1000;
        val->val2 = (temp_mdegc % 1000) * 1000;
        return 0;
    }

    return -ENOTSUP;
}

/* Define the sensor driver API — must appear BEFORE TMP36_INIT / DT_INST_FOREACH_STATUS_OKAY */
static const struct sensor_driver_api tmp36_api = {
    .sample_fetch = tmp36_sample_fetch,
    .channel_get  = tmp36_channel_get,
};

#define TMP36_INIT(inst)                                                        \
    static struct tmp36_data tmp36_data_##inst;                                 \
    static const struct tmp36_config tmp36_config_##inst = {                   \
        .adc_channels = {                                                       \
            ADC_DT_SPEC_INST_GET_BY_IDX(inst, 0),                              \
            ADC_DT_SPEC_INST_GET_BY_IDX(inst, 1),                              \
            ADC_DT_SPEC_INST_GET_BY_IDX(inst, 2),                              \
            ADC_DT_SPEC_INST_GET_BY_IDX(inst, 3),                              \
        },                                                                      \
        .channel_index = DT_INST_PROP(inst, channel),                          \
    };                                                                          \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, tmp36_init, NULL,                       \
        &tmp36_data_##inst, &tmp36_config_##inst,                              \
        POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,                              \
        &tmp36_api);

DT_INST_FOREACH_STATUS_OKAY(TMP36_INIT)