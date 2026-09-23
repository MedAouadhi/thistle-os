// SPDX-License-Identifier: BSD-3-Clause
//
// Host tests for the ES8311 driver's amplifier gating. The T-Deck Max's
// speaker amplifier hisses and draws current whenever its enable line is high,
// and that line floats high until something drives it, so these pin down that
// the driver claims it at init and raises it only around playback.

#include <assert.h>
#include <stdio.h>

#define ES8311_HOST_TEST
#include "../src/drv_audio_es8311.c"

#define AMP_EXPANDER 0
#define AMP_PIN      6

static const hal_audio_driver_t *drv;

static audio_es8311_config_t config(void)
{
    return (audio_es8311_config_t){
        .i2s_num = 0, .i2c_bus = (void *)1, .i2c_addr = 0x18,
        .pin_mclk = 38, .pin_bclk = 39, .pin_ws = 18, .pin_dout = 40,
        .amp_expander = AMP_EXPANDER, .amp_expander_pin = AMP_PIN,
    };
}

static void reset(void)
{
    if (drv) drv->deinit();
    drv = drv_audio_es8311_get();
    s_event_count = 0;
    s_codec_create_result = ESP_OK;
    s_expander_result = ESP_OK;
    s_during_write = NULL;
    s_expander_calls = 0;
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

static size_t count_of(shim_event_kind_t kind)
{
    size_t n = 0;
    for (size_t i = 0; i < s_event_count; i++) n += s_events[i].kind == kind;
    return n;
}

static const uint8_t PCM[64];

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
    s_mutex_storage = 1;
    s_timer.armed = false;  // it has just fired
    s_timer.callback(s_timer.arg);
    s_mutex_storage = 0;
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

int main(void)
{
    test_init_drives_the_amplifier_off_before_touching_the_codec();
    test_a_codec_that_will_not_come_up_leaves_the_amplifier_off();
    test_an_amplifier_line_that_cannot_be_driven_fails_init();
    test_play_opens_the_codec_then_raises_the_amplifier_then_writes();
    test_a_stream_of_plays_raises_the_amplifier_once();
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
    puts("es8311 amplifier gating: all tests passed");
    return 0;
}
