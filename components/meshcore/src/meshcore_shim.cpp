// SPDX-License-Identifier: BSD-3-Clause
// ThistleOS — MeshCore engine
//
// A MeshCore chat node (BaseChatMesh) on its own task, on whatever radio the
// HAL has registered. Everything that touches the mesh happens on that task:
// commands arrive on a queue, and what the node hears goes out through the
// callbacks the kernel registered. Nothing else ever holds the mesh, so it
// needs no lock.
//
// What a MeshCore node keeps across a reboot, this keeps too: its identity, or
// every peer would see a stranger after each boot, and its contacts with their
// routes. It also keeps the clock moving forward, because a peer ignores an
// advert whose timestamp is no later than the last one it had from this node.

#include "meshcore_shim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Keep SimpleMeshTables from reaching for the Arduino filesystem.
#undef ESP32

#include <Mesh.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <SHA256.h>

#define ESP32 1

extern "C" {
#include "hal/board.h"
}

static const char* TAG = "meshcore";

// Arduino.h declares it; MeshCore's debug printing writes to it.
FakeSerial Serial;

// The key every MeshCore node holds for its Public channel
// (examples/companion_radio/MyMesh.cpp).
#define PUBLIC_CHANNEL_NAME   "Public"
#define PUBLIC_CHANNEL_PSK    "izOH6cXN6mrJ5e26oRXNcg=="

// A clock that has never been set starts here, as MeshCore's own do.
#define CLOCK_BASELINE        1715770351u   // 15 May 2024

#define TASK_STACK            20480   // the crypto self-check alone used 10.7 KB
#define TASK_PRIORITY         5
#define LOOP_PERIOD_MS        10
#define CMD_QUEUE_DEPTH       8
#define PACKET_POOL           16
#define RX_SLOTS              8
#define MAX_PENDING           8
#define SEND_ATTEMPTS         3
#define BOOT_ADVERT_DELAY_MS  2000
#define SAVE_DELAY_MS         5000

#define IDENTITY_FILE         "mesh_identity"
#define CONTACTS_FILE         "mesh_contacts"
#define IDENTITY_MAGIC        0x3149434du   // "MCI1"
#define CONTACTS_MAGIC        0x3143434du   // "MCC1"

static void copy_str(char* dest, size_t size, const char* src) {
    if (size == 0) return;
    size_t n = src ? strnlen(src, size - 1) : 0;
    if (n) memcpy(dest, src, n);
    dest[n] = '\0';
}

static bool clock_is_set(time_t t) { return t >= (time_t)CLOCK_BASELINE; }

// ── Radio ──────────────────────────────────────────────────────────────
//
// The HAL radio hands over each packet from its own interrupt task, while
// MeshCore polls recvRaw() from the mesh task. A few slots between them keep
// packets that arrive close together, where a single buffer kept only the last.

class HalRadio : public mesh::Radio {
    struct Slot {
        uint8_t len;
        int16_t rssi;
        uint8_t data[MAX_TRANS_UNIT];
    };

    const hal_radio_driver_t* _drv = nullptr;
    meshcore_config_t _cfg = {};
    Slot _slots[RX_SLOTS];
    int _head = 0;
    int _count = 0;
    portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
    float _last_rssi = 0;
    bool _sending = false;

    static void on_rx(const uint8_t* data, size_t len, int rssi, void* ctx) {
        auto* self = static_cast<HalRadio*>(ctx);
        if (len == 0 || len > MAX_TRANS_UNIT) return;
        portENTER_CRITICAL(&self->_lock);
        if (self->_count == RX_SLOTS) {
            self->dropped++;
        } else {
            Slot& slot = self->_slots[(self->_head + self->_count) % RX_SLOTS];
            slot.len = (uint8_t)len;
            slot.rssi = (int16_t)rssi;
            memcpy(slot.data, data, len);
            self->_count++;
        }
        portEXIT_CRITICAL(&self->_lock);
    }

public:
    uint32_t dropped = 0;
    uint32_t received = 0;

