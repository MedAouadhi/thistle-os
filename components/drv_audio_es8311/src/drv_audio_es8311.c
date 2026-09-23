// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — ES8311 audio codec driver
//
// A wrapper around Espressif's esp_codec_dev component, which carries the
// codec's register initialisation, rather than a port of it — the same
// preference for a maintained vendor library as the radio and modem drivers.
//
// What the wrapper adds is the speaker amplifier. On the T-Deck Max its enable
// line is an IO expander pin with a pull-up behind it (schematic page 8,
// NS4150B CTRL), so it reads "on" until something drives it, and an amplifier
// left on hisses and draws current at idle. The driver owns that line: it
// drives it off at init, raises it around playback, and drops it on stop or
// once playback has gone quiet for AMP_IDLE_MS.

#include "drv_audio_es8311.h"

#ifdef ES8311_HOST_TEST
#include "es8311_test_shim.h"
#else
#include "thistle_expander.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#endif

static const char *TAG = "es8311";

// How long playback may go quiet before the amplifier is dropped. Long enough
// to ride out the gap between two buffers of a stream and to let the last one
// drain from the I2S DMA; short enough that nobody hears the hiss.
#define AMP_IDLE_MS 500

// The format a codec opens with until someone configures another.
#define DEFAULT_SAMPLE_RATE 44100

// LilyGo's ES8311 examples play at 50; the codec's full scale is loud on this
// board's speaker.
#define DEFAULT_VOLUME 50

// ---------------------------------------------------------------------------
// Hardware port: the codec, behind the six calls the driver makes of it.
// Replaced by recording fakes under ES8311_HOST_TEST.
// ---------------------------------------------------------------------------

#ifndef ES8311_HOST_TEST
static struct {
    i2s_chan_handle_t             tx;
    const audio_codec_data_if_t  *data_if;
    const audio_codec_ctrl_if_t  *ctrl_if;
    const audio_codec_if_t       *codec_if;
    esp_codec_dev_handle_t        dev;
} s_port;

static void port_codec_destroy(void)
{
    if (s_port.dev)      esp_codec_dev_delete(s_port.dev);
    if (s_port.codec_if) audio_codec_delete_codec_if(s_port.codec_if);
    if (s_port.ctrl_if)  audio_codec_delete_ctrl_if(s_port.ctrl_if);
    if (s_port.data_if)  audio_codec_delete_data_if(s_port.data_if);
    if (s_port.tx)       i2s_del_channel(s_port.tx);
    memset(&s_port, 0, sizeof(s_port));
}

