#include <algorithm>
#include <cassert>

#define SX1262_LIFECYCLE_HOST_TEST
#include "../src/drv_radio_sx1262.cpp"

static size_t event_index(sx1262_test::Event event)
{
    const auto it = std::find(sx1262_test::events.begin(), sx1262_test::events.end(), event);
    assert(it != sx1262_test::events.end());
    return static_cast<size_t>(it - sx1262_test::events.begin());
}

static size_t event_count(sx1262_test::Event event)
{
    return static_cast<size_t>(std::count(
        sx1262_test::events.begin(), sx1262_test::events.end(), event));
}

static bool has_event(sx1262_test::Event event)
{
    std::lock_guard<std::mutex> lock(sx1262_test::events_mutex);
    return std::find(sx1262_test::events.begin(), sx1262_test::events.end(), event)
        != sx1262_test::events.end();
}

static radio_sx1262_config_t config()
{
    return {1, 2, 3, 4, 5, 8000000, 0, 0, false};
}

/* An SX1262 fitted with a TCXO runs its oscillator off DIO3, and until DIO3
 * supplies it the PLL never locks: `begin()` still succeeds, and every
 * transmission then times out waiting for a TxDone that cannot come. Modules
 * with a plain crystal must not be sent the command at all. */
static void a_module_gets_the_oscillator_and_switch_its_board_declares()
{
    sx1262_test::reset();
    auto fitted = config();
    fitted.tcxo_mv = 2400;
    fitted.dio2_rf_switch = true;
    assert(sx1262_init(&fitted) == ESP_OK);
    assert(sx1262_test::tcxo_calls == 1);
    assert(sx1262_test::tcxo_voltage == 2.4f);
    assert(sx1262_test::dio2_rf_switch_calls == 1);
    sx1262_deinit();

    sx1262_test::reset();
    auto bare = config();
    assert(sx1262_init(&bare) == ESP_OK);
    assert(sx1262_test::tcxo_calls == 0);
    assert(sx1262_test::dio2_rf_switch_calls == 0);
    sx1262_deinit();
}

/* The band is the board's, not the part's: the same SX1262 ships matched to
 * 868 MHz in Europe and 915 MHz in North America, and a board profile is the
 * only place that difference is written down. */
static void band_comes_from_the_board_that_declares_it()
{
    sx1262_test::reset();
    auto eu = config();
    eu.freq_hz = 868000000;
    assert(sx1262_init(&eu) == ESP_OK);
    assert(sx1262_test::begin_frequency == 868.0f);
    sx1262_deinit();

    /* A board that declares no band keeps the 915 MHz this driver has always
     * begun at, so the boards already in the catalogue are unmoved. */
    sx1262_test::reset();
    auto silent = config();
    assert(sx1262_init(&silent) == ESP_OK);
    assert(sx1262_test::begin_frequency == 915.0f);
    sx1262_deinit();
}

/* MeshCore nodes send 16 preamble symbols and listen for 16. A receiver set
 * for fewer than a sender uses can miss the packet altogether. */
static void the_radio_uses_the_preamble_meshcore_nodes_use()
{
    sx1262_test::reset();
    auto cfg = config();
    assert(sx1262_init(&cfg) == ESP_OK);
    assert(sx1262_test::begin_preamble == 16);
    sx1262_deinit();
}

/* Wait for the receive task to read, but bounded: a regression here should fail
 * the suite rather than hang CI until it is killed. */