    // Put the radio on the mesh's channel. Every setting has to take, or the
    // node would be on air somewhere no peer listens.
    esp_err_t configure(const hal_radio_driver_t* drv, const meshcore_config_t& cfg) {
        _drv = drv;
        _cfg = cfg;
        if (!drv->set_frequency || !drv->set_bandwidth || !drv->set_spreading_factor ||
            !drv->send || !drv->start_receive) {
            ESP_LOGE(TAG, "radio %s cannot be tuned for the mesh", drv->name ? drv->name : "?");
            return ESP_ERR_NOT_SUPPORTED;
        }
        esp_err_t err;
        if ((err = drv->set_frequency(cfg.freq_hz)) != ESP_OK ||
            (err = drv->set_bandwidth(cfg.bw_hz)) != ESP_OK ||
            (err = drv->set_spreading_factor(cfg.sf)) != ESP_OK) {
            ESP_LOGE(TAG, "radio refused %u Hz / %u Hz / SF%u: %s", (unsigned)cfg.freq_hz,
                     (unsigned)cfg.bw_hz, (unsigned)cfg.sf, esp_err_to_name(err));
            return err;
        }
        if (drv->set_tx_power && drv->set_tx_power(cfg.tx_power_dbm) != ESP_OK) {
            ESP_LOGW(TAG, "radio refused %d dBm; keeping its own power", cfg.tx_power_dbm);
        }
        return ESP_OK;
    }

    void begin() override {
        esp_err_t err = _drv->start_receive(on_rx, this);
        if (err != ESP_OK) ESP_LOGE(TAG, "radio would not listen: %s", esp_err_to_name(err));
    }

    int recvRaw(uint8_t* bytes, int sz) override {
        int n = 0;
        portENTER_CRITICAL(&_lock);
        if (_count > 0) {
            Slot& slot = _slots[_head];
            n = slot.len < sz ? slot.len : sz;
            memcpy(bytes, slot.data, n);
            _last_rssi = slot.rssi;
            _head = (_head + 1) % RX_SLOTS;
            _count--;
        }
        portEXIT_CRITICAL(&_lock);
        if (n > 0) received++;
        return n;
    }

    // LoRa time on air (Semtech AN1200.13) for an explicit header with CRC,
    // at the preamble MeshCore uses.
    uint32_t getEstAirtimeFor(int len_bytes) override {
        const double symbol_ms = (double)(1u << _cfg.sf) * 1000.0 / (double)_cfg.bw_hz;
        const int ldro = symbol_ms > 16.0 ? 1 : 0;
        const int cr = (_cfg.cr >= 5 && _cfg.cr <= 8) ? _cfg.cr - 4 : 1;
        const int sf = _cfg.sf;
        double numerator = 8.0 * len_bytes - 4.0 * sf + 28 + 16;
        double payload_symbols = 8;
        if (numerator > 0) {
            payload_symbols += ceil(numerator / (4.0 * (sf - 2 * ldro))) * (cr + 4);
        }
        const double preamble_symbols = 16 + 4.25;
        return (uint32_t)ceil((preamble_symbols + payload_symbols) * symbol_ms);
    }

    float packetScore(float snr, int packet_len) override { return snr; }

    // The HAL's send returns once the packet is on air and the radio is back
    // to listening, so a send is complete by the time this returns.
    bool startSendRaw(const uint8_t* bytes, int len) override {
        _sending = true;
        esp_err_t err = _drv->send(bytes, (size_t)len);
        _sending = false;
        if (err != ESP_OK) ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
        return err == ESP_OK;
    }

    bool isSendComplete() override { return !_sending; }
    void onSendFinished() override {}
    bool isInRecvMode() const override { return !_sending; }
    float getLastRSSI() const override { return _last_rssi; }
};

// ── Clocks ─────────────────────────────────────────────────────────────

class MillisClock : public mesh::MillisecondClock {
public:
    unsigned long getMillis() override { return (unsigned long)(esp_timer_get_time() / 1000); }
};

class HardwareRNG : public mesh::RNG {
public:
    void random(uint8_t* dest, size_t sz) override { esp_fill_random(dest, sz); }
};

// The system clock when something has set it, and otherwise a clock that only
// moves forward from the latest time this node is known to have used.
class ForwardClock : public mesh::RTCClock {
    uint32_t _floor = CLOCK_BASELINE;
    int64_t _floor_at_us = 0;

public:
    uint32_t getCurrentTime() override {
        uint32_t running = _floor + (uint32_t)((esp_timer_get_time() - _floor_at_us) / 1000000);
        time_t now = time(nullptr);
        if (clock_is_set(now) && (uint32_t)now > running) return (uint32_t)now;
        return running;
    }