static esp_err_t port_codec_create(const audio_es8311_config_t *cfg)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(cfg->i2s_num, I2S_ROLE_MASTER);
    // An underrun then plays silence rather than looping the last buffer.
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_port.tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // esp_codec_dev only reconfigures the channel's clock and slots when it
    // opens, so the channel must already be in standard mode. MCLK is left at
    // the default 256 x fs, which is also the codec's default divider.
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = cfg->pin_mclk,
            .bclk = cfg->pin_bclk,
            .ws   = cfg->pin_ws,
            .dout = cfg->pin_dout,
            .din  = I2S_GPIO_UNUSED,  // playback only
        },
    };
    ret = i2s_channel_init_std_mode(s_port.tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        port_codec_destroy();
        return ret;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .port = (uint8_t)cfg->i2s_num, .tx_handle = s_port.tx };
    s_port.data_if = audio_codec_new_i2s_data(&i2s_cfg);

    // The library takes the 8-bit form of the address and shifts it back down
    // for the i2c_master driver.
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = (uint8_t)(cfg->i2c_addr << 1),
        .bus_handle = cfg->i2c_bus,
    };
    s_port.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_port.data_if || !s_port.ctrl_if) {
        ESP_LOGE(TAG, "codec interfaces could not be created");
        port_codec_destroy();
        return ESP_FAIL;
    }

    // No PA pin: the amplifier sits on an expander the library cannot reach,
    // and the driver gates it itself. hw_gain is left zeroed, which the
    // library reads as its 5.0 V / 3.3 V defaults — the same as LilyGo's
    // EspCodec wrapper, whose PA voltage is never set in their examples.
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = s_port.ctrl_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,
        .use_mclk   = true,
    };
    s_port.codec_if = es8311_codec_new(&es_cfg);
    if (!s_port.codec_if) {
        ESP_LOGE(TAG, "no ES8311 at 0x%02x", cfg->i2c_addr);
        port_codec_destroy();
        return ESP_ERR_NOT_FOUND;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_port.codec_if,
        .data_if  = s_port.data_if,
    };
    s_port.dev = esp_codec_dev_new(&dev_cfg);
    if (!s_port.dev) {
        port_codec_destroy();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t port_codec_open(const hal_audio_config_t *fs)
{
    esp_codec_dev_sample_info_t info = {
        .bits_per_sample = fs->bits_per_sample,
        .channel         = fs->channels,
        .sample_rate     = fs->sample_rate,
    };
    int ret = esp_codec_dev_open(s_port.dev, &info);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static void port_codec_close(void)
{
    esp_codec_dev_close(s_port.dev);
}

static esp_err_t port_codec_write(const uint8_t *data, size_t len)
{
    // The write takes a mutable buffer because a codec without hardware volume
    // is scaled in place. The ES8311 has hardware volume, so the caller's
    // buffer is only read.
    int ret = esp_codec_dev_write(s_port.dev, (void *)data, (int)len);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t port_codec_set_volume(uint8_t percent)
{
    int ret = esp_codec_dev_set_out_vol(s_port.dev, percent);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
#endif

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

static struct {
    audio_es8311_config_t cfg;
    hal_audio_config_t    fs;
    uint8_t               volume;
    bool                  initialized;
    bool                  open;
    bool                  amp_on;
    // Created by the first init and never deleted: esp_timer_stop does not
    // wait for a callback already on its way, so a timeout can still arrive
    // after deinit and must find both of these alive.
    SemaphoreHandle_t     lock;
    esp_timer_handle_t    idle_timer;
} s_audio;

static esp_err_t amp_set(bool on)
{
    if (s_audio.cfg.amp_expander < 0) {
        s_audio.amp_on = on;
        return ESP_OK;
    }
    esp_err_t ret = thistle_expander_set_level(s_audio.cfg.amp_expander,
                                               s_audio.cfg.amp_expander_pin, on);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "amplifier %s failed: %s", on ? "on" : "off", esp_err_to_name(ret));
        return ret;
    }
    s_audio.amp_on = on;
    return ESP_OK;
}

// Silence the board: the amplifier goes first, so the speaker never hears the
// codec's output stage being shut down. Called with the lock held.
static void go_quiet(void)
{
    if (s_audio.amp_on) amp_set(false);
    if (s_audio.open) {
        port_codec_close();
        s_audio.open = false;
    }
}

// (Re)start the idle countdown. esp_timer refuses to start a timer that is
// already armed, so it is stopped first.
static void arm_idle_timer(void)
{
    esp_timer_stop(s_audio.idle_timer);
    esp_timer_start_once(s_audio.idle_timer, (uint64_t)AMP_IDLE_MS * 1000);
}

// Runs in the esp_timer task, which must not block, so the lock is only ever
// tried. A driver that is busy — playing, or changing volume, which does not
// re-arm the timer — is tried again a timeout later.
static void idle_timeout(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_audio.lock, 0) != pdTRUE) {
        arm_idle_timer();
        return;
    }
    if (s_audio.initialized) go_quiet();
    xSemaphoreGive(s_audio.lock);
}

// ---------------------------------------------------------------------------
// vtable implementations
// ---------------------------------------------------------------------------

static void es8311_deinit(void);

