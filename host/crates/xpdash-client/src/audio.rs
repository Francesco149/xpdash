//! Audio output management using CPAL and ringbuf with volume scaling.

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;
use parking_lot::Mutex;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use ringbuf::traits::{Consumer, Producer, Split};
use ringbuf::HeapRb;

pub type RawAudioProducer = ringbuf::wrap::caching::Caching<Arc<HeapRb<f32>>, true, false>;
pub type SharedAudioProducer = Arc<Mutex<RawAudioProducer>>;

pub struct AudioController {
    _stream: Option<cpal::Stream>,
    producer: SharedAudioProducer,
    volume_bits: Arc<AtomicU32>,
    muted: Arc<AtomicBool>,
}

impl AudioController {
    pub fn new() -> Self {
        // 48 kHz stereo = 96,000 f32 samples/sec.
        // 200ms buffer capacity = 19,200 samples — absorbs network burst without overflow.
        let rb = HeapRb::<f32>::new(19200);
        let (producer, consumer) = rb.split();

        let volume_bits = Arc::new(AtomicU32::new(1.0f32.to_bits()));
        let muted = Arc::new(AtomicBool::new(false));

        let vol_clone = volume_bits.clone();
        let muted_clone = muted.clone();

        let stream = match init_cpal_stream(consumer, vol_clone, muted_clone) {
            Ok(s) => {
                log::info!("cpal low-latency audio output initialized successfully (48kHz stereo).");
                Some(s)
            }
            Err(e) => {
                log::warn!("Could not initialize cpal audio output device ({}); running audio headless.", e);
                None
            }
        };

        Self {
            _stream: stream,
            producer: Arc::new(Mutex::new(producer)),
            volume_bits,
            muted,
        }
    }

    pub fn producer(&self) -> SharedAudioProducer {
        self.producer.clone()
    }

    pub fn set_volume(&self, vol: f32) {
        let clamped = vol.clamp(0.0, 2.0);
        self.volume_bits.store(clamped.to_bits(), Ordering::Relaxed);
    }

    pub fn volume(&self) -> f32 {
        f32::from_bits(self.volume_bits.load(Ordering::Relaxed))
    }

    pub fn set_muted(&self, muted: bool) {
        self.muted.store(muted, Ordering::Relaxed);
    }

    pub fn is_muted(&self) -> bool {
        self.muted.load(Ordering::Relaxed)
    }
}

/// Helper to decode 16-bit signed LE PCM bytes to f32 samples and push to ring buffer
pub fn push_pcm16_samples<P: Producer<Item = f32>>(producer: &mut P, pcm_bytes: &[u8]) {
    let mut i = 0;
    while i + 2 <= pcm_bytes.len() {
        let sample_i16 = i16::from_le_bytes([pcm_bytes[i], pcm_bytes[i + 1]]);
        let sample_f32 = (sample_i16 as f32) / 32768.0;
        let _ = producer.try_push(sample_f32);
        i += 2;
    }
}

fn init_cpal_stream<C: Consumer<Item = f32> + Send + 'static>(
    mut consumer: C,
    volume_bits: Arc<AtomicU32>,
    muted: Arc<AtomicBool>,
) -> Result<cpal::Stream, Box<dyn std::error::Error>> {
    let host = cpal::default_host();
    let device = host
        .default_output_device()
        .ok_or_else(|| "No default audio output device found")?;

    let config = cpal::StreamConfig {
        channels: 2,
        sample_rate: cpal::SampleRate(48000),
        buffer_size: cpal::BufferSize::Fixed(480), // 10ms buffer size
    };

    let mut last_sample: f32 = 0.0;
    let mut underrun_count: u32 = 0;
    let stream = device.build_output_stream(
        &config,
        move |data: &mut [f32], _: &cpal::OutputCallbackInfo| {
            let is_muted = muted.load(Ordering::Relaxed);
            let vol = f32::from_bits(volume_bits.load(Ordering::Relaxed));
            for sample in data.iter_mut() {
                if let Some(s) = consumer.try_pop() {
                    last_sample = s;
                    underrun_count = 0;
                } else {
                    // Sample-hold with fade-to-zero: repeat last sample for 1ms (48 samples),
                    // then fade to zero over 1ms to avoid click artifacts.
                    underrun_count += 1;
                    if underrun_count > 96 {
                        last_sample = 0.0;
                    } else if underrun_count > 48 {
                        last_sample *= 0.95;
                    }
                }
                *sample = if is_muted { 0.0 } else { last_sample * vol };
            }
        },
        move |err| {
            log::error!("CPAL audio stream error: {}", err);
        },
        None,
    )?;

    stream.play()?;
    Ok(stream)
}
