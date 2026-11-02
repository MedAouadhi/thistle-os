// SPDX-License-Identifier: BSD-3-Clause
//! A tailnet call's task: the ring, then the audio (protocol.md section
//! 4.8, `ws-opus`). The signalling, the call's state and its rules are
//! tailnet_chat.rs' (with tailnet_call_policy.rs); this is what runs while a
//! call is ringing here, being placed or connected.
//!
//! A FreeRTOS task made for the call and gone after (kernel_task), its
//! stack in internal RAM: libopus pushes its arrays on its pseudostack, in
//! PSRAM, and still used 14.4 KB of the C stack on top (ticket 01), so the
//! task has 20 KB and logs how much it never used. While it runs the device
//! is held awake, the microphone is taken as dictation takes it
//! (`rs_audio_capture_begin`, so a player pauses and sets its own format
//! again after) and `rs_call_in_progress()` is true.
//!
//! Ringing here: the call manager's ringtone at the system volume, unless
//! the system is muted or the user silenced it, and the motor every 2 s.
//! Connected, every 20 ms: 20 ms from the microphone (turned down while the
//! far end is loud, silence while muted), encoded and handed to the chat
//! module, which writes it to the call's connection and reads what came
//! (`tailnet_chat::pump_call`); then the next frame from the jitter buffer,
//! decoded or concealed, to the speaker at the call's volume. The codec is
//! 16 kHz mono 16-bit for the call, and is stopped after.

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};

use crate::hal_registry::{registry, HalAudioConfig, HalAudioDriver, HAL_HAPTIC_MESSAGE, HAL_HAPTIC_RING};
use crate::power_manager::HoldGuard;
use crate::tailnet_call_media::{self as media, Ducker, Jitter, Pull, FRAME_SAMPLES};
use crate::tailnet_call_policy::Phase;

/// Ticket 01: libopus used 14.4 KB of the calling task's stack.
const STACK_SIZE: usize = 20 * 1024;
const FRAME: Duration = Duration::from_millis(20);
const RATE: u32 = 16_000;

#[cfg(not(test))]
extern "C" {
    fn esp_log_write(level: i32, tag: *const u8, format: *const u8, ...);
}

const ESP_LOG_WARN: i32 = 2;
const ESP_LOG_INFO: i32 = 3;

fn log(level: i32, text: &str) {
    #[cfg(not(test))]
    {
        let c = std::ffi::CString::new(text.replace('\0', "")).unwrap_or_default();
        unsafe { esp_log_write(level, b"tailnet_call\0".as_ptr(), b"%s\n\0".as_ptr(), c.as_ptr()) };
    }
    #[cfg(test)]
    let _ = (level, text);
}

pub(crate) fn info(text: &str) {
    log(ESP_LOG_INFO, text);
}

pub(crate) fn warn(text: &str) {
    log(ESP_LOG_WARN, text);
}

// ---------------------------------------------------------------------------
// Opus: components/opus, on the S3 and in the simulator
// ---------------------------------------------------------------------------

#[cfg(all(not(test), any(not(target_os = "espidf"), all(target_arch = "xtensa", not(feature = "esp32"), not(feature = "esp32s2")))))]
mod codec {
    use std::ffi::c_void;

    extern "C" {
        fn thistle_opus_encoder_new(bitrate: i32) -> *mut c_void;
        fn thistle_opus_encoder_free(enc: *mut c_void);
        fn thistle_opus_decoder_new() -> *mut c_void;
        fn thistle_opus_decoder_free(dec: *mut c_void);
        fn opus_encode(enc: *mut c_void, pcm: *const i16, frame: i32, data: *mut u8, max: i32) -> i32;
        fn opus_decode(dec: *mut c_void, data: *const u8, len: i32, pcm: *mut i16, frame: i32, fec: i32) -> i32;
        fn thistle_opus_scratch_used() -> usize;
    }

    pub struct Codec {
        enc: *mut c_void,
        dec: *mut c_void,
    }

    impl Codec {
        pub fn new() -> Option<Codec> {
            let enc = unsafe { thistle_opus_encoder_new(24_000) };
            let dec = unsafe { thistle_opus_decoder_new() };
            if enc.is_null() || dec.is_null() {
                unsafe {
                    if !enc.is_null() {
                        thistle_opus_encoder_free(enc);
                    }
                    if !dec.is_null() {
                        thistle_opus_decoder_free(dec);
                    }
                }
                return None;
            }
            Some(Codec { enc, dec })
        }