static esp_err_t es8311_init(const void *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_audio.initialized) return ESP_ERR_INVALID_STATE;

    memcpy(&s_audio.cfg, config, sizeof(s_audio.cfg));
    s_audio.fs = (hal_audio_config_t){
        .sample_rate = DEFAULT_SAMPLE_RATE, .bits_per_sample = 16, .channels = 2,
    };
    s_audio.volume = DEFAULT_VOLUME;
    s_audio.open = false;

    // Claim the amplifier line before anything else: until it is driven it
    // reads "on", and the codec bring-up below is audible through it.
    esp_err_t ret = amp_set(false);
    if (ret != ESP_OK) return ret;

    if (!s_audio.lock) {
        s_audio.lock = xSemaphoreCreateMutex();
        if (!s_audio.lock) return ESP_ERR_NO_MEM;
    }
    if (!s_audio.idle_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = idle_timeout,
            .name     = "es8311_idle",
        };
        ret = esp_timer_create(&timer_args, &s_audio.idle_timer);
        if (ret != ESP_OK) return ret;
    }

    ret = port_codec_create(&s_audio.cfg);
    if (ret != ESP_OK) return ret;

    s_audio.initialized = true;
    ESP_LOGI(TAG, "initialized on I2S%d (MCLK=%d BCLK=%d WS=%d DOUT=%d) amp=%d/%d",
             s_audio.cfg.i2s_num, s_audio.cfg.pin_mclk, s_audio.cfg.pin_bclk,
             s_audio.cfg.pin_ws, s_audio.cfg.pin_dout,
             (int)s_audio.cfg.amp_expander, (int)s_audio.cfg.amp_expander_pin);
    return ESP_OK;
}

static void es8311_deinit(void)
{
    if (!s_audio.initialized) return;

    esp_timer_stop(s_audio.idle_timer);
    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    go_quiet();
    port_codec_destroy();
    s_audio.initialized = false;
    xSemaphoreGive(s_audio.lock);
}

static esp_err_t es8311_play(const uint8_t *data, size_t len)
{
    if (!s_audio.initialized) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    esp_timer_stop(s_audio.idle_timer);

    esp_err_t ret = ESP_OK;
    if (!s_audio.open) {
        ret = port_codec_open(&s_audio.fs);
        if (ret == ESP_OK) {
            s_audio.open = true;
            ret = port_codec_set_volume(s_audio.volume);
        }
    }
    // Only once the codec is running, so its start-up is not amplified.
    if (ret == ESP_OK && !s_audio.amp_on) ret = amp_set(true);
    if (ret == ESP_OK) ret = port_codec_write(data, len);

    arm_idle_timer();
    xSemaphoreGive(s_audio.lock);
    return ret;
}

static esp_err_t es8311_stop(void)
{
    if (!s_audio.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    esp_timer_stop(s_audio.idle_timer);
    go_quiet();
    xSemaphoreGive(s_audio.lock);
    return ESP_OK;
}

static esp_err_t es8311_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (!s_audio.initialized) {
        s_audio.volume = percent;
        return ESP_OK;
    }

    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    s_audio.volume = percent;
    esp_err_t ret = s_audio.open ? port_codec_set_volume(percent) : ESP_OK;
    xSemaphoreGive(s_audio.lock);
    return ret;
}

static esp_err_t es8311_configure(const hal_audio_config_t *cfg)
{
    if (!cfg || cfg->sample_rate == 0) return ESP_ERR_INVALID_ARG;
    switch (cfg->bits_per_sample) {
        case 16: case 24: case 32: break;
        default: return ESP_ERR_INVALID_ARG;
    }
    if (cfg->channels != 1 && cfg->channels != 2) return ESP_ERR_INVALID_ARG;
    if (!s_audio.initialized) return ESP_ERR_INVALID_STATE;

    // The codec is reopened at the new format by the next play.
    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    esp_timer_stop(s_audio.idle_timer);
    go_quiet();
    s_audio.fs = *cfg;
    xSemaphoreGive(s_audio.lock);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// vtable + get
// ---------------------------------------------------------------------------

static const hal_audio_driver_t s_vtable = {
    .init       = es8311_init,
    .deinit     = es8311_deinit,
    .play       = es8311_play,
    .stop       = es8311_stop,
    .set_volume = es8311_set_volume,
    .configure  = es8311_configure,
    .name       = "ES8311",
};

const hal_audio_driver_t *drv_audio_es8311_get(void)
{
    return &s_vtable;
}
