/*
 * Copyright (c) 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_adc

#include <errno.h>
#include <hal/adc_hal.h>
#include <hal/adc_oneshot_hal.h>
#include <hal/adc_types.h>
#include <soc/adc_periph.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_clk_tree.h>
#include <esp_private/periph_ctrl.h>
#include <esp_private/sar_periph_ctrl.h>
#include <esp_private/adc_share_hw_ctrl.h>

#if defined(CONFIG_ADC_ESP32_DMA)
#if !SOC_GDMA_SUPPORTED
#error "SoCs without GDMA peripheral are not supported!"
#endif
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_esp32.h>
#endif

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/clock_control.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(adc_esp32, CONFIG_ADC_LOG_LEVEL);

#define ADC_RESOLUTION_MIN SOC_ADC_DIGI_MIN_BITWIDTH
#define ADC_RESOLUTION_MAX SOC_ADC_DIGI_MAX_BITWIDTH

#define VALID_RESOLUTION(r) ((r) >= ADC_RESOLUTION_MIN && (r) <= ADC_RESOLUTION_MAX)

/* Default internal reference voltage */
#define ADC_ESP32_DEFAULT_VREF_INTERNAL (1100)

#define ADC_DMA_BUFFER_SIZE DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED

#define ADC_CLIP_MVOLT_12DB	2550

struct adc_esp32_conf {
	const struct device *clock_dev;
	const clock_control_subsys_t clock_subsys;
	adc_unit_t unit;
	uint8_t channel_count;
	const struct device *gpio_port;
#if defined(CONFIG_ADC_ESP32_DMA)
	const struct device *dma_dev;
	uint8_t dma_channel;
#endif /* defined(CONFIG_ADC_ESP32_DMA) */
};

struct adc_esp32_data {
	adc_oneshot_hal_ctx_t hal;
	adc_atten_t attenuation[SOC_ADC_MAX_CHANNEL_NUM];
	uint8_t resolution[SOC_ADC_MAX_CHANNEL_NUM];
	adc_cali_handle_t cal_handle[SOC_ADC_MAX_CHANNEL_NUM];
	uint16_t meas_ref_internal;
	uint16_t *buffer;
#if defined(CONFIG_ADC_ESP32_DMA)
	adc_hal_dma_ctx_t adc_hal_dma_ctx;
	uint8_t *dma_buffer;
	struct k_sem dma_conv_wait_lock;
#endif /* defined(CONFIG_ADC_ESP32_DMA) */
};