        pub fn encode(&mut self, pcm: &[i16], out: &mut [u8]) -> usize {
            let n = unsafe { opus_encode(self.enc, pcm.as_ptr(), pcm.len() as i32, out.as_mut_ptr(), out.len() as i32) };
            n.max(0) as usize
        }

        /// `None` conceals a lost frame.
        pub fn decode(&mut self, packet: Option<&[u8]>, pcm: &mut [i16]) -> bool {
            let (p, n) = packet.map_or((std::ptr::null(), 0), |p| (p.as_ptr(), p.len() as i32));
            unsafe { opus_decode(self.dec, p, n, pcm.as_mut_ptr(), pcm.len() as i32, 0) == pcm.len() as i32 }
        }

        pub fn scratch_used() -> usize {
            unsafe { thistle_opus_scratch_used() }
        }
    }

    impl Drop for Codec {
        fn drop(&mut self) {
            unsafe {
                thistle_opus_encoder_free(self.enc);
                thistle_opus_decoder_free(self.dec);
            }
        }
    }
}

#[cfg(not(all(not(test), any(not(target_os = "espidf"), all(target_arch = "xtensa", not(feature = "esp32"), not(feature = "esp32s2"))))))]
mod codec {
    /// No libopus on this chip (or in the host tests): no call connects.
    pub struct Codec;

    impl Codec {
        pub fn new() -> Option<Codec> {
            None
        }
        pub fn encode(&mut self, _pcm: &[i16], _out: &mut [u8]) -> usize {
            0
        }
        pub fn decode(&mut self, _packet: Option<&[u8]>, _pcm: &mut [i16]) -> bool {
            false
        }
        pub fn scratch_used() -> usize {
            0
        }
    }
}

use codec::Codec;

/// Whether this build can carry a call's audio.
pub(crate) fn supported() -> bool {
    cfg!(any(not(target_os = "espidf"), all(target_arch = "xtensa", not(feature = "esp32"), not(feature = "esp32s2"))))
}

// ---------------------------------------------------------------------------
// What the chat module and the UI share with the task
// ---------------------------------------------------------------------------

struct Media {
    /// The call the task runs for.
    call: Option<String>,
    jitter: Jitter,
}

static MEDIA: Mutex<Media> = Mutex::new(Media { call: None, jitter: Jitter::empty() });
/// The task runs.
static RUNNING: AtomicBool = AtomicBool::new(false);
static MUTED: AtomicBool = AtomicBool::new(false);
static SILENCED: AtomicBool = AtomicBool::new(false);
static VOLUME: AtomicU8 = AtomicU8::new(crate::call_manager::DEFAULT_VOLUME);

fn media() -> MutexGuard<'static, Media> {
    MEDIA.lock().unwrap_or_else(PoisonError::into_inner)
}

/// An audio frame of call `call` came: into the jitter buffer.
pub(crate) fn audio_in(call: &str, seq: u32, payload: &[u8]) {
    let mut m = media();
    if m.call.as_deref() == Some(call) {
        m.jitter.push(seq, payload.to_vec());
    }
}

pub(crate) fn set_muted(on: bool) {
    MUTED.store(on, Ordering::Relaxed);
}

pub(crate) fn muted() -> bool {
    MUTED.load(Ordering::Relaxed)
}

pub(crate) fn set_volume(percent: u8) {
    VOLUME.store(percent.min(100), Ordering::Relaxed);
}

pub(crate) fn volume() -> u8 {
    VOLUME.load(Ordering::Relaxed)
}

/// Stop the ringing of the call ringing here; it goes on ringing for the
/// caller.
pub(crate) fn silence() {
    SILENCED.store(true, Ordering::Relaxed);
}

/// Whether the task runs: the call has the audio.
pub(crate) fn running() -> bool {
    RUNNING.load(Ordering::SeqCst)
}

