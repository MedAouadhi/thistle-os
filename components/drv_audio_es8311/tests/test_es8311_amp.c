// SPDX-License-Identifier: BSD-3-Clause
//
// Host tests for the ES8311 driver's amplifier gating. The T-Deck Max's
// speaker amplifier hisses and draws current whenever its enable line is high,
// and that line floats high until something drives it, so these pin down that
// the driver claims it at init and raises it only around playback. Recording
// shares the codec with playback, so the capture tests pin down that a record
// brings the codec up without ever raising the amplifier. The route tests pin
// down that a call handed the speaker keeps the amplifier as it asked, with
// nothing here dropping it or playing over it.

#include <assert.h>
#include <stdio.h>

#define ES8311_HOST_TEST
#include "../src/drv_audio_es8311.c"

#define AMP_EXPANDER 0
#define AMP_PIN      6

// The T-Deck Max's AUDIO_SEL: XL9555 P12, expander pin 10, high = A7682E.
#define ROUTE_EXPANDER 0
#define ROUTE_PIN      10
#define ROUTE_VOICE    1

static const hal_audio_driver_t *drv;

static audio_es8311_config_t config(void)
{
    return (audio_es8311_config_t){
        .i2s_num = 0, .i2c_bus = (void *)1, .i2c_addr = 0x18,
        .pin_mclk = 38, .pin_bclk = 39, .pin_ws = 18, .pin_dout = 40, .pin_din = 17,
        .amp_expander = AMP_EXPANDER, .amp_expander_pin = AMP_PIN,
        .voice_route_expander = -1,
    };
}

static audio_es8311_config_t route_config(void)
{
    audio_es8311_config_t cfg = config();
    cfg.voice_route_expander     = ROUTE_EXPANDER;
    cfg.voice_route_expander_pin = ROUTE_PIN;
    cfg.voice_route_level        = ROUTE_VOICE;
    return cfg;
}

static void reset(void)
{
    if (drv) drv->deinit();
    drv = drv_audio_es8311_get();
    s_event_count = 0;
    s_codec_create_result = ESP_OK;
    s_expander_result = ESP_OK;
    s_route_result = ESP_OK;
    s_route_pin = ROUTE_PIN;
    s_during_write = NULL;
    s_during_read = NULL;
    s_expander_calls = 0;
    s_now_us = 1000000;
}

static void init_with(audio_es8311_config_t cfg)
{
    assert(drv->init(&cfg) == ESP_OK);
    s_event_count = 0;
}

static size_t index_of(shim_event_kind_t kind)
{
    for (size_t i = 0; i < s_event_count; i++) {
        if (s_events[i].kind == kind) return i;
    }
    assert(!"event never happened");
    return 0;
}

static size_t last_index_of(shim_event_kind_t kind)
{
    for (size_t i = s_event_count; i-- > 0;) {
        if (s_events[i].kind == kind) return i;
    }
    assert(!"event never happened");
    return 0;
}

// Bytes read from the codec and thrown away before the caller's own read,
// which is the last read in the log.
static size_t dropped_bytes(void)
{
    size_t n = 0, last = last_index_of(EV_CODEC_READ);
    for (size_t i = 0; i < last; i++) {
        if (s_events[i].kind == EV_CODEC_READ) n += s_events[i].arg;
    }
    return n;
}

static size_t count_of(shim_event_kind_t kind)
{
    size_t n = 0;
    for (size_t i = 0; i < s_event_count; i++) n += s_events[i].kind == kind;
    return n;
}

static const uint8_t PCM[64];
static uint8_t MIC[64];

static void test_init_drives_the_amplifier_off_before_touching_the_codec(void)
{
    reset();
    audio_es8311_config_t cfg = config();
    assert(drv->init(&cfg) == ESP_OK);

    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CREATE));
    assert(count_of(EV_AMP_ON) == 0);
    assert(s_last_expander == AMP_EXPANDER && s_last_expander_pin == AMP_PIN);
}

