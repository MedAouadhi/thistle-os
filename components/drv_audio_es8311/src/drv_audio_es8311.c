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
//
// The microphone reaches the SoC through the codec's ADC, on the same I2S
// port as playback: a duplex pair sharing one bit clock and word clock, so
// both directions run at one format. Recording opens the codec but leaves the
// amplifier alone, and the idle timeout closes it once reads stop too.
//
// Each direction has a lock of its own, held across its write or read, and a
// third covers the codec's state: open, the amplifier, the format. A write
// and a read hold only their own direction while the data moves, so a player
// and a recorder in different tasks run side by side. Anything that closes
// the codec (stop, configure, deinit, the idle timeout) takes all three, in
// the order playback, capture, state, and so waits for both to finish.

#include "drv_audio_es8311.h"

#ifdef ES8311_HOST_TEST
#include "es8311_test_shim.h"
#else
#include "thistle_expander.h"
#include "thistle_power.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
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

// The codec's ADC gain, 0-42 dB in 6 dB steps, on top of the 30 dB analog
// PGA esp_codec_dev sets. The library's default is 0 dB. At 24 dB a T-Deck
// Max in a quiet room reads about -43 dBFS RMS, and its own speaker at the
// default volume about -35 dBFS, with nothing near clipping (ticket 12).
#define DEFAULT_MIC_GAIN_DB 24
#define MAX_MIC_GAIN_DB     42

// The I2S DMA ring, set explicitly (these are ESP-IDF's defaults) because the
// settling arithmetic below needs to know how much audio it holds. Frames,
// whatever the format: the frame count per buffer survives a reconfigure.
#define DMA_DESC_NUM   6
#define DMA_FRAME_NUM  240
#define RX_RING_FRAMES (DMA_DESC_NUM * DMA_FRAME_NUM)

// After the codec powers up, the microphone input swings full scale for about
// 75 ms, then carries a decaying DC offset into the noise floor by about
// 400 ms. Measured on a T-Deck Max at 8, 16, 44.1 and 48 kHz: it is constant
// in time, not in samples, as befits the RC network on the mic bias
// (schematic p8). Audio captured before then is dropped.
#define MIC_SETTLE_MS 400

// What the drop reads into: PSRAM by preference, and only while dropping.
#define DROP_CHUNK_BYTES 4096

// ---------------------------------------------------------------------------
// Hardware port: the codec, behind the eight calls the driver makes of it.
// Replaced by recording fakes under ES8311_HOST_TEST.
// ---------------------------------------------------------------------------

#ifndef ES8311_HOST_TEST
static struct {
    i2s_chan_handle_t             tx;
    i2s_chan_handle_t             rx;
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
    if (s_port.rx)       i2s_del_channel(s_port.rx);
    memset(&s_port, 0, sizeof(s_port));
}