static void await_read(int expected)
{
    for (int i = 0; i < 1000 && sx1262_test::read_calls < expected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(sx1262_test::read_calls == expected);
}

/* DIO1 carries both RxDone and TxDone. The receive task runs at the highest
 * priority, so on a TxDone it preempts the sender, reads a packet that was
 * never received, and in doing so clears the very flag the sender is waiting
 * on — which then times out. A transmission must pass the task by. */
static void a_transmission_is_not_read_back_as_a_received_packet()
{
    sx1262_test::reset();
    auto cfg = config();
    assert(sx1262_init(&cfg) == ESP_OK);

    dio1_isr();
    for (int i = 0; i < 1000 && sx1262_test::read_calls == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(sx1262_test::read_calls == 0);
    assert(sx1262_test::rearm_calls == 0);

    /* Once receiving, the same line means what the task assumes it means. */
    assert(sx1262_start_receive(nullptr, nullptr) == ESP_OK);
    dio1_isr();
    await_read(1);
    sx1262_deinit();
}

/* A transmission puts the radio in standby, so a listener that sends goes deaf
 * unless listening is picked back up. A messenger calls start_receive once and
 * then sends; without this it hears the first message and nothing after its own
 * first reply. */
static void a_listener_that_sends_is_still_listening_afterwards()
{
    sx1262_test::reset();
    auto cfg = config();
    assert(sx1262_init(&cfg) == ESP_OK);

    uint8_t payload[] = {1, 2, 3};
    assert(sx1262_start_receive(nullptr, nullptr) == ESP_OK);
    const int armed_before = sx1262_test::rearm_calls;
    assert(sx1262_send(payload, sizeof(payload)) == ESP_OK);
    assert(sx1262_test::rearm_calls == armed_before + 1);

    /* And a packet arriving after that send is still read. */
    dio1_isr();
    await_read(1);
    sx1262_deinit();

    /* A radio nobody asked to listen to stays quiet after a send. */
    sx1262_test::reset();
    assert(sx1262_init(&cfg) == ESP_OK);
    assert(sx1262_send(payload, sizeof(payload)) == ESP_OK);
    assert(sx1262_test::rearm_calls == 0);
    sx1262_deinit();
}

int main()
{
    auto cfg = config();

    band_comes_from_the_board_that_declares_it();
    the_radio_uses_the_preamble_meshcore_nodes_use();
    a_module_gets_the_oscillator_and_switch_its_board_declares();
    a_transmission_is_not_read_back_as_a_received_packet();
    a_listener_that_sends_is_still_listening_afterwards();

    sx1262_test::reset();
    sx1262_test::fail_task_create = true;
    assert(sx1262_init(&cfg) == ESP_ERR_NO_MEM);
    assert(!sx1262_test::interrupt_attached);
    assert(event_index(sx1262_test::Event::Detach) < event_index(sx1262_test::Event::RadioDelete));
    assert(event_index(sx1262_test::Event::RadioDelete) < event_index(sx1262_test::Event::ModuleDelete));
    assert(event_index(sx1262_test::Event::ModuleDelete) < event_index(sx1262_test::Event::HalDelete));
    assert(event_count(sx1262_test::Event::SemaphoreDelete) == 1);

    sx1262_test::fail_task_create = false;
    assert(sx1262_init(&cfg) == ESP_OK);
    sx1262_deinit();

    sx1262_test::reset();
    sx1262_test::fail_semaphore_create = true;
    assert(sx1262_init(&cfg) == ESP_ERR_NO_MEM);
    assert(!sx1262_test::interrupt_attached);
    assert(event_count(sx1262_test::Event::Attach) == 0);
    assert(event_count(sx1262_test::Event::RadioDelete) == 1);

    sx1262_test::fail_semaphore_create = false;
    assert(sx1262_init(&cfg) == ESP_OK);
    sx1262_deinit();

    sx1262_test::reset();
    assert(sx1262_init(&cfg) == ESP_OK);
    assert(sx1262_start_receive(nullptr, nullptr) == ESP_OK);
    sx1262_test::block_read = true;
    dio1_isr();

    {
        std::unique_lock<std::mutex> lock(sx1262_test::read_mutex);
        sx1262_test::read_cv.wait(lock, [] { return sx1262_test::read_entered; });
    }

    std::thread deinit_thread([] { sx1262_deinit(); });
    while (!has_event(sx1262_test::Event::Detach)) std::this_thread::yield();
    assert(!has_event(sx1262_test::Event::RadioDelete));
    {
        std::lock_guard<std::mutex> lock(sx1262_test::read_mutex);
        sx1262_test::release_read = true;
    }
    sx1262_test::read_cv.notify_all();
    deinit_thread.join();
    sx1262_deinit();

    assert(!sx1262_test::interrupt_attached);
    assert(event_index(sx1262_test::Event::Detach) < event_index(sx1262_test::Event::TaskExit));
    assert(event_index(sx1262_test::Event::TaskExit) < event_index(sx1262_test::Event::Standby));
    assert(event_index(sx1262_test::Event::Standby) < event_index(sx1262_test::Event::RadioDelete));
    assert(sx1262_test::rearm_calls == 1);

    sx1262_test::reset();
    assert(sx1262_init(&cfg) == ESP_OK);
    sx1262_test::block_isr = true;
    std::thread isr_thread([] { dio1_isr(); });
    {
        std::unique_lock<std::mutex> lock(sx1262_test::isr_mutex);
        sx1262_test::isr_cv.wait(lock, [] { return sx1262_test::isr_entered; });
    }
    std::thread isr_deinit_thread([] { sx1262_deinit(); });
    while (!has_event(sx1262_test::Event::Detach)) std::this_thread::yield();
    assert(!has_event(sx1262_test::Event::RadioDelete));
    {
        std::lock_guard<std::mutex> lock(sx1262_test::isr_mutex);
        sx1262_test::release_isr = true;
    }
    sx1262_test::isr_cv.notify_all();
    isr_thread.join();
    isr_deinit_thread.join();
    assert(event_index(sx1262_test::Event::Detach) < event_index(sx1262_test::Event::TaskExit));
    assert(event_index(sx1262_test::Event::TaskExit) < event_index(sx1262_test::Event::RadioDelete));

    for (int i = 0; i < 100; ++i) {
        sx1262_test::reset();
        assert(sx1262_init(&cfg) == ESP_OK);
        dio1_isr();
        sx1262_deinit();
        sx1262_deinit();
        assert(!sx1262_test::interrupt_attached);
    }
    return 0;
}