static void test_a_codec_that_will_not_come_up_leaves_the_amplifier_off(void)
{
    reset();
    s_codec_create_result = ESP_FAIL;
    audio_es8311_config_t cfg = config();
    assert(drv->init(&cfg) == ESP_FAIL);

    assert(count_of(EV_AMP_OFF) == 1);
    assert(count_of(EV_AMP_ON) == 0);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_ERR_INVALID_STATE);
}

static void test_an_amplifier_line_that_cannot_be_driven_fails_init(void)
{
    reset();
    s_expander_result = ESP_ERR_NOT_FOUND;
    audio_es8311_config_t cfg = config();
    assert(drv->init(&cfg) == ESP_ERR_NOT_FOUND);
    assert(count_of(EV_CODEC_CREATE) == 0);
}

static void test_play_opens_the_codec_then_raises_the_amplifier_then_writes(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);

    assert(index_of(EV_CODEC_OPEN) < index_of(EV_AMP_ON));
    assert(index_of(EV_AMP_ON) < index_of(EV_CODEC_WRITE));
}

static void test_the_device_stays_awake_while_the_codec_is_open(void)
{
    reset();
    init_with(config());
    int before = s_power_holds;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(s_power_holds == before + 1);
    assert(drv->stop() == ESP_OK);
    assert(s_power_holds == before);
}

static void test_a_stream_of_plays_raises_the_amplifier_once(void)
{
    reset();
    init_with(config());
    for (int i = 0; i < 3; i++) assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);

    assert(count_of(EV_CODEC_OPEN) == 1);
    assert(count_of(EV_AMP_ON) == 1);
    assert(count_of(EV_CODEC_WRITE) == 3);
}

static void test_stop_drops_the_amplifier_before_closing_the_codec(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_event_count = 0;

    assert(drv->stop() == ESP_OK);
    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CLOSE));
    assert(!s_timer.armed);
}

static void test_a_caller_that_never_stops_is_silenced_once_it_goes_idle(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(s_timer.armed);
    s_event_count = 0;

    assert(shim_fire_idle_timer());
    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CLOSE));

    // The next play brings everything back up rather than writing to a
    // closed codec.
    s_event_count = 0;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(count_of(EV_CODEC_OPEN) == 1 && count_of(EV_AMP_ON) == 1);
}

// Deliver the timeout regardless of whether the timer is still armed: the
// callback can already be on its way when a play stops the timer.
static void fire_idle_timer_mid_write(void) { s_timer.callback(s_timer.arg); }

static void test_an_idle_timeout_that_lands_during_a_write_is_ignored(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);

    // A timeout from the previous play, delivered while the next one is still
    // writing: the write holds the driver, so the timeout stands down, and
    // the play re-arms it on the way out.
    s_during_write = fire_idle_timer_mid_write;
    s_event_count = 0;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);

    assert(count_of(EV_AMP_OFF) == 0);
    assert(count_of(EV_CODEC_CLOSE) == 0);
    assert(s_timer.armed);
}

static void test_an_idle_timeout_that_finds_the_driver_busy_tries_again(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_event_count = 0;

    // Not every holder of the lock is a play that will re-arm the timer — a
    // volume change is not. A timeout that just stood down would leave the
    // amplifier on for good.
    *s_audio.lock = 1;
    s_timer.armed = false;  // it has just fired
    s_timer.callback(s_timer.arg);
    *s_audio.lock = 0;
    assert(count_of(EV_AMP_OFF) == 0);
    assert(s_timer.armed);

    assert(shim_fire_idle_timer());
    assert(count_of(EV_AMP_OFF) == 1);
}

static void test_an_idle_timeout_that_arrives_after_deinit_does_nothing(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    drv->deinit();
    s_event_count = 0;

    // esp_timer_stop does not wait for a callback already on its way.
    s_timer.callback(s_timer.arg);
    assert(s_event_count == 0);
    assert(!s_timer.armed);

    // And the driver comes back up cleanly afterwards.
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
}