    // Never backwards: a peer drops an advert older than the last it saw.
    void setCurrentTime(uint32_t t) override {
        if (t > getCurrentTime()) {
            _floor = t;
            _floor_at_us = esp_timer_get_time();
        }
    }
};

// ── The node ───────────────────────────────────────────────────────────

struct Command {
    enum Kind : uint8_t { Direct, Channel, Advert, Rename } kind;
    uint8_t channel;
    uint32_t message_id;
    uint8_t pub_key[PUB_KEY_SIZE];
    char text[MAX_TEXT_LEN + 1];
};

class ThistleMesh : public BaseChatMesh {
    struct Pending {
        bool used;
        uint32_t message_id;
        uint8_t pub_key[PUB_KEY_SIZE];
        char text[MAX_TEXT_LEN + 1];
        uint32_t timestamp;
        uint8_t attempt;
        uint32_t acks[SEND_ATTEMPTS];
        unsigned long deadline;
    };

    const meshcore_callbacks_t* _cb;
    ForwardClock& _clock;
    char _dir[MESHCORE_PATH_MAX];
    Pending _pending[MAX_PENDING];
    unsigned long _save_at = 0;
    bool _dirty = false;

    void path_of(char* out, size_t size, const char* file) const {
        snprintf(out, size, "%s/%s", _dir, file);
    }

    void report_contact(const ContactInfo& c) {
        if (!_cb->on_contact) return;
        meshcore_contact_t out = {};
        memcpy(out.pub_key, c.id.pub_key, PUB_KEY_SIZE);
        copy_str(out.name, sizeof(out.name), c.name);
        out.type = c.type;
        out.path_len = c.out_path_len == OUT_PATH_UNKNOWN ? MESHCORE_PATH_UNKNOWN
                                                           : (uint8_t)(c.out_path_len & 63);
        out.last_advert = c.last_advert_timestamp;
        out.lat_e6 = c.gps_lat;
        out.lon_e6 = c.gps_lon;
        _cb->on_contact(&out);
    }

    void report_delivery(uint32_t id, uint8_t state) {
        if (_cb->on_delivery) _cb->on_delivery(id, state);
    }

    void report_message(uint8_t kind, uint8_t channel, const uint8_t* key, const char* name,
                        uint32_t timestamp, const char* text) {
        messages_received++;
        if (!_cb->on_message) return;
        meshcore_message_t m = {};
        m.kind = kind;
        m.channel = channel;
        if (key) memcpy(m.sender_key, key, PUB_KEY_SIZE);
        copy_str(m.sender_name, sizeof(m.sender_name), name);
        m.timestamp = timestamp;
        copy_str(m.text, sizeof(m.text), text);
        _cb->on_message(&m);
    }

    void mark_dirty() {
        if (!_dirty) {
            _dirty = true;
            _save_at = futureMillis(SAVE_DELAY_MS);
        }
    }

    void transmit(Pending& p) {
        ContactInfo* c = lookupContactByPubKey(p.pub_key, PUB_KEY_SIZE);
        if (!c) {
            ESP_LOGW(TAG, "message %u: contact gone", (unsigned)p.message_id);
            report_delivery(p.message_id, MESHCORE_FAILED);
            p.used = false;
            return;
        }
        uint32_t timeout = 0;
        int rc = sendMessage(*c, p.timestamp, p.attempt, p.text, p.acks[p.attempt], timeout);
        if (rc == MSG_SEND_FAILED) {
            report_delivery(p.message_id, MESHCORE_FAILED);
            p.used = false;
            return;
        }
        ESP_LOGI(TAG, "message %u to %s, attempt %u, %s", (unsigned)p.message_id, c->name,
                 (unsigned)p.attempt + 1, rc == MSG_SEND_SENT_FLOOD ? "flood" : "direct");
        p.deadline = futureMillis(timeout);
        messages_sent++;
    }