/* Convert zephyr,gain property to the ESP32 attenuation */
static inline int gain_to_atten(enum adc_gain gain, adc_atten_t *atten)
{
	switch (gain) {
	case ADC_GAIN_1:
		*atten = ADC_ATTEN_DB_0;
		break;
	case ADC_GAIN_4_5:
		*atten = ADC_ATTEN_DB_2_5;
		break;
	case ADC_GAIN_1_2:
		*atten = ADC_ATTEN_DB_6;
		break;
	case ADC_GAIN_1_4:
		*atten = ADC_ATTEN_DB_12;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

#if !defined(CONFIG_ADC_ESP32_DMA)

/* Convert voltage by inverted attenuation to support zephyr gain values */
static void atten_to_gain(adc_atten_t atten, uint32_t *val_mv)
{
	uint32_t num, den;

	if (!val_mv) {
		return;
	}

	switch (atten) {
	case ADC_ATTEN_DB_2_5: /* 1/ADC_GAIN_4_5 */
		num = 4;
		den = 5;
		break;
	case ADC_ATTEN_DB_6: /* 1/ADC_GAIN_1_2 */
		num = 1;
		den = 2;
		break;
	case ADC_ATTEN_DB_12: /* 1/ADC_GAIN_1_4 */
		num = 1;
		den = 4;
		break;
	case ADC_ATTEN_DB_0: /* 1/ADC_GAIN_1 */
	default:
		num = 1;
		den = 1;
		break;
	}

	*val_mv = (*val_mv * num) / den;
}

#endif /* !defined(CONFIG_ADC_ESP32_DMA) */

static void adc_hw_calibration(adc_unit_t unit)
{
#if SOC_ADC_CALIBRATION_V1_SUPPORTED
	adc_hal_calibration_init(unit);
	for (int j = 0; j < SOC_ADC_ATTEN_NUM; j++) {
		adc_calc_hw_calibration_code(unit, j);
#if SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED
		/* Load the channel compensation from efuse */
		for (int k = 0; k < SOC_ADC_CHANNEL_NUM(unit); k++) {
			adc_load_hw_calibration_chan_compens(unit, k, j);
		}
#endif /* SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED */
	}
#endif /* SOC_ADC_CALIBRATION_V1_SUPPORTED */
}

#if defined(CONFIG_ADC_ESP32_DMA)

static void IRAM_ATTR adc_esp32_dma_conv_done(const struct device *dma_dev, void *user_data,
					      uint32_t channel, int status)
{
	ARG_UNUSED(dma_dev);
	ARG_UNUSED(status);

	const struct device *dev = user_data;
	struct adc_esp32_data *data = dev->data;

	k_sem_give(&data->dma_conv_wait_lock);
}

static int adc_esp32_dma_start(const struct device *dev, uint8_t *buf, size_t len)
{
	const struct adc_esp32_conf *conf = dev->config;

	int err = 0;
	struct dma_config dma_cfg = {0};
	struct dma_status dma_status = {0};
	struct dma_block_config dma_blk = {0};

	err = dma_get_status(conf->dma_dev, conf->dma_channel, &dma_status);
	if (err) {
		LOG_ERR("Unable to get dma channel[%u] status (%d)",
			(unsigned int)conf->dma_channel, err);
		return -EINVAL;
	}

	if (dma_status.busy) {
		LOG_ERR("dma channel[%u] is busy!", (unsigned int)conf->dma_channel);
		return -EBUSY;
	}

	unsigned int key = irq_lock();

	dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	dma_cfg.dma_callback = adc_esp32_dma_conv_done;
	dma_cfg.user_data = (void *)dev;
	dma_cfg.dma_slot = ESP_GDMA_TRIG_PERIPH_ADC0;
	dma_cfg.block_count = 1;
	dma_cfg.head_block = &dma_blk;
	dma_blk.block_size = len;
	dma_blk.dest_address = (uint32_t)buf;

	err = dma_config(conf->dma_dev, conf->dma_channel, &dma_cfg);
	if (err) {
		LOG_ERR("Error configuring dma (%d)", err);
		goto unlock;
	}

	err = dma_start(conf->dma_dev, conf->dma_channel);
	if (err) {
		LOG_ERR("Error starting dma (%d)", err);
		goto unlock;
	}

unlock:
	irq_unlock(key);
	return err;
}

static int adc_esp32_dma_stop(const struct device *dev)
{
	const struct adc_esp32_conf *conf = dev->config;
	unsigned int key = irq_lock();
	int err = 0;

	err = dma_stop(conf->dma_dev, conf->dma_channel);
	if (err) {
		LOG_ERR("Error stopping dma (%d)", err);
	}

	irq_unlock(key);
	return err;
}

static int adc_esp32_fill_digi_pattern(const struct device *dev, const struct adc_sequence *seq,
				       void *pattern_config, uint32_t *pattern_len,
				       uint32_t *unit_attenuation)
{
	const struct adc_esp32_conf *conf = dev->config;
	struct adc_esp32_data *data = dev->data;

	adc_digi_pattern_config_t *adc_digi_pattern_config =
		(adc_digi_pattern_config_t *)pattern_config;
	const uint32_t unit_atten_uninit = 999;
	uint32_t channel_mask = 1, channels_copy = seq->channels;

	*pattern_len = 0;
	*unit_attenuation = unit_atten_uninit;
	for (uint8_t channel_id = 0; channel_id < conf->channel_count; channel_id++) {
		if (channels_copy & channel_mask) {
			if (*unit_attenuation == unit_atten_uninit) {
				*unit_attenuation = data->attenuation[channel_id];
			} else if (*unit_attenuation != data->attenuation[channel_id]) {
				LOG_ERR("Channel[%u] attenuation different of unit[%u] attenuation",
					(unsigned int)channel_id, (unsigned int)conf->unit);
				return -EINVAL;
			}

			adc_digi_pattern_config->atten = data->attenuation[channel_id];
			adc_digi_pattern_config->channel = channel_id;
			adc_digi_pattern_config->unit = conf->unit;
			adc_digi_pattern_config->bit_width = seq->resolution;
			adc_digi_pattern_config++;

			*pattern_len += 1;
			if (*pattern_len > SOC_ADC_PATT_LEN_MAX) {
				LOG_ERR("Max pattern len is %d", SOC_ADC_PATT_LEN_MAX);
				return -EINVAL;
			}

			channels_copy &= ~channel_mask;
			if (!channels_copy) {
				break;
			}
		}
		channel_mask <<= 1;
	}

	return 0;
}

static void adc_esp32_digi_start(const struct device *dev, void *pattern_config,
				 uint32_t pattern_len, uint32_t number_of_samplings,
				 uint32_t sample_freq_hz, uint32_t unit_attenuation)
{
	const struct adc_esp32_conf *conf = dev->config;
	struct adc_esp32_data *data = dev->data;

	sar_periph_ctrl_adc_continuous_power_acquire();
	adc_lock_acquire(conf->unit);

#if SOC_ADC_CALIBRATION_V1_SUPPORTED
	adc_set_hw_calibration_code(conf->unit, unit_attenuation);
#endif /* SOC_ADC_CALIBRATION_V1_SUPPORTED */

#if SOC_ADC_ARBITER_SUPPORTED
	if (conf->unit == ADC_UNIT_2) {
		adc_arbiter_t config = ADC_ARBITER_CONFIG_DEFAULT();

		adc_hal_arbiter_config(&config);
	}
#endif /* SOC_ADC_ARBITER_SUPPORTED */

	adc_hal_digi_ctrlr_cfg_t adc_hal_digi_ctrlr_cfg;
	soc_module_clk_t clk_src = ADC_DIGI_CLK_SRC_DEFAULT;
	uint32_t clk_src_freq_hz = 0;

	esp_clk_tree_src_get_freq_hz(clk_src, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
				     &clk_src_freq_hz);

	adc_hal_digi_ctrlr_cfg.conv_mode =
		(conf->unit == ADC_UNIT_1) ? ADC_CONV_SINGLE_UNIT_1 : ADC_CONV_SINGLE_UNIT_2;
	adc_hal_digi_ctrlr_cfg.clk_src = clk_src;
	adc_hal_digi_ctrlr_cfg.clk_src_freq_hz = clk_src_freq_hz;
	adc_hal_digi_ctrlr_cfg.sample_freq_hz = sample_freq_hz;
	adc_hal_digi_ctrlr_cfg.adc_pattern = (adc_digi_pattern_config_t *)pattern_config;
	adc_hal_digi_ctrlr_cfg.adc_pattern_len = pattern_len;

	uint32_t number_of_adc_digi_samples = number_of_samplings * pattern_len;

	adc_hal_dma_config_t adc_hal_dma_config = {
		.dev = (void *)GDMA_LL_GET_HW(0),
		.eof_desc_num = 1,
		.eof_step = 1,
		.dma_chan = conf->dma_channel,
		.eof_num = number_of_adc_digi_samples,
	};

	adc_hal_dma_ctx_config(&data->adc_hal_dma_ctx, &adc_hal_dma_config);

	adc_hal_set_controller(conf->unit, ADC_HAL_CONTINUOUS_READ_MODE);
	adc_hal_digi_init(&data->adc_hal_dma_ctx);
	adc_hal_digi_controller_config(&data->adc_hal_dma_ctx, &adc_hal_digi_ctrlr_cfg);
	adc_hal_digi_start(&data->adc_hal_dma_ctx, data->dma_buffer);
}

static void adc_esp32_digi_stop(const struct device *dev)
{
	const struct adc_esp32_conf *conf = dev->config;
	struct adc_esp32_data *data = dev->data;

	adc_hal_digi_dis_intr(&data->adc_hal_dma_ctx, ADC_HAL_DMA_INTR_MASK);
	adc_hal_digi_clr_intr(&data->adc_hal_dma_ctx, ADC_HAL_DMA_INTR_MASK);
	adc_hal_digi_stop(&data->adc_hal_dma_ctx);
	adc_hal_digi_deinit(&data->adc_hal_dma_ctx);
	adc_lock_release(conf->unit);
	sar_periph_ctrl_adc_continuous_power_release();
}

static void adc_esp32_fill_seq_buffer(const void *seq_buffer, const void *dma_buffer,
				      uint32_t number_of_samples)
{
	uint16_t *sample = (uint16_t *)seq_buffer;
	adc_digi_output_data_t *digi_data = (adc_digi_output_data_t *)dma_buffer;

	for (uint32_t k = 0; k < number_of_samples; k++) {
		*sample++ = (uint16_t)(digi_data++)->type2.data;
	}
}

static int adc_esp32_wait_for_dma_conv_done(const struct device *dev)
{
	struct adc_esp32_data *data = dev->data;
	int err = 0;

	err = k_sem_take(&data->dma_conv_wait_lock, K_FOREVER);
	if (err) {
		LOG_ERR("Error taking dma_conv_wait_lock (%d)", err);
	}

	return err;
}

#endif /* defined(CONFIG_ADC_ESP32_DMA) */

static int adc_esp32_read(const struct device *dev, const struct adc_sequence *seq)
{
	struct adc_esp32_data *data = dev->data;

	uint8_t channel_id = find_lsb_set(seq->channels) - 1;

	if (seq->buffer_size < 2) {
		LOG_ERR("Sequence buffer space too low '%d'", seq->buffer_size);
		return -ENOMEM;
	}

#if !defined(CONFIG_ADC_ESP32_DMA)
	if (seq->channels > BIT(channel_id)) {
		LOG_ERR("Multi-channel readings not supported");
		return -ENOTSUP;
	}
#endif /* !defined(CONFIG_ADC_ESP32_DMA) */

	if (seq->options) {
		if (seq->options->extra_samplings) {
			LOG_ERR("Extra samplings not supported");
			return -ENOTSUP;
		}

#if !defined(CONFIG_ADC_ESP32_DMA)
		if (seq->options->interval_us) {
			LOG_ERR("Interval between samplings not supported");
			return -ENOTSUP;
		}
#endif /* !defined(CONFIG_ADC_ESP32_DMA) */
	}

	if (!VALID_RESOLUTION(seq->resolution)) {
		LOG_ERR("unsupported resolution (%d)", seq->resolution);
		return -ENOTSUP;
	}

	if (seq->calibrate) {
		/* TODO: Does this mean actual Vref measurement on selected GPIO ?*/
		LOG_ERR("calibration is not supported");
		return -ENOTSUP;
	}

	data->resolution[channel_id] = seq->resolution;

#if !defined(CONFIG_ADC_ESP32_DMA)

	uint32_t acq_raw;

	adc_oneshot_hal_setup(&data->hal, channel_id);

#if SOC_ADC_CALIBRATION_V1_SUPPORTED
	adc_set_hw_calibration_code(data->hal.unit, data->attenuation[channel_id]);
#endif /* SOC_ADC_CALIBRATION_V1_SUPPORTED */

	adc_oneshot_hal_convert(&data->hal, &acq_raw);

	if (data->cal_handle[channel_id]) {
		if (data->meas_ref_internal > 0) {
			uint32_t acq_mv;

			adc_cali_raw_to_voltage(data->cal_handle[channel_id], acq_raw, &acq_mv);

			LOG_DBG("ADC acquisition [unit: %u, chan: %u, acq_raw: %u, acq_mv: %u]",
				data->hal.unit, channel_id, acq_raw, acq_mv);

#if CONFIG_SOC_SERIES_ESP32
		if (data->attenuation[channel_id] == ADC_ATTEN_DB_12) {
			if (acq_mv > ADC_CLIP_MVOLT_12DB) {
				acq_mv = ADC_CLIP_MVOLT_12DB;
			}
		}
#endif /* CONFIG_SOC_SERIES_ESP32 */

			/* Fit according to selected attenuation */
			atten_to_gain(data->attenuation[channel_id], &acq_mv);
			acq_raw = acq_mv * ((1 << data->resolution[channel_id]) - 1) /
				  data->meas_ref_internal;
		} else {
			LOG_WRN("ADC reading is uncompensated");
		}
	} else {
		LOG_WRN("ADC reading is uncompensated");
	}

	/* Store result */
	data->buffer = (uint16_t *)seq->buffer;
	data->buffer[0] = acq_raw;

#else /* !defined(CONFIG_ADC_ESP32_DMA) */

	int err = 0;
	uint32_t adc_pattern_len, unit_attenuation;
	adc_digi_pattern_config_t adc_digi_pattern_config[SOC_ADC_MAX_CHANNEL_NUM];

	err = adc_esp32_fill_digi_pattern(dev, seq, &adc_digi_pattern_config, &adc_pattern_len,
					  &unit_attenuation);
	if (err || adc_pattern_len == 0) {
		return -EINVAL;
	}

	const struct adc_sequence_options *options = seq->options;
	uint32_t sample_freq_hz = SOC_ADC_SAMPLE_FREQ_THRES_HIGH, number_of_samplings = 1;

	if (options != NULL) {
		number_of_samplings = seq->buffer_size / (adc_pattern_len * sizeof(uint16_t));

		if (options->interval_us) {
			sample_freq_hz = MHZ(1) / options->interval_us;
		}
	}

	if (!number_of_samplings) {
		LOG_ERR("buffer_size insufficient to store at least one set of samples!");
		return -EINVAL;
	}

	if (sample_freq_hz < SOC_ADC_SAMPLE_FREQ_THRES_LOW ||
	    sample_freq_hz > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) {
		LOG_ERR("ADC sampling frequency out of range: %uHz", sample_freq_hz);
		return -EINVAL;
	}

	uint32_t number_of_adc_samples = number_of_samplings * adc_pattern_len;
	uint32_t number_of_adc_dma_data_bytes =
		number_of_adc_samples * SOC_ADC_DIGI_DATA_BYTES_PER_CONV;

	if (number_of_adc_dma_data_bytes > ADC_DMA_BUFFER_SIZE) {
		LOG_ERR("dma buffer size insufficient to store a complete sequence!");
		return -EINVAL;
	}

	err = adc_esp32_dma_start(dev, data->dma_buffer, number_of_adc_dma_data_bytes);
	if (err) {
		return err;
	}

	adc_esp32_digi_start(dev, &adc_digi_pattern_config, adc_pattern_len, number_of_samplings,
			     sample_freq_hz, unit_attenuation);

	err = adc_esp32_wait_for_dma_conv_done(dev);
	if (err) {
		return err;
	}

	adc_esp32_digi_stop(dev);

	err = adc_esp32_dma_stop(dev);
	if (err) {
		return err;
	}

	adc_esp32_fill_seq_buffer(seq->buffer, data->dma_buffer, number_of_adc_samples);

#endif /* !defined(CONFIG_ADC_ESP32_DMA) */

	return 0;
}

#ifdef CONFIG_ADC_ASYNC
static int adc_esp32_read_async(const struct device *dev, const struct adc_sequence *sequence,
				struct k_poll_signal *async)
{
	(void)(dev);
	(void)(sequence);
	(void)(async);

	return -ENOTSUP;
}
#endif /* CONFIG_ADC_ASYNC */

static int adc_esp32_channel_setup(const struct device *dev, const struct adc_channel_cfg *cfg)
{
	const struct adc_esp32_conf *conf = (const struct adc_esp32_conf *)dev->config;
	struct adc_esp32_data *data = (struct adc_esp32_data *)dev->data;
	adc_atten_t old_atten = data->attenuation[cfg->channel_id];

	if (cfg->channel_id >= conf->channel_count) {
		LOG_ERR("Unsupported channel id '%d'", cfg->channel_id);
		return -ENOTSUP;
	}

	if (cfg->reference != ADC_REF_INTERNAL) {
		LOG_ERR("Unsupported channel reference '%d'", cfg->reference);
		return -ENOTSUP;
	}

	if (cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		LOG_ERR("Unsupported acquisition_time '%d'", cfg->acquisition_time);
		return -ENOTSUP;
	}

	if (cfg->differential) {
		LOG_ERR("Differential channels are not supported");
		return -ENOTSUP;
	}

	if (gain_to_atten(cfg->gain, &data->attenuation[cfg->channel_id])) {
		LOG_ERR("Unsupported gain value '%d'", cfg->gain);
		return -ENOTSUP;
	}

	adc_oneshot_hal_chan_cfg_t config = {
		.atten = data->attenuation[cfg->channel_id],
		.bitwidth = data->resolution[cfg->channel_id],
	};

	adc_oneshot_hal_channel_config(&data->hal, &config, cfg->channel_id);

	if ((data->cal_handle[cfg->channel_id] == NULL) ||
	    (data->attenuation[cfg->channel_id] != old_atten)) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
		adc_cali_curve_fitting_config_t cal_config = {
			.unit_id = conf->unit,
			.chan = cfg->channel_id,
			.atten = data->attenuation[cfg->channel_id],
			.bitwidth = data->resolution[cfg->channel_id],
		};

		LOG_DBG("Curve fitting calib [unit_id: %u, chan: %u, atten: %u, bitwidth: %u]",
			conf->unit, cfg->channel_id, data->attenuation[cfg->channel_id],
			data->resolution[cfg->channel_id]);

		if (data->cal_handle[cfg->channel_id] != NULL) {
			/* delete pre-existing calib scheme */
			adc_cali_delete_scheme_curve_fitting(data->cal_handle[cfg->channel_id]);
		}

		adc_cali_create_scheme_curve_fitting(&cal_config,
						     &data->cal_handle[cfg->channel_id]);
#endif /* ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED */

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
		adc_cali_line_fitting_config_t cal_config = {
			.unit_id = conf->unit,
			.atten = data->attenuation[cfg->channel_id],
			.bitwidth = data->resolution[cfg->channel_id],
#if CONFIG_SOC_SERIES_ESP32
			.default_vref = data->meas_ref_internal
#endif
		};

		LOG_DBG("Line fitting calib [unit_id: %u, chan: %u, atten: %u, bitwidth: %u]",
			conf->unit, cfg->channel_id, data->attenuation[cfg->channel_id],
			data->resolution[cfg->channel_id]);

		if (data->cal_handle[cfg->channel_id] != NULL) {
			/* delete pre-existing calib scheme */
			adc_cali_delete_scheme_line_fitting(data->cal_handle[cfg->channel_id]);
		}

		adc_cali_create_scheme_line_fitting(&cal_config,
						    &data->cal_handle[cfg->channel_id]);
#endif /* ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED */
	}

#if defined(CONFIG_ADC_ESP32_DMA)
	if (!SOC_ADC_DIG_SUPPORTED_UNIT(conf->unit)) {
		LOG_ERR("ADC2 dma mode is no longer supported, please use ADC1!");
		return -EINVAL;
	}
#endif /* defined(CONFIG_ADC_ESP32_DMA) */

	/* GPIO config for ADC mode */

	int io_num = adc_channel_io_map[conf->unit][cfg->channel_id];

	if (io_num < 0) {
		LOG_ERR("Channel %u not supported!", cfg->channel_id);
		return -ENOTSUP;
	}

	struct gpio_dt_spec gpio = {
		.port = conf->gpio_port,
		.dt_flags = 0,
		.pin = io_num,
	};

	int err = gpio_pin_configure_dt(&gpio, GPIO_DISCONNECTED);

	if (err) {
		LOG_ERR("Error disconnecting io (%d)", io_num);
		return err;
	}

	return 0;
}

static int adc_esp32_init(const struct device *dev)
{
	struct adc_esp32_data *data = (struct adc_esp32_data *)dev->data;
	const struct adc_esp32_conf *conf = (struct adc_esp32_conf *)dev->config;
	uint32_t clock_src_hz;

#if SOC_ADC_DIG_CTRL_SUPPORTED && !SOC_ADC_RTC_CTRL_SUPPORTED
	if (!device_is_ready(conf->clock_dev)) {
		return -ENODEV;
	}

	clock_control_on(conf->clock_dev, conf->clock_subsys);
#endif

	esp_clk_tree_src_get_freq_hz(ADC_DIGI_CLK_SRC_DEFAULT,
				     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &clock_src_hz);

	adc_oneshot_hal_cfg_t config = {
		.unit = conf->unit,
		.work_mode = ADC_HAL_SINGLE_READ_MODE,
		.clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
		.clk_src_freq_hz = clock_src_hz,
	};

	adc_oneshot_hal_init(&data->hal, &config);

	sar_periph_ctrl_adc_oneshot_power_acquire();

	if (!device_is_ready(conf->gpio_port)) {
		LOG_ERR("gpio0 port not ready");
		return -ENODEV;
	}

#if defined(CONFIG_ADC_ESP32_DMA)

	if (k_sem_init(&data->dma_conv_wait_lock, 0, 1)) {
		LOG_ERR("dma_conv_wait_lock initialization failed!");
		return -EINVAL;
	}

	data->adc_hal_dma_ctx.rx_desc = k_aligned_alloc(sizeof(uint32_t), sizeof(dma_descriptor_t));
	if (!data->adc_hal_dma_ctx.rx_desc) {
		LOG_ERR("rx_desc allocation failed!");
		return -ENOMEM;
	}
	LOG_DBG("rx_desc = 0x%08X", (unsigned int)data->adc_hal_dma_ctx.rx_desc);

	data->dma_buffer = k_aligned_alloc(sizeof(uint32_t), ADC_DMA_BUFFER_SIZE);
	if (!data->dma_buffer) {
		LOG_ERR("dma buffer allocation failed!");
		k_free(data->adc_hal_dma_ctx.rx_desc);
		return -ENOMEM;
	}
	LOG_DBG("data->dma_buffer = 0x%08X", (unsigned int)data->dma_buffer);

#endif /* defined(CONFIG_ADC_ESP32_DMA) */

	for (uint8_t i = 0; i < SOC_ADC_MAX_CHANNEL_NUM; i++) {
		data->resolution[i] = ADC_RESOLUTION_MAX;
		data->attenuation[i] = ADC_ATTEN_DB_0;
		data->cal_handle[i] = NULL;
	}

	/* Default reference voltage. This could be calibrated externaly */
	data->meas_ref_internal = ADC_ESP32_DEFAULT_VREF_INTERNAL;

	adc_hw_calibration(conf->unit);

	return 0;
}

static DEVICE_API(adc, api_esp32_driver_api) = {
	.channel_setup = adc_esp32_channel_setup,
	.read = adc_esp32_read,
#ifdef CONFIG_ADC_ASYNC
	.read_async = adc_esp32_read_async,
#endif /* CONFIG_ADC_ASYNC */
	.ref_internal = ADC_ESP32_DEFAULT_VREF_INTERNAL,
};

#define ADC_ESP32_CONF_GPIO_PORT_INIT .gpio_port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),

#if defined(CONFIG_ADC_ESP32_DMA)

#define ADC_ESP32_CONF_DMA_INIT(n)                                                                 \
	.dma_dev = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas),     \
					(DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_IDX(n, 0))),           \
					(NULL)),                          \
		 .dma_channel = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas), \
					(DT_INST_DMAS_CELL_BY_IDX(n, 0, channel)),                 \
					(0xff)),