static void test_a_new_format_takes_effect_at_the_next_play(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_event_count = 0;

    hal_audio_config_t fs = { .sample_rate = 16000, .bits_per_sample = 16, .channels = 1 };
    assert(drv->configure(&fs) == ESP_OK);
    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CLOSE));

    s_event_count = 0;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(s_events[index_of(EV_CODEC_OPEN)].arg == 16000);
}

static void test_a_format_the_codec_cannot_carry_is_refused(void)
{
    reset();
    init_with(config());
    hal_audio_config_t fs = { .sample_rate = 44100, .bits_per_sample = 12, .channels = 2 };
    assert(drv->configure(&fs) == ESP_ERR_INVALID_ARG);
    fs = (hal_audio_config_t){ .sample_rate = 44100, .bits_per_sample = 16, .channels = 3 };
    assert(drv->configure(&fs) == ESP_ERR_INVALID_ARG);
}

static void test_volume_is_applied_at_open_and_immediately_while_open(void)
{
    reset();
    init_with(config());
    assert(drv->set_volume(30) == ESP_OK);
    assert(count_of(EV_CODEC_VOLUME) == 0);

    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(s_events[index_of(EV_CODEC_VOLUME)].arg == 30);
    assert(index_of(EV_CODEC_VOLUME) < index_of(EV_AMP_ON));

    s_event_count = 0;
    assert(drv->set_volume(120) == ESP_OK);
    assert(s_events[index_of(EV_CODEC_VOLUME)].arg == 100);
}

static void test_deinit_while_playing_silences_before_tearing_down(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_event_count = 0;

    drv->deinit();
    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CLOSE));
    assert(index_of(EV_CODEC_CLOSE) < index_of(EV_CODEC_DESTROY));
    drv = NULL;
}

static void test_a_board_without_an_amplifier_line_never_touches_an_expander(void)
{
    reset();
    audio_es8311_config_t cfg = config();
    cfg.amp_expander = -1;
    init_with(cfg);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(drv->stop() == ESP_OK);
    assert(s_expander_calls == 0);
}

static void test_play_refuses_nothing_to_play(void)
{
    reset();
    init_with(config());
    assert(drv->play(NULL, 4) == ESP_ERR_INVALID_ARG);
    assert(drv->play(PCM, 0) == ESP_ERR_INVALID_ARG);
    assert(count_of(EV_AMP_ON) == 0);
}

static void test_record_opens_the_codec_and_leaves_the_amplifier_off(void)
{
    reset();
    init_with(config());
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);

    assert(index_of(EV_CODEC_OPEN) < index_of(EV_CODEC_READ));
    assert(s_events[last_index_of(EV_CODEC_READ)].arg == sizeof(MIC));
    assert(count_of(EV_AMP_ON) == 0);
}

static void test_record_applies_the_mic_gain_before_reading(void)
{
    reset();
    init_with(config());
    assert(drv->set_mic_gain(30) == ESP_OK);
    assert(count_of(EV_CODEC_MIC_GAIN) == 0);

    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(s_events[index_of(EV_CODEC_MIC_GAIN)].arg == 30);
    assert(index_of(EV_CODEC_MIC_GAIN) < index_of(EV_CODEC_READ));

    // Applied at once while open, and held to the preamp's 42 dB ceiling.
    s_event_count = 0;
    assert(drv->set_mic_gain(60) == ESP_OK);
    assert(s_events[index_of(EV_CODEC_MIC_GAIN)].arg == 42);
}

static void test_play_while_recording_raises_the_amplifier_without_reopening(void)
{
    reset();
    init_with(config());
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);

    assert(count_of(EV_CODEC_OPEN) == 1);
    assert(count_of(EV_AMP_ON) == 1);
    size_t callers_reads = 0;
    for (size_t i = 0; i < s_event_count; i++) {
        callers_reads += s_events[i].kind == EV_CODEC_READ && s_events[i].arg == sizeof(MIC);
    }
    assert(callers_reads == 2);
}