    static const char* type_name(uint8_t type) {
        switch (type) {
        case PAYLOAD_TYPE_REQ:       return "request";
        case PAYLOAD_TYPE_RESPONSE:  return "response";
        case PAYLOAD_TYPE_TXT_MSG:   return "message";
        case PAYLOAD_TYPE_ACK:       return "ack";
        case PAYLOAD_TYPE_ADVERT:    return "advert";
        case PAYLOAD_TYPE_GRP_TXT:   return "channel message";
        case PAYLOAD_TYPE_GRP_DATA:  return "channel data";
        case PAYLOAD_TYPE_ANON_REQ:  return "login";
        case PAYLOAD_TYPE_PATH:      return "path";
        case PAYLOAD_TYPE_TRACE:     return "trace";
        case PAYLOAD_TYPE_MULTIPART: return "multipart";
        case PAYLOAD_TYPE_CONTROL:   return "control";
        default:                     return "raw";
        }
    }

protected:
    // One line per packet on air, so a link to another node can be checked
    // from the console without a second radio.
    void logRx(mesh::Packet* pkt, int len, float score) override {
        ESP_LOGI(TAG, "rx %s, %s, %d bytes, %d dBm", type_name(pkt->getPayloadType()),
                 pkt->isRouteDirect() ? "direct" : "flood", len, (int)_radio->getLastRSSI());
    }

    void logTx(mesh::Packet* pkt, int len) override {
        ESP_LOGI(TAG, "tx %s, %s, %d bytes", type_name(pkt->getPayloadType()),
                 pkt->isRouteDirect() ? "direct" : "flood", len);
    }

    void logTxFail(mesh::Packet* pkt, int len) override {
        ESP_LOGW(TAG, "tx %s failed, %d bytes", type_name(pkt->getPayloadType()), len);
    }

    // A client that does not repeat has nothing to gain from waiting before
    // it reads a flood packet; the companion firmware does not wait either.
    int calcRxDelay(float score, uint32_t air_time) const override { return 0; }

    // A full contact list makes room by dropping the peer heard from least
    // recently, rather than ignoring every new one.
    bool shouldOverwriteWhenFull() const override { return true; }

    void onContactOverwrite(const uint8_t* pub_key) override {
        if (_cb->on_contact_removed) _cb->on_contact_removed(pub_key);
    }

    void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len,
                             const uint8_t* path) override {
        // Only contacts that made it into the table can be messaged.
        if (!lookupContactByPubKey(contact.id.pub_key, PUB_KEY_SIZE)) return;
        ESP_LOGI(TAG, "advert from %s", contact.name);
        report_contact(contact);
        mark_dirty();
    }

    void onContactPathUpdated(const ContactInfo& contact) override {
        report_contact(contact);
        mark_dirty();
    }

    ContactInfo* processAck(const uint8_t* data) override {
        uint32_t ack;
        memcpy(&ack, data, sizeof(ack));
        for (auto& p : _pending) {
            if (!p.used) continue;
            for (int i = 0; i <= p.attempt && i < SEND_ATTEMPTS; i++) {
                if (p.acks[i] != ack) continue;
                ESP_LOGI(TAG, "message %u delivered", (unsigned)p.message_id);
                report_delivery(p.message_id, MESHCORE_DELIVERED);
                p.used = false;
                return lookupContactByPubKey(p.pub_key, PUB_KEY_SIZE);
            }
        }
        return checkConnectionsAck(data);
    }

    void onMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp,
                       const char* text) override {
        report_message(MESHCORE_MSG_DIRECT, 0, from.id.pub_key, from.name, sender_timestamp, text);
    }

    // CLI replies from repeaters and room servers; not conversation.
    void onCommandDataRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp,
                           const char* text) override {}

    // Room server posts arrive signed, on behalf of whoever wrote them.
    void onSignedMessageRecv(const ContactInfo& from, mesh::Packet* pkt, uint32_t sender_timestamp,
                             const uint8_t* sender_prefix, const char* text) override {
        report_message(MESHCORE_MSG_DIRECT, 0, from.id.pub_key, from.name, sender_timestamp, text);
    }

    // A channel message carries its sender only as "name: " at the front.
    void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                              uint32_t timestamp, const char* text) override {
        int idx = findChannelIdx(channel);
        char name[MESHCORE_NAME_MAX] = "";
        const char* body = text;
        const char* sep = strstr(text, ": ");
        if (sep && sep > text && (size_t)(sep - text) < sizeof(name)) {
            memcpy(name, text, sep - text);
            name[sep - text] = '\0';
            body = sep + 2;
        }
        report_message(MESHCORE_MSG_CHANNEL, idx < 0 ? 0 : (uint8_t)idx, nullptr, name,
                       timestamp, body);
    }

    uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override {
        return 500 + 16 * airtime;
    }

    uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path_len) const override {
        return 500 + (airtime * 6 + 250) * ((path_len & 63) + 1);
    }

    // Each pending message keeps its own deadline in tick().
    void onSendTimeout() override {}

    uint8_t onContactRequest(const ContactInfo& contact, uint32_t sender_timestamp,
                             const uint8_t* data, uint8_t len, uint8_t* reply) override {
        return 0;
    }

    void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override {}