#else

#define ADC_ESP32_CONF_DMA_INIT(inst)

#endif /* defined(CONFIG_ADC_ESP32_DMA) */

#define ADC_ESP32_CHECK_CHANNEL_REF(chan)                                                          \
	BUILD_ASSERT(DT_ENUM_HAS_VALUE(chan, zephyr_reference, adc_ref_internal),                  \
		     "adc_esp32 only supports ADC_REF_INTERNAL as a reference");

#define ESP32_ADC_INIT(inst)                                                                       \
                                                                                                   \
	DT_INST_FOREACH_CHILD(inst, ADC_ESP32_CHECK_CHANNEL_REF)                                   \
                                                                                                   \
	static const struct adc_esp32_conf adc_esp32_conf_##inst = {                               \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),                             \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(inst, offset),         \
		.unit = DT_PROP(DT_DRV_INST(inst), unit) - 1,                                      \
		.channel_count = DT_PROP(DT_DRV_INST(inst), channel_count),                        \
		ADC_ESP32_CONF_GPIO_PORT_INIT ADC_ESP32_CONF_DMA_INIT(inst)};                      \
                                                                                                   \
	static struct adc_esp32_data adc_esp32_data_##inst = {                                     \
		.hal =                                                                             \
			{                                                                          \
				.dev = (adc_oneshot_soc_handle_t)DT_INST_REG_ADDR(inst),           \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, &adc_esp32_init, NULL, &adc_esp32_data_##inst,                 \
			      &adc_esp32_conf_##inst, POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,       \
			      &api_esp32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ESP32_ADC_INIT)