static void test_a_recorder_that_stops_reading_has_the_codec_closed(void)
{
    reset();
    init_with(config());
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(s_timer.armed);
    s_event_count = 0;

    assert(shim_fire_idle_timer());
    assert(count_of(EV_CODEC_CLOSE) == 1);
}

static void test_a_board_without_a_mic_line_refuses_to_record(void)
{
    reset();
    audio_es8311_config_t cfg = config();
    cfg.pin_din = -1;
    init_with(cfg);

    assert(drv->record(MIC, sizeof(MIC)) == ESP_ERR_NOT_SUPPORTED);
    assert(drv->set_mic_gain(24) == ESP_ERR_NOT_SUPPORTED);
    assert(count_of(EV_CODEC_OPEN) == 0);

    // Playback is unaffected, and never touches a gain it has no use for.
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(count_of(EV_CODEC_MIC_GAIN) == 0);
}

static void test_record_refuses_nowhere_to_put_it(void)
{
    reset();
    init_with(config());
    assert(drv->record(NULL, 4) == ESP_ERR_INVALID_ARG);
    assert(drv->record(MIC, 0) == ESP_ERR_INVALID_ARG);
    assert(count_of(EV_CODEC_OPEN) == 0);
}

// The microphone's bias network takes about 400 ms to settle after the codec
// powers up: full-scale swings for the first ~75 ms, then a decaying DC
// offset. Measured on a T-Deck Max at 8, 16, 44.1 and 48 kHz (ticket 12).
#define FRAMES_44K_SETTLING (44100 * 400 / 1000)
#define RING_FRAMES         (6 * 240)

static void test_a_record_that_opens_the_codec_drops_the_settling_audio_first(void)
{
    reset();
    init_with(config());
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);

    // 44.1 kHz stereo 16-bit by default: four bytes a frame.
    assert(dropped_bytes() == FRAMES_44K_SETTLING * 4);
    assert(s_events[last_index_of(EV_CODEC_READ)].arg == sizeof(MIC));

    // Once settled, the next take is the caller's alone.
    s_event_count = 0;
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(count_of(EV_CODEC_READ) == 1);
}

static void test_the_settling_time_is_counted_at_the_recording_format(void)
{
    reset();
    init_with(config());
    hal_audio_config_t fs = { .sample_rate = 16000, .bits_per_sample = 16, .channels = 1 };
    assert(drv->configure(&fs) == ESP_OK);
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(dropped_bytes() == 16000 * 400 / 1000 * 2);
}

static void test_a_record_long_after_playback_opened_the_codec_drops_nothing(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_now_us += 2000000;
    s_event_count = 0;

    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(count_of(EV_CODEC_READ) == 1);
}

static void test_a_record_soon_after_playback_opened_the_codec_drops_the_rest(void)
{
    reset();
    init_with(config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    // 100 ms on, the I2S ring holds only its last 1440 frames, captured
    // between 67 and 100 ms; the settling runs on to 400 ms.
    s_now_us += 100000;
    s_event_count = 0;

    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(dropped_bytes() == (size_t)(FRAMES_44K_SETTLING - (4410 - RING_FRAMES)) * 4);
}

static void test_a_codec_reopened_after_going_idle_settles_again(void)
{
    reset();
    init_with(config());
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(shim_fire_idle_timer());
    s_now_us += 5000000;
    s_event_count = 0;

    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(dropped_bytes() == FRAMES_44K_SETTLING * 4);
}

// ── Two directions at once ──────────────────────────────────────────────────
// A player and a recorder in different tasks must not queue behind each
// other's I/O: a write blocks for as long as its audio takes to play, and a
// reader held off that long loses what overflowed the DMA ring meanwhile.

static int s_io_checks;

static void io_holds_only_its_own_direction_tx(void)
{
    assert(*s_audio.tx_lock == 1);
    assert(*s_audio.rx_lock == 0);
    assert(*s_audio.lock == 0);
    s_io_checks++;
}

static void io_holds_only_its_own_direction_rx(void)
{
    assert(*s_audio.rx_lock == 1);
    assert(*s_audio.tx_lock == 0);
    assert(*s_audio.lock == 0);
    s_io_checks++;
}

static void test_a_write_holds_only_the_playback_side(void)
{
    reset();
    init_with(config());
    s_io_checks = 0;
    s_during_write = io_holds_only_its_own_direction_tx;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(s_io_checks == 1);
}

static void test_a_read_and_its_settling_drop_hold_only_the_capture_side(void)
{
    reset();
    init_with(config());
    s_io_checks = 0;
    s_during_read = io_holds_only_its_own_direction_rx;
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(s_io_checks > 1);  // the drop's reads and the caller's
}

static void test_an_idle_timeout_that_lands_during_a_read_is_ignored(void)
{
    reset();
    init_with(config());
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);

    s_during_read = fire_idle_timer_mid_write;
    s_event_count = 0;
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);

    assert(count_of(EV_CODEC_CLOSE) == 0);
    assert(s_timer.armed);
}