static esp_err_t port_codec_create(const audio_es8311_config_t *cfg)
{
    const bool has_mic = cfg->pin_din >= 0;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(cfg->i2s_num, I2S_ROLE_MASTER);
    // An underrun then plays silence rather than looping the last buffer.
    chan_cfg.auto_clear    = true;
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    // A duplex pair when there is a microphone: the two channels share the
    // port's clocks, which is how the codec expects them.
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_port.tx, has_mic ? &s_port.rx : NULL);
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
            .din  = has_mic ? cfg->pin_din : I2S_GPIO_UNUSED,
        },
    };
    ret = i2s_channel_init_std_mode(s_port.tx, &std_cfg);
    if (ret == ESP_OK && has_mic) ret = i2s_channel_init_std_mode(s_port.rx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        port_codec_destroy();
        return ret;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = (uint8_t)cfg->i2s_num,
        .tx_handle = s_port.tx,
        .rx_handle = s_port.rx,
    };
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
    // The microphone is analog, on the codec's MIC1P/MIC1N (schematic p8).
    //
    // no_dac_ref: by default the library has the codec send the DAC's input
    // back on the right slot of ASDOUT (ADCDAT_SEL 5, "ADC + DACR"), as an
    // echo-cancellation reference. An app recording in stereo would then get
    // whatever was playing, bit for bit, in its right channel. Set, it writes
    // ADCDAT_SEL 0, which the ES8311 user guide (register 0x44) gives as
    // "ADC + ADC": the microphone in both slots. The library's own comment
    // says the right slot is left empty; on the T-Deck Max it carries the
    // microphone, as the user guide says.
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if    = s_port.ctrl_if,
        .codec_mode = has_mic ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1,
        .use_mclk   = true,
        .no_dac_ref = true,
    };
    s_port.codec_if = es8311_codec_new(&es_cfg);
    if (!s_port.codec_if) {
        ESP_LOGE(TAG, "no ES8311 at 0x%02x", cfg->i2c_addr);
        port_codec_destroy();
        return ESP_ERR_NOT_FOUND;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = has_mic ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT,
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

static esp_err_t port_codec_read(uint8_t *buf, size_t len)
{
    int ret = esp_codec_dev_read(s_port.dev, buf, (int)len);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t port_codec_set_volume(uint8_t percent)
{
    int ret = esp_codec_dev_set_out_vol(s_port.dev, percent);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t port_codec_set_mic_gain(uint8_t db)
{
    int ret = esp_codec_dev_set_in_gain(s_port.dev, (float)db);
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
    uint8_t               mic_gain_db;
    bool                  initialized;
    bool                  open;
    bool                  amp_on;
    // When the codec last opened, and whether a record has since dropped
    // what the microphone captured while settling.
    int64_t               opened_us;
    bool                  settled;
    // Created by the first init and never deleted: esp_timer_stop does not
    // wait for a callback already on its way, so a timeout can still arrive
    // after deinit and must find these alive.
    SemaphoreHandle_t     tx_lock;  // held across a write
    SemaphoreHandle_t     rx_lock;  // held across a read
    SemaphoreHandle_t     lock;     // the state above; never held across I/O
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

// The power manager's hold while the codec is open, or 0.
static uint32_t s_awake;

// Silence the board: the amplifier goes first, so the speaker never hears the
// codec's output stage being shut down. Called with all three locks held.
static void go_quiet(void)
{
    if (s_audio.amp_on) amp_set(false);
    if (s_audio.open) {
        port_codec_close();
        s_audio.open = false;
    }
    power_manager_release(s_awake);
    s_awake = 0;
}

static bool has_mic(void) { return s_audio.cfg.pin_din >= 0; }

// Bring the codec up at the current format and settings if it is closed.
// Called with the state lock held. The amplifier is not touched: a record
// needs the codec running but not the speaker.
static esp_err_t ensure_open(void)
{
    if (s_audio.open) return ESP_OK;
    esp_err_t ret = port_codec_open(&s_audio.fs);
    if (ret != ESP_OK) return ret;
    s_audio.open = true;
    /* Sleep would stop the I2S clocks under whatever is playing or listening. */
    if (!s_awake) s_awake = power_manager_hold("audio is in use");
    s_audio.opened_us = esp_timer_get_time();
    s_audio.settled = false;
    ret = port_codec_set_volume(s_audio.volume);
    if (ret == ESP_OK && has_mic()) ret = port_codec_set_mic_gain(s_audio.mic_gain_db);
    return ret;
}

// How many bytes of what the microphone captured before it settled are still
// to be read, the first time anyone reads since the codec opened; zero after.
// Called with the state lock held. The oldest audio still waiting is the
// start of the I2S ring, which holds RX_RING_FRAMES: everything since the
// open if that is less, otherwise only the latest. What of it falls before
// the settling point is to be dropped, and if the settling point is still to
// come, the wait for it.
static size_t settling_bytes(void)
{
    if (s_audio.settled) return 0;
    s_audio.settled = true;

    const int64_t rate    = s_audio.fs.sample_rate;
    const int64_t elapsed = (esp_timer_get_time() - s_audio.opened_us) * rate / 1000000;
    const int64_t oldest  = elapsed > RX_RING_FRAMES ? elapsed - RX_RING_FRAMES : 0;
    const int64_t settle  = rate * MIC_SETTLE_MS / 1000;
    if (oldest >= settle) return 0;

    const size_t frame_bytes = (size_t)s_audio.fs.channels * (s_audio.fs.bits_per_sample / 8);
    return (size_t)(settle - oldest) * frame_bytes;
}

// Read and throw away `remaining` bytes. Called with the capture lock held.
static esp_err_t drop_bytes(size_t remaining)
{
    const size_t frame_bytes = (size_t)s_audio.fs.channels * (s_audio.fs.bits_per_sample / 8);
    uint8_t *chunk = heap_caps_malloc_prefer(DROP_CHUNK_BYTES, 2,
                                             MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (!chunk) return ESP_ERR_NO_MEM;
    const size_t step = DROP_CHUNK_BYTES - DROP_CHUNK_BYTES % frame_bytes;

    esp_err_t ret = ESP_OK;
    while (remaining > 0 && ret == ESP_OK) {
        size_t n = remaining < step ? remaining : step;
        ret = port_codec_read(chunk, n);
        remaining -= n;
    }
    heap_caps_free(chunk);
    return ret;
}

// Take every lock, in the one order that cannot deadlock: playback, capture,
// state. Play and record each take their own direction and then the state.
static void take_all(void)
{
    xSemaphoreTake(s_audio.tx_lock, portMAX_DELAY);
    xSemaphoreTake(s_audio.rx_lock, portMAX_DELAY);
    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
}

static void give_all(void)
{
    xSemaphoreGive(s_audio.lock);
    xSemaphoreGive(s_audio.rx_lock);
    xSemaphoreGive(s_audio.tx_lock);
}

// (Re)start the idle countdown. esp_timer refuses to start a timer that is
// already armed, so it is stopped first.
static void arm_idle_timer(void)
{
    esp_timer_stop(s_audio.idle_timer);
    esp_timer_start_once(s_audio.idle_timer, (uint64_t)AMP_IDLE_MS * 1000);
}

// Runs in the esp_timer task, which must not block, so the locks are only
// ever tried. A driver that is busy — a write or read in flight, or a volume
// change, which does not re-arm the timer — is tried again a timeout later.
static void idle_timeout(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_audio.tx_lock, 0) != pdTRUE) {
        arm_idle_timer();
        return;
    }
    if (xSemaphoreTake(s_audio.rx_lock, 0) != pdTRUE) {
        xSemaphoreGive(s_audio.tx_lock);
        arm_idle_timer();
        return;
    }
    if (xSemaphoreTake(s_audio.lock, 0) != pdTRUE) {
        xSemaphoreGive(s_audio.rx_lock);
        xSemaphoreGive(s_audio.tx_lock);
        arm_idle_timer();
        return;
    }
    if (s_audio.initialized) go_quiet();
    give_all();
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
    s_audio.mic_gain_db = DEFAULT_MIC_GAIN_DB;
    s_audio.open = false;

    // Claim the amplifier line before anything else: until it is driven it
    // reads "on", and the codec bring-up below is audible through it.
    esp_err_t ret = amp_set(false);
    if (ret != ESP_OK) return ret;

    SemaphoreHandle_t *locks[] = { &s_audio.tx_lock, &s_audio.rx_lock, &s_audio.lock };
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++) {
        if (!*locks[i]) *locks[i] = xSemaphoreCreateMutex();
        if (!*locks[i]) return ESP_ERR_NO_MEM;
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
    ESP_LOGI(TAG, "initialized on I2S%d (MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d) amp=%d/%d",
             s_audio.cfg.i2s_num, s_audio.cfg.pin_mclk, s_audio.cfg.pin_bclk,
             s_audio.cfg.pin_ws, s_audio.cfg.pin_dout, s_audio.cfg.pin_din,
             (int)s_audio.cfg.amp_expander, (int)s_audio.cfg.amp_expander_pin);
    return ESP_OK;
}

static void es8311_deinit(void)
{
    if (!s_audio.initialized) return;

    esp_timer_stop(s_audio.idle_timer);
    take_all();
    go_quiet();
    port_codec_destroy();
    s_audio.initialized = false;
    give_all();
}

static esp_err_t es8311_play(const uint8_t *data, size_t len)
{
    if (!s_audio.initialized) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_audio.tx_lock, portMAX_DELAY);
    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    esp_timer_stop(s_audio.idle_timer);
    esp_err_t ret = ensure_open();
    // Only once the codec is running, so its start-up is not amplified.
    if (ret == ESP_OK && !s_audio.amp_on) ret = amp_set(true);
    xSemaphoreGive(s_audio.lock);

    // The playback lock alone keeps the codec open under the write.
    if (ret == ESP_OK) ret = port_codec_write(data, len);

    arm_idle_timer();
    xSemaphoreGive(s_audio.tx_lock);
    return ret;
}

static esp_err_t es8311_record(uint8_t *buf, size_t len)
{
    if (!s_audio.initialized) return ESP_ERR_INVALID_STATE;
    if (!has_mic()) return ESP_ERR_NOT_SUPPORTED;
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_audio.rx_lock, portMAX_DELAY);
    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    esp_timer_stop(s_audio.idle_timer);
    esp_err_t ret = ensure_open();
    size_t unsettled = ret == ESP_OK ? settling_bytes() : 0;
    xSemaphoreGive(s_audio.lock);

    // The capture lock alone keeps the codec open under the reads, so a
    // player is not held up for the settling time either.
    if (ret == ESP_OK && unsettled > 0) ret = drop_bytes(unsettled);
    if (ret == ESP_OK) ret = port_codec_read(buf, len);

    // The same countdown as playback: a recorder that stops reading without
    // calling stop still has the codec closed behind it.
    arm_idle_timer();
    xSemaphoreGive(s_audio.rx_lock);
    return ret;
}

static esp_err_t es8311_stop(void)
{
    if (!s_audio.initialized) return ESP_ERR_INVALID_STATE;

    take_all();
    esp_timer_stop(s_audio.idle_timer);
    go_quiet();
    give_all();
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

static esp_err_t es8311_set_mic_gain(uint8_t db)
{
    if (db > MAX_MIC_GAIN_DB) db = MAX_MIC_GAIN_DB;
    if (!s_audio.initialized) {
        s_audio.mic_gain_db = db;
        return ESP_OK;
    }
    if (!has_mic()) return ESP_ERR_NOT_SUPPORTED;

    xSemaphoreTake(s_audio.lock, portMAX_DELAY);
    s_audio.mic_gain_db = db;
    esp_err_t ret = s_audio.open ? port_codec_set_mic_gain(db) : ESP_OK;
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

    // The codec is reopened at the new format by the next play or record.
    take_all();
    esp_timer_stop(s_audio.idle_timer);
    go_quiet();
    s_audio.fs = *cfg;
    give_all();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// vtable + get
// ---------------------------------------------------------------------------

static const hal_audio_driver_t s_vtable = {
    .init         = es8311_init,
    .deinit       = es8311_deinit,
    .play         = es8311_play,
    .stop         = es8311_stop,
    .set_volume   = es8311_set_volume,
    .configure    = es8311_configure,
    .name         = "ES8311",
    .record       = es8311_record,
    .set_mic_gain = es8311_set_mic_gain,
};

const hal_audio_driver_t *drv_audio_es8311_get(void)
{
    return &s_vtable;
}