/// Start the task for call `call`, which rings here, is being placed or is
/// connected. The policy allows one live call, so a task still running is
/// finishing the last one: it takes this one on after. False if the task
/// could not be made.
pub(crate) fn start(call: &str) -> bool {
    MUTED.store(false, Ordering::Relaxed);
    SILENCED.store(false, Ordering::Relaxed);
    crate::call_manager::load_call_volume();
    {
        // RUNNING changes only under this lock, so a task finishing either
        // sees the new call or has gone before it is set.
        let mut m = media();
        m.call = Some(call.to_string());
        m.jitter = Jitter::call();
        if RUNNING.swap(true, Ordering::SeqCst) {
            return true;
        }
    }
    let id = call.to_string();
    if !crate::kernel_task::spawn(c"call", STACK_SIZE, crate::kernel_task::PRIORITY_DEFAULT + 2, move || run(id)) {
        let mut m = media();
        m.call = None;
        RUNNING.store(false, Ordering::SeqCst);
        warn("could not start the call task");
        return false;
    }
    true
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------

fn audio() -> Option<&'static HalAudioDriver> {
    unsafe { registry().audio.as_ref() }
}

fn as_bytes(pcm: &[i16]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(pcm.as_ptr() as *const u8, pcm.len() * 2) }
}

fn buzz() {
    if crate::hal_registry::haptic_play(HAL_HAPTIC_RING) == 0x102 {
        crate::hal_registry::haptic_play(HAL_HAPTIC_MESSAGE);
    }
}

struct Task {
    id: String,
    configured: bool,
    codec: Option<Codec>,
    ducker: Ducker,
    seq: u32,
    sent: u32,
    played: u32,
    /// The ring: when its next step is due, and which.
    ring_step: usize,
    ring_due: Instant,
    ring_sounding: bool,
    tone: Vec<u8>,
    gap: Vec<u8>,
    connected_since: Option<Instant>,
    mic_failed: bool,
}

/// Two short rings and a pause, two seconds in all, as the call manager
/// rings: (tone, milliseconds). The motor buzzes at the start of each.
const CADENCE: [(bool, u64); 4] = [(true, 400), (false, 200), (true, 400), (false, 1000)];

impl Task {
    fn configure(&mut self) {
        if self.configured {
            return;
        }
        self.configured = true;
        if let Some(configure) = audio().and_then(|a| a.configure) {
            let cfg = HalAudioConfig { sample_rate: RATE, bits_per_sample: 16, channels: 1 };
            unsafe { configure(&cfg) };
        }
    }

    fn play(&mut self, bytes: &[u8]) -> bool {
        match audio().and_then(|a| a.play) {
            Some(play) => unsafe { play(bytes.as_ptr(), bytes.len()) == 0 },
            None => false,
        }
    }

    fn stop_audio(&mut self) {
        if self.ring_sounding || self.configured {
            if let Some(stop) = audio().and_then(|a| a.stop) {
                unsafe { stop() };
            }
            self.ring_sounding = false;
        }
    }

    /// One step of the ring, if due.
    fn ring(&mut self) {
        let now = Instant::now();
        if now < self.ring_due {
            return;
        }
        let quiet = SILENCED.load(Ordering::Relaxed) || crate::audio_volume::rs_audio_is_muted();
        let (tone, ms) = CADENCE[self.ring_step];
        if tone && !SILENCED.load(Ordering::Relaxed) {
            buzz();
        }
        if !quiet {
            self.configure();
            if self.tone.is_empty() {
                self.tone = crate::call_manager::ring_tone(400);
                self.gap = vec![0; (RATE as usize * 2) / 5];
            }
            if tone {
                let t = std::mem::take(&mut self.tone);
                self.ring_sounding |= self.play(&t);
                self.tone = t;
            } else if ms <= 200 {
                let g = std::mem::take(&mut self.gap);
                self.play(&g[..(RATE as usize / 1000 * 2 * ms as usize).min(g.len())]);
                self.gap = g;
            }
        }
        self.ring_due = now + Duration::from_millis(ms);
        self.ring_step = (self.ring_step + 1) % CADENCE.len();
    }