// ── The voice route ─────────────────────────────────────────────────────────
// The T-Deck Max's speaker hears the codec or the A7682E through a switch
// (schematic page 8, U27/U28). While a call has it, the amplifier is the
// call's: on for the loudspeaker, off for headphones.

static void test_init_puts_the_switch_on_the_codec_once_the_amplifier_is_off(void)
{
    reset();
    audio_es8311_config_t cfg = route_config();
    assert(drv->init(&cfg) == ESP_OK);

    assert(count_of(EV_ROUTE) == 1);
    assert(s_events[index_of(EV_ROUTE)].arg == !ROUTE_VOICE);
    assert(index_of(EV_AMP_OFF) < index_of(EV_ROUTE));
    assert(index_of(EV_ROUTE) < index_of(EV_CODEC_CREATE));
    assert(count_of(EV_AMP_ON) == 0);
}

static void test_a_switch_that_cannot_be_driven_fails_init(void)
{
    reset();
    s_route_result = ESP_ERR_NOT_FOUND;
    audio_es8311_config_t cfg = route_config();
    assert(drv->init(&cfg) == ESP_ERR_NOT_FOUND);
    assert(count_of(EV_CODEC_CREATE) == 0);
}

static void test_the_voice_route_stops_playback_then_switches_then_raises_the_amplifier(void)
{
    reset();
    init_with(route_config());
    int before = s_power_holds;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_event_count = 0;

    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    // Quiet first, so neither the codec stopping nor the switch is heard.
    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CLOSE));
    assert(index_of(EV_CODEC_CLOSE) < index_of(EV_ROUTE));
    assert(s_events[index_of(EV_ROUTE)].arg == ROUTE_VOICE);
    assert(index_of(EV_ROUTE) < last_index_of(EV_AMP_ON));
    assert(s_audio.amp_on);
    // Nothing of playback's is left to drop the amplifier later.
    assert(!s_timer.armed);
    assert(s_power_holds == before);
}

static void test_the_voice_route_holds_the_amplifier_against_everything_else(void)
{
    reset();
    init_with(route_config());
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    s_event_count = 0;

    // A ringtone that plays on is refused, and touches nothing.
    assert(drv->play(PCM, sizeof(PCM)) == ESP_ERR_INVALID_STATE);
    assert(s_event_count == 0);
    assert(!s_timer.armed);

    // A recorder still runs, and its idle timeout closes the codec alone.
    assert(drv->record(MIC, sizeof(MIC)) == ESP_OK);
    assert(s_timer.armed);
    assert(shim_fire_idle_timer());
    assert(count_of(EV_CODEC_CLOSE) == 1);

    // A ringtone player's stop, or a new format, likewise.
    assert(drv->stop() == ESP_OK);
    hal_audio_config_t fs = { .sample_rate = 8000, .bits_per_sample = 16, .channels = 1 };
    assert(drv->configure(&fs) == ESP_OK);

    assert(count_of(EV_AMP_OFF) == 0);
    assert(count_of(EV_ROUTE) == 0);
    assert(s_audio.amp_on);
}