public:
    char name[MESHCORE_NAME_MAX] = "";
    uint32_t messages_sent = 0;
    uint32_t messages_received = 0;

    ThistleMesh(HalRadio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, ForwardClock& rtc,
                mesh::PacketManager& mgr, mesh::MeshTables& tables, const char* dir,
                const meshcore_callbacks_t* cb)
        : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _cb(cb), _clock(rtc) {
        copy_str(_dir, sizeof(_dir), dir);
        memset(_pending, 0, sizeof(_pending));
    }

    // The node's keypair, made once and kept: a new one would make it a
    // stranger to every peer that knows it.
    bool load_identity() {
        char path[MESHCORE_PATH_MAX + 16];
        path_of(path, sizeof(path), IDENTITY_FILE);
        uint8_t buf[4 + PRV_KEY_SIZE + PUB_KEY_SIZE];
        FILE* f = fopen(path, "rb");
        if (f) {
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            uint32_t magic;
            memcpy(&magic, buf, 4);
            if (n == sizeof(buf) && magic == IDENTITY_MAGIC) {
                self_id.readFrom(buf + 4, PRV_KEY_SIZE + PUB_KEY_SIZE);
                return true;
            }
            ESP_LOGW(TAG, "%s is not an identity; making a new one", path);
        }

        // MeshCore reserves the 00 and FF hashes.
        do {
            self_id = mesh::LocalIdentity(getRNG());
        } while (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF);

        uint32_t magic = IDENTITY_MAGIC;
        memcpy(buf, &magic, 4);
        self_id.writeTo(buf + 4, PRV_KEY_SIZE + PUB_KEY_SIZE);
        f = fopen(path, "wb");
        bool saved = f && fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);
        if (f) fclose(f);
        memset(buf, 0, sizeof(buf));
        if (!saved) {
            ESP_LOGE(TAG, "could not save the new identity to %s", path);
            return false;
        }
        ESP_LOGI(TAG, "new identity saved to %s", path);
        return true;
    }

    // Contacts and their routes, and the latest time this node used.
    struct FileHeader {
        uint32_t magic;
        uint16_t count;
        uint16_t reserved;
        uint32_t clock;
    };
    struct FileRecord {
        uint8_t pub_key[PUB_KEY_SIZE];
        char name[32];
        uint8_t type;
        uint8_t flags;
        uint8_t out_path_len;
        uint8_t reserved;
        uint32_t last_advert_timestamp;
        uint32_t lastmod;
        int32_t gps_lat;
        int32_t gps_lon;
        uint8_t out_path[MAX_PATH_SIZE];
    };

    void load_contacts() {
        char path[MESHCORE_PATH_MAX + 16];
        path_of(path, sizeof(path), CONTACTS_FILE);
        FILE* f = fopen(path, "rb");
        if (!f) return;
        FileHeader h;
        if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != CONTACTS_MAGIC) {
            fclose(f);
            ESP_LOGW(TAG, "%s is not a contact list; starting empty", path);
            return;
        }
        _clock.setCurrentTime(h.clock + 1);
        int loaded = 0;
        FileRecord r;
        for (int i = 0; i < h.count && fread(&r, sizeof(r), 1, f) == 1; i++) {
            ContactInfo c;
            memset(&c, 0, sizeof(c));
            c.id = mesh::Identity(r.pub_key);
            copy_str(c.name, sizeof(c.name), r.name);
            c.type = r.type;
            c.flags = r.flags;
            c.out_path_len = r.out_path_len;
            memcpy(c.out_path, r.out_path, sizeof(c.out_path));
            c.last_advert_timestamp = r.last_advert_timestamp;
            c.lastmod = r.lastmod;
            c.gps_lat = r.gps_lat;
            c.gps_lon = r.gps_lon;
            c.shared_secret_valid = false;
            if (!addContact(c)) break;
            _clock.setCurrentTime(r.lastmod + 1);
            report_contact(c);
            loaded++;
        }
        fclose(f);
        ESP_LOGI(TAG, "%d contacts loaded", loaded);
    }

    void save_contacts() {
        char path[MESHCORE_PATH_MAX + 16];
        char tmp[MESHCORE_PATH_MAX + 20];
        path_of(path, sizeof(path), CONTACTS_FILE);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE* f = fopen(tmp, "wb");
        if (!f) {
            ESP_LOGW(TAG, "could not write %s", tmp);
            return;
        }
        FileHeader h = {CONTACTS_MAGIC, (uint16_t)getNumContacts(), 0, _clock.getCurrentTime()};
        bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
        ContactsIterator it;
        ContactInfo c;
        while (ok && it.hasNext(this, c)) {
            FileRecord r = {};
            memcpy(r.pub_key, c.id.pub_key, PUB_KEY_SIZE);
            copy_str(r.name, sizeof(r.name), c.name);
            r.type = c.type;
            r.flags = c.flags;
            r.out_path_len = c.out_path_len;
            r.last_advert_timestamp = c.last_advert_timestamp;
            r.lastmod = c.lastmod;
            r.gps_lat = c.gps_lat;
            r.gps_lon = c.gps_lon;
            memcpy(r.out_path, c.out_path, sizeof(r.out_path));
            ok = fwrite(&r, sizeof(r), 1, f) == 1;
        }
        fclose(f);
        if (!ok) {
            remove(tmp);
            ESP_LOGW(TAG, "could not write %s", tmp);
            return;
        }
        remove(path);
        if (rename(tmp, path) != 0) ESP_LOGW(TAG, "could not replace %s", path);
    }

    // What this node and its peers have to agree on: a signature verifies,
    // and two identities reach the same shared secret. Kernel crypto gone
    // wrong would otherwise leave the node unable to read anything, with no
    // word as to why. It is also the deepest the receive path goes.
    // Returns what failed, or nullptr.
    const char* crypto_self_check() {
        // AES-128 and HMAC-SHA256 may run on the crypto accelerator, and a
        // peer reads this node only if they are bit-exact: FIPS-197 C.1 and
        // RFC 4231 case 1.
        static const uint8_t aes_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        static const uint8_t aes_in[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                           0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        static const uint8_t aes_out[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                            0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
        uint8_t block[16];
        mesh::Utils::encrypt(aes_key, block, aes_in, sizeof(aes_in));
        if (memcmp(block, aes_out, sizeof(block)) != 0) return "AES-128";

        // FIPS 180-2: SHA-256("abc"). Packet, channel and ack hashes.
        static const uint8_t sha_out[4] = {0xba, 0x78, 0x16, 0xbf};
        uint8_t digest[4];
        mesh::Utils::sha256(digest, sizeof(digest), (const uint8_t*)"abc", 3);
        if (memcmp(digest, sha_out, sizeof(digest)) != 0) return "SHA-256";

        static const uint8_t hmac_key[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                             0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                             0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
        static const uint8_t hmac_out[8] = {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53};
        uint8_t mac[8];
        SHA256 sha;
        sha.resetHMAC(hmac_key, sizeof(hmac_key));
        sha.update("Hi There", 8);
        sha.finalizeHMAC(hmac_key, sizeof(hmac_key), mac, sizeof(mac));
        if (memcmp(mac, hmac_out, sizeof(mac)) != 0) return "HMAC-SHA256";

        static const uint8_t msg[] = "thistle";
        uint8_t sig[SIGNATURE_SIZE];
        self_id.sign(sig, msg, sizeof(msg));
        if (!self_id.verify(sig, msg, sizeof(msg))) return "Ed25519";

        mesh::LocalIdentity other(getRNG());
        uint8_t ours[PUB_KEY_SIZE], theirs[PUB_KEY_SIZE];
        self_id.calcSharedSecret(ours, other);
        other.calcSharedSecret(theirs, self_id);
        bool agree = memcmp(ours, theirs, PUB_KEY_SIZE) == 0;
        memset(ours, 0, sizeof(ours));
        memset(theirs, 0, sizeof(theirs));
        return agree ? nullptr : "X25519";
    }

    void begin_node() {
        BaseChatMesh::begin();
        if (!addChannel(PUBLIC_CHANNEL_NAME, PUBLIC_CHANNEL_PSK)) {
            ESP_LOGE(TAG, "the Public channel key did not decode");
        }
    }

    void advertise(uint32_t delay_ms) {
        mesh::Packet* pkt = createSelfAdvert(name);
        if (!pkt) {
            ESP_LOGW(TAG, "no packet free for an advert");
            return;
        }
        sendFlood(pkt, delay_ms);
        mark_dirty();   // the advert carries this clock; keep it
        ESP_LOGI(TAG, "advertising as %s", name);
    }

    void handle(const Command& cmd) {
        switch (cmd.kind) {
        case Command::Direct: {
            Pending* slot = nullptr;
            for (auto& p : _pending) {
                if (!p.used) { slot = &p; break; }
            }
            if (!slot) {
                ESP_LOGW(TAG, "message %u: too many awaiting delivery", (unsigned)cmd.message_id);
                report_delivery(cmd.message_id, MESHCORE_FAILED);
                return;
            }
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->message_id = cmd.message_id;
            memcpy(slot->pub_key, cmd.pub_key, PUB_KEY_SIZE);
            copy_str(slot->text, sizeof(slot->text), cmd.text);
            slot->timestamp = getRTCClock()->getCurrentTimeUnique();
            transmit(*slot);
            mark_dirty();
            break;
        }
        case Command::Channel: {
            ChannelDetails details;
            if (!getChannel(cmd.channel, details) || details.name[0] == '\0') {
                report_delivery(cmd.message_id, MESHCORE_FAILED);
                return;
            }
            uint32_t now = getRTCClock()->getCurrentTimeUnique();
            bool queued = sendGroupMessage(now, details.channel, name, cmd.text,
                                           (int)strlen(cmd.text));
            if (queued) messages_sent++;
            report_delivery(cmd.message_id, queued ? MESHCORE_SENT : MESHCORE_FAILED);
            mark_dirty();
            break;
        }
        case Command::Advert:
            advertise(0);
            break;
        case Command::Rename:
            copy_str(name, sizeof(name), cmd.text);
            advertise(0);
            break;
        }
    }

    // Retry what went unacknowledged, flooding on the last attempt in case
    // the remembered route has gone, and save once things settle.
    void tick() {
        for (auto& p : _pending) {
            if (!p.used || !millisHasNowPassed(p.deadline)) continue;
            if (p.attempt + 1 >= SEND_ATTEMPTS) {
                ESP_LOGW(TAG, "message %u: no acknowledgement", (unsigned)p.message_id);
                report_delivery(p.message_id, MESHCORE_FAILED);
                p.used = false;
                continue;
            }
            p.attempt++;
            ContactInfo* c = lookupContactByPubKey(p.pub_key, PUB_KEY_SIZE);
            if (c && p.attempt == SEND_ATTEMPTS - 1 && c->out_path_len != OUT_PATH_UNKNOWN) {
                resetPathTo(*c);
            }
            transmit(p);
        }
        if (_dirty && millisHasNowPassed(_save_at)) {
            _dirty = false;
            save_contacts();
        }
    }
};

// ── Task ───────────────────────────────────────────────────────────────

static HalRadio* s_radio;
static MillisClock s_millis;
static HardwareRNG s_rng;
static ForwardClock* s_clock;
static StaticPoolPacketManager* s_packets;
static SimpleMeshTables* s_tables;
static ThistleMesh* s_mesh;
static QueueHandle_t s_commands;
static TaskHandle_t s_task;
static meshcore_config_t s_config;
static const meshcore_callbacks_t* s_callbacks;
static volatile bool s_running;

static void mesh_task(void*) {
    const char* broken = s_mesh->crypto_self_check();
    if (!broken) {
        ESP_LOGI(TAG, "crypto checked; %u bytes of stack to spare",
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    } else {
        ESP_LOGE(TAG, "%s self-check failed: no peer will be able to read this node", broken);
    }
    s_mesh->begin_node();
    s_mesh->load_contacts();

    if (s_config.name[0]) {
        copy_str(s_mesh->name, sizeof(s_mesh->name), s_config.name);
    } else {
        snprintf(s_mesh->name, sizeof(s_mesh->name), "ThistleOS-%02X%02X",
                 s_mesh->self_id.pub_key[0], s_mesh->self_id.pub_key[1]);
    }

    ESP_LOGI(TAG, "on air as %s, key %02x%02x%02x%02x..., %u Hz BW %u Hz SF%u", s_mesh->name,
             s_mesh->self_id.pub_key[0], s_mesh->self_id.pub_key[1], s_mesh->self_id.pub_key[2],
             s_mesh->self_id.pub_key[3], (unsigned)s_config.freq_hz, (unsigned)s_config.bw_hz,
             (unsigned)s_config.sf);
    if (s_callbacks->on_ready) s_callbacks->on_ready(s_mesh->self_id.pub_key, s_mesh->name);
    s_running = true;

    s_mesh->advertise(BOOT_ADVERT_DELAY_MS);

    Command cmd;
    for (;;) {
        while (xQueueReceive(s_commands, &cmd, 0) == pdTRUE) s_mesh->handle(cmd);
        s_mesh->loop();
        s_mesh->tick();
        if (xQueueReceive(s_commands, &cmd, pdMS_TO_TICKS(LOOP_PERIOD_MS)) == pdTRUE) {
            s_mesh->handle(cmd);
        }
    }
}

static esp_err_t post(const Command& cmd) {
    if (!s_running) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_commands, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

// ── C API ──────────────────────────────────────────────────────────────

extern "C" {

esp_err_t meshcore_start(const meshcore_config_t* config, const meshcore_callbacks_t* callbacks) {
    if (!config || !callbacks) return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_ERR_INVALID_STATE;

    const hal_registry_t* reg = hal_get_registry();
    if (!reg || !reg->radio) return ESP_ERR_NOT_FOUND;

    s_config = *config;
    s_callbacks = callbacks;

    if (!s_radio) s_radio = new HalRadio();
    esp_err_t err = s_radio->configure(reg->radio, s_config);
    if (err != ESP_OK) return err;

    if (!s_commands) s_commands = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(Command));
    if (!s_clock) s_clock = new ForwardClock();
    if (!s_packets) s_packets = new StaticPoolPacketManager(PACKET_POOL);
    if (!s_tables) s_tables = new SimpleMeshTables();
    if (!s_mesh) {
        s_mesh = new ThistleMesh(*s_radio, s_millis, s_rng, *s_clock, *s_packets, *s_tables,
                                 s_config.state_dir, s_callbacks);
    }
    if (!s_commands || !s_clock || !s_packets || !s_tables || !s_mesh) return ESP_ERR_NO_MEM;

    if (!s_mesh->load_identity()) return ESP_FAIL;

    if (xTaskCreate(mesh_task, "meshcore", TASK_STACK, nullptr, TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool meshcore_running(void) { return s_running; }

esp_err_t meshcore_send_direct(const uint8_t* pub_key, const char* text, uint32_t message_id) {
    if (!pub_key || !text) return ESP_ERR_INVALID_ARG;
    if (strlen(text) > MAX_TEXT_LEN) return ESP_ERR_INVALID_SIZE;
    Command cmd = {};
    cmd.kind = Command::Direct;
    cmd.message_id = message_id;
    memcpy(cmd.pub_key, pub_key, PUB_KEY_SIZE);
    copy_str(cmd.text, sizeof(cmd.text), text);
    return post(cmd);
}

esp_err_t meshcore_send_channel(uint8_t channel, const char* text, uint32_t message_id) {
    if (!text) return ESP_ERR_INVALID_ARG;
    if (strlen(text) > MAX_TEXT_LEN) return ESP_ERR_INVALID_SIZE;
    Command cmd = {};
    cmd.kind = Command::Channel;
    cmd.channel = channel;
    cmd.message_id = message_id;
    copy_str(cmd.text, sizeof(cmd.text), text);
    return post(cmd);
}

esp_err_t meshcore_send_advert(void) {
    Command cmd = {};
    cmd.kind = Command::Advert;
    return post(cmd);
}

esp_err_t meshcore_set_name(const char* name) {
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    Command cmd = {};
    cmd.kind = Command::Rename;
    copy_str(cmd.text, MESHCORE_NAME_MAX, name);
    return post(cmd);
}

esp_err_t meshcore_get_stats(meshcore_stats_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_running) return ESP_ERR_INVALID_STATE;
    out->packets_sent = s_mesh->getNumSentFlood() + s_mesh->getNumSentDirect();
    out->packets_received = s_radio->received;
    out->messages_sent = s_mesh->messages_sent;
    out->messages_received = s_mesh->messages_received;
    out->rx_dropped = s_radio->dropped;
    return ESP_OK;
}

}  // extern "C"