    /// 20 ms of a connected call.
    fn step(&mut self, mic: &mut [i16; FRAME_SAMPLES], out: &mut [i16; FRAME_SAMPLES], packet: &mut [u8]) {
        if self.connected_since.is_none() {
            self.connected_since = Some(Instant::now());
            self.stop_audio();
            self.configured = false;
            self.configure();
            self.codec = Codec::new();
            if self.codec.is_none() {
                warn("no Opus codec: the call carries no audio");
            }
        }
        let started = Instant::now();
        // The microphone: its read takes the 20 ms.
        let recorded = match audio().and_then(|a| a.record) {
            Some(record) if !self.mic_failed => {
                let bytes = unsafe { std::slice::from_raw_parts_mut(mic.as_mut_ptr() as *mut u8, FRAME_SAMPLES * 2) };
                let ok = unsafe { record(bytes.as_mut_ptr(), bytes.len()) } == 0;
                if !ok {
                    self.mic_failed = true;
                    warn("the microphone failed; sending silence");
                }
                ok
            }
            _ => false,
        };
        if !recorded {
            mic.fill(0);
        }
        if MUTED.load(Ordering::Relaxed) {
            mic.fill(0);
        } else {
            self.ducker.mic(mic);
        }
        let mut frames = Vec::new();
        if let Some(codec) = self.codec.as_mut() {
            let n = codec.encode(mic, packet);
            if n > 0 {
                frames.push((self.seq, packet[..n].to_vec()));
                self.seq = self.seq.wrapping_add(1);
                self.sent += 1;
            }
        }
        crate::tailnet_chat::pump_call(&self.id, frames);

        let pull = media().jitter.pull();
        let have = match (&pull, self.codec.as_mut()) {
            (Pull::Play(p), Some(codec)) => codec.decode(Some(p), out),
            (Pull::Conceal, Some(codec)) => codec.decode(None, out),
            _ => false,
        };
        if !have {
            out.fill(0);
        }
        media::scale(out, media::volume_q8(VOLUME.load(Ordering::Relaxed)));
        self.ducker.played(out);
        if have {
            self.played += 1;
        }
        self.play(as_bytes(out));
        if !recorded {
            // Neither the microphone nor the speaker kept time.
            let spent = started.elapsed();
            if spent < FRAME {
                std::thread::sleep(FRAME - spent);
            }
        }
    }
}

/// One call, from its ring or its dial to its end.
fn run_call(id: &str, packet: &mut [u8], mic: &mut [i16; FRAME_SAMPLES], out: &mut [i16; FRAME_SAMPLES]) {
    let mut t = Task {
        id: id.to_string(),
        configured: false,
        codec: None,
        ducker: Ducker::new(),
        seq: 0,
        sent: 0,
        played: 0,
        ring_step: 0,
        ring_due: Instant::now(),
        ring_sounding: false,
        tone: Vec::new(),
        gap: Vec::new(),
        connected_since: None,
        mic_failed: false,
    };
    loop {
        match crate::tailnet_chat::call_phase(id) {
            Some(Phase::Ringing) => {
                t.ring();
                std::thread::sleep(Duration::from_millis(50));
            }
            Some(Phase::Reaching | Phase::Offered) => std::thread::sleep(Duration::from_millis(50)),
            Some(Phase::Connected) => t.step(mic, out, packet),
            Some(Phase::Over(_)) | None => break,
        }
    }
    t.stop_audio();
    let (concealed, late, dropped) = {
        let m = media();
        (m.jitter.concealed, m.jitter.late, m.jitter.dropped)
    };
    if t.connected_since.is_some() {
        info(&format!(
            "played {} frames, sent {}; {concealed} frames lost or late and concealed ({late} late, {dropped} over 200 ms); \
             microphone turned down for {} frames; Opus scratch {} bytes",
            t.played,
            t.sent,
            t.ducker.ducked,
            Codec::scratch_used()
        ));
    }
}

fn run(first: String) {
    {
        let _awake = HoldGuard::new(b"a tailnet call\0");
        crate::audio_volume::rs_audio_capture_begin();
        // In PSRAM on the device (an allocation over 4 KB would be; this is
        // 1.3 KB, from the heap rather than the stack).
        let mut packet = vec![0u8; 1275];
        let mut mic = [0i16; FRAME_SAMPLES];
        let mut out = [0i16; FRAME_SAMPLES];
        let mut id = first;
        loop {
            run_call(&id, &mut packet, &mut mic, &mut out);
            // A call placed or offered while this one was ending: take it on.
            let mut m = media();
            match m.call.clone() {
                Some(next) if next != id => id = next,
                _ => {
                    m.call = None;
                    m.jitter = Jitter::empty();
                    RUNNING.store(false, Ordering::SeqCst);
                    break;
                }
            }
        }
        crate::audio_volume::rs_audio_capture_end();
        if let Some(unused) = crate::kernel_task::stack_unused() {
            let level = if unused < 2048 { ESP_LOG_WARN } else { ESP_LOG_INFO };
            log(level, &format!("the call task left {unused} of {STACK_SIZE} bytes of stack unused"));
        }
    }
    // SMS that waited for the call may be fetched now.
    crate::net_coordinator::poke();
    crate::tailnet_chat::changed_now();
}