static void test_headphones_take_the_voice_route_with_the_amplifier_off(void)
{
    reset();
    init_with(route_config());
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, false) == ESP_OK);

    assert(s_events[index_of(EV_ROUTE)].arg == ROUTE_VOICE);
    assert(count_of(EV_AMP_ON) == 0);
    assert(!s_audio.amp_on);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_ERR_INVALID_STATE);
    assert(count_of(EV_AMP_ON) == 0);
}

static void test_the_voice_route_asked_again_changes_only_the_amplifier(void)
{
    reset();
    init_with(route_config());
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    s_event_count = 0;

    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, false) == ESP_OK);
    assert(count_of(EV_AMP_OFF) == 1 && count_of(EV_ROUTE) == 0);
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, false) == ESP_OK);
    assert(count_of(EV_AMP_OFF) == 1);
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    assert(count_of(EV_AMP_ON) == 1 && count_of(EV_ROUTE) == 0);
    assert(count_of(EV_CODEC_OPEN) == 0 && count_of(EV_CODEC_CLOSE) == 0);
}

static void test_back_to_the_codec_drops_the_amplifier_then_switches_and_gating_resumes(void)
{
    reset();
    init_with(route_config());
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    s_event_count = 0;

    assert(drv->set_route(HAL_AUDIO_ROUTE_CODEC, true) == ESP_OK);
    assert(index_of(EV_AMP_OFF) < index_of(EV_ROUTE));
    assert(s_events[index_of(EV_ROUTE)].arg == !ROUTE_VOICE);
    assert(count_of(EV_AMP_ON) == 0);

    // Playback has the amplifier again, around itself only.
    s_event_count = 0;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(index_of(EV_CODEC_OPEN) < index_of(EV_AMP_ON));
    assert(index_of(EV_AMP_ON) < index_of(EV_CODEC_WRITE));
    assert(shim_fire_idle_timer());
    assert(index_of(EV_AMP_OFF) < index_of(EV_CODEC_CLOSE));
}

static void test_the_codec_route_when_already_there_leaves_playback_alone(void)
{
    reset();
    init_with(route_config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    s_event_count = 0;

    assert(drv->set_route(HAL_AUDIO_ROUTE_CODEC, false) == ESP_OK);
    assert(s_event_count == 0);
    assert(s_audio.amp_on && s_timer.armed);
}

static void test_a_switch_that_will_not_move_leaves_the_route_where_it_was(void)
{
    reset();
    init_with(route_config());
    s_route_result = ESP_FAIL;
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_FAIL);
    assert(count_of(EV_AMP_ON) == 0);
    s_route_result = ESP_OK;
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);

    // And on the way back: still the call's until the switch has moved.
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    s_route_result = ESP_FAIL;
    assert(drv->set_route(HAL_AUDIO_ROUTE_CODEC, false) == ESP_FAIL);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_ERR_INVALID_STATE);
    s_route_result = ESP_OK;
    assert(drv->set_route(HAL_AUDIO_ROUTE_CODEC, false) == ESP_OK);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
}

static void test_a_switch_that_selects_the_voice_channel_low(void)
{
    reset();
    audio_es8311_config_t cfg = route_config();
    cfg.voice_route_level = 0;
    assert(drv->init(&cfg) == ESP_OK);
    assert(s_events[index_of(EV_ROUTE)].arg == 1);

    s_event_count = 0;
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    assert(s_events[index_of(EV_ROUTE)].arg == 0);
}

static void test_deinit_in_the_voice_route_leaves_the_board_quiet_on_the_codec(void)
{
    reset();
    init_with(route_config());
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_OK);
    s_event_count = 0;

    drv->deinit();
    assert(index_of(EV_AMP_OFF) < index_of(EV_ROUTE));
    assert(s_events[index_of(EV_ROUTE)].arg == !ROUTE_VOICE);

    // And the driver comes back up on the codec, playing.
    init_with(route_config());
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
    assert(count_of(EV_AMP_ON) == 1);
}

static void test_a_board_without_an_audio_switch_has_no_voice_route(void)
{
    reset();
    init_with(config());
    int calls = s_expander_calls;
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_ERR_NOT_SUPPORTED);
    assert(drv->set_route(HAL_AUDIO_ROUTE_CODEC, false) == ESP_ERR_NOT_SUPPORTED);
    assert(s_expander_calls == calls);
    assert(drv->play(PCM, sizeof(PCM)) == ESP_OK);
}

static void test_set_route_refuses_before_init_and_a_route_it_does_not_know(void)
{
    reset();
    assert(drv->set_route(HAL_AUDIO_ROUTE_VOICE, true) == ESP_ERR_INVALID_STATE);
    init_with(route_config());
    assert(drv->set_route((hal_audio_route_t)7, true) == ESP_ERR_INVALID_ARG);
    assert(s_event_count == 0);
}

int main(void)
{
    test_init_drives_the_amplifier_off_before_touching_the_codec();
    test_a_codec_that_will_not_come_up_leaves_the_amplifier_off();
    test_an_amplifier_line_that_cannot_be_driven_fails_init();
    test_play_opens_the_codec_then_raises_the_amplifier_then_writes();
    test_a_stream_of_plays_raises_the_amplifier_once();
    test_the_device_stays_awake_while_the_codec_is_open();
    test_stop_drops_the_amplifier_before_closing_the_codec();
    test_a_caller_that_never_stops_is_silenced_once_it_goes_idle();
    test_an_idle_timeout_that_lands_during_a_write_is_ignored();
    test_an_idle_timeout_that_finds_the_driver_busy_tries_again();
    test_an_idle_timeout_that_arrives_after_deinit_does_nothing();
    test_a_new_format_takes_effect_at_the_next_play();
    test_a_format_the_codec_cannot_carry_is_refused();
    test_volume_is_applied_at_open_and_immediately_while_open();
    test_deinit_while_playing_silences_before_tearing_down();
    test_a_board_without_an_amplifier_line_never_touches_an_expander();
    test_play_refuses_nothing_to_play();
    test_record_opens_the_codec_and_leaves_the_amplifier_off();
    test_record_applies_the_mic_gain_before_reading();
    test_play_while_recording_raises_the_amplifier_without_reopening();
    test_a_recorder_that_stops_reading_has_the_codec_closed();
    test_a_board_without_a_mic_line_refuses_to_record();
    test_record_refuses_nowhere_to_put_it();
    test_a_record_that_opens_the_codec_drops_the_settling_audio_first();
    test_the_settling_time_is_counted_at_the_recording_format();
    test_a_record_long_after_playback_opened_the_codec_drops_nothing();
    test_a_record_soon_after_playback_opened_the_codec_drops_the_rest();
    test_a_codec_reopened_after_going_idle_settles_again();
    test_a_write_holds_only_the_playback_side();
    test_a_read_and_its_settling_drop_hold_only_the_capture_side();
    test_an_idle_timeout_that_lands_during_a_read_is_ignored();
    test_init_puts_the_switch_on_the_codec_once_the_amplifier_is_off();
    test_a_switch_that_cannot_be_driven_fails_init();
    test_the_voice_route_stops_playback_then_switches_then_raises_the_amplifier();
    test_the_voice_route_holds_the_amplifier_against_everything_else();
    test_headphones_take_the_voice_route_with_the_amplifier_off();
    test_the_voice_route_asked_again_changes_only_the_amplifier();
    test_back_to_the_codec_drops_the_amplifier_then_switches_and_gating_resumes();
    test_the_codec_route_when_already_there_leaves_playback_alone();
    test_a_switch_that_will_not_move_leaves_the_route_where_it_was();
    test_a_switch_that_selects_the_voice_channel_low();
    test_deinit_in_the_voice_route_leaves_the_board_quiet_on_the_codec();
    test_a_board_without_an_audio_switch_has_no_voice_route();
    test_set_route_refuses_before_init_and_a_route_it_does_not_know();
    puts("es8311 amplifier gating and capture: all tests passed");
    return 0;
}
