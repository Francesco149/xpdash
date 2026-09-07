//! xpdash-client — Native low-latency audio/video client with eframe GUI and anti-desync clock.

mod app;
mod audio;
mod network;
mod ui;

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use ringbuf::traits::{Consumer, Producer, Split};
use ringbuf::HeapRb;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpStream, UdpSocket};

use xpdash_core::{
    AudioSliceHeader, MsgHelloSyn, MsgInputEvent, MsgVideoResize, NetPacketHeader, PacketType,
    PtsClock, TcpFrameHeader, VideoChunkHeader, INPUT_TYPE_MOUSE_REL, OP_HELLO_SYN,
    OP_INPUT_EVENT, OP_PING, OP_PONG, OP_STREAM_START, OP_VIDEO_RESIZE, TCP_CONTROL_PORT,
    UDP_MEDIA_PORT,
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    log::info!("=== xpdash-client initializing ===");

    let args: Vec<String> = std::env::args().collect();
    let is_headless = args.iter().any(|a| a == "--headless")
        || std::env::var("XPDASH_HEADLESS").map(|v| v == "1").unwrap_or(false)
        || std::env::var("XPDASH_SOAK_SECONDS").is_ok();

    if is_headless {
        let rt = tokio::runtime::Runtime::new()?;
        rt.block_on(run_headless_mode(args))
    } else {
        // Native GUI Mode with eframe
        let target_arg = args.get(1).and_then(|a| {
            if a.starts_with("--") {
                None
            } else {
                Some(a.clone())
            }
        });

        let native_options = eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default()
                .with_title("xpdash — Windows XP Remote Console")
                .with_inner_size([1024.0, 768.0])
                .with_min_inner_size([640.0, 480.0])
                .with_active(true),
            vsync: false, // Decouple paint rate from host monitor — present frames immediately
            ..Default::default()
        };

        // Create multithreaded Tokio runtime for background discovery & streaming tasks
        let _rt = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()?;
        let _guard = _rt.enter();

        log::info!("Launching eframe native GUI window...");
        eframe::run_native(
            "xpdash-client",
            native_options,
            Box::new(move |cc| Ok(Box::new(app::XpDashApp::new(cc, target_arg)))),
        )?;

        Ok(())
    }
}

async fn run_headless_mode(args: Vec<String>) -> Result<(), Box<dyn std::error::Error>> {
    log::info!("Starting in headless stream mode...");
    let agent_ip = args
        .iter()
        .find(|a| !a.starts_with("--") && a.as_str() != args[0])
        .cloned()
        .unwrap_or_else(|| "10.0.10.113".to_string());

    log::info!("Connecting to Windows XP Agent: {}", agent_ip);

    let rb = HeapRb::<f32>::new(4800);
    let (mut audio_prod, audio_cons) = rb.split();

    let audio_running = Arc::new(AtomicBool::new(true));
    let _audio_stream = match init_audio_output(audio_cons) {
        Ok(stream) => {
            log::info!("cpal low-latency audio output initialized successfully.");
            Some(stream)
        }
        Err(e) => {
            log::warn!("Could not initialize cpal audio device ({}); running audio headless.", e);
            None
        }
    };

    let agent_addr: SocketAddr = format!("{}:{}", agent_ip, TCP_CONTROL_PORT).parse()?;
    let mut tcp_stream = match TcpStream::connect(agent_addr).await {
        Ok(s) => {
            log::info!("Connected to agent control port {}.", agent_addr);
            s
        }
        Err(e) => {
            log::error!("Failed to connect to agent TCP port {}: {}", agent_addr, e);
            return Err(e.into());
        }
    };

    let start_msg = [OP_STREAM_START, 0, 0, 0];
    tcp_stream.write_all(&start_msg).await?;
    log::info!("Sent OP_STREAM_START to agent.");

    let (media_tx, mut media_rx) = tokio::sync::mpsc::channel::<()>(1);
    tokio::spawn(async move {
        if let Err(e) = run_media_loop(&mut audio_prod).await {
            log::error!("Media loop error: {}", e);
        }
        let _ = media_tx.send(()).await;
    });

    let soak_duration_sec: u64 = std::env::var("XPDASH_SOAK_SECONDS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let inject_mouse = std::env::var("XPDASH_INJECT_MOUSE").map(|v| v == "1").unwrap_or(false);

    if soak_duration_sec > 0 {
        log::info!("Soak test mode: running for {} seconds ({} minutes)", soak_duration_sec, soak_duration_sec / 60);
    }
    if inject_mouse {
        log::info!("Side-to-side mouse injection enabled (50ms interval, alternating dx=+35/-35)");
    }

    let soak_start = Instant::now();
    let mut mouse_interval = tokio::time::interval(Duration::from_millis(50));
    let mut mouse_tick = 0u64;

    let mut ping_interval = tokio::time::interval(Duration::from_secs(1));
    let mut last_ping_sent: Option<Instant> = None;
    let mut buf = [0u8; 1024];

    loop {
        tokio::select! {
            _ = media_rx.recv() => {
                log::warn!("Media receiver stopped.");
                break;
            }
            _ = ping_interval.tick() => {
                last_ping_sent = Some(Instant::now());
                let ping_frame = [OP_PING, 0, 0, 0];
                let _ = tcp_stream.write_all(&ping_frame).await;
                if soak_duration_sec > 0 && soak_start.elapsed().as_secs() >= soak_duration_sec {
                    log::info!("Soak test duration reached ({}s). Cleanly terminating stream.", soak_duration_sec);
                    break;
                }
            }
            _ = mouse_interval.tick(), if inject_mouse => {
                mouse_tick += 1;
                let dx: i16 = if (mouse_tick % 40) < 20 { 35 } else { -35 };
                let ev = MsgInputEvent {
                    event_type: INPUT_TYPE_MOUSE_REL,
                    param1: 0,
                    param2: dx,
                    param3: 0,
                    key_down: 0,
                };
                let mut ev_buf = [0u8; MsgInputEvent::SIZE];
                if ev.encode(&mut ev_buf) {
                    let mut frame_buf = [0u8; TcpFrameHeader::SIZE + MsgInputEvent::SIZE];
                    let fhdr = TcpFrameHeader {
                        opcode: OP_INPUT_EVENT,
                        reserved: 0,
                        payload_len: MsgInputEvent::SIZE as u16,
                    };
                    fhdr.encode(&mut frame_buf[..TcpFrameHeader::SIZE]);
                    frame_buf[TcpFrameHeader::SIZE..].copy_from_slice(&ev_buf);
                    let _ = tcp_stream.write_all(&frame_buf).await;
                }
            }
            res = tcp_stream.read(&mut buf) => {
                let n = match res {
                    Ok(n) if n > 0 => n,
                    _ => {
                        log::warn!("TCP connection closed by agent.");
                        break;
                    }
                };

                let mut offset = 0;
                while offset + TcpFrameHeader::SIZE <= n {
                    if let Some(hdr) = TcpFrameHeader::parse(&buf[offset..]) {
                        let payload_start = offset + TcpFrameHeader::SIZE;
                        let payload_end = payload_start + hdr.payload_len as usize;

                        if payload_end <= n {
                            let payload = &buf[payload_start..payload_end];
                            match hdr.opcode {
                                OP_HELLO_SYN => {
                                    if let Some(syn) = MsgHelloSyn::parse(payload) {
                                        log::info!(
                                            "[Agent Connected] Name='{}' Screen={}x{}@{}bpp AgentVer={}",
                                            syn.machine_name, syn.screen_width, syn.screen_height, syn.bpp, syn.agent_version
                                        );
                                    }
                                }
                                OP_VIDEO_RESIZE => {
                                    if let Some(res) = MsgVideoResize::parse(payload) {
                                        log::info!("[Display Change] Agent resized to {}x{}@{}bpp", res.new_width, res.new_height, res.new_bpp);
                                    }
                                }
                                OP_PONG => {
                                    if let Some(sent) = last_ping_sent.take() {
                                        let rtt = sent.elapsed().as_secs_f64() * 1000.0;
                                        log::info!("[Latency RTT] Control Round-Trip Time: {:.2} ms", rtt);
                                    }
                                }
                                OP_PING => {
                                    let pong = [OP_PONG, 0, 0, 0];
                                    let _ = tcp_stream.write_all(&pong).await;
                                }
                                _ => {}
                            }
                            offset = payload_end;
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }
    }

    audio_running.store(false, Ordering::SeqCst);
    Ok(())
}
fn init_audio_output<C: Consumer<Item = f32> + Send + 'static>(
    mut consumer: C,
) -> Result<cpal::Stream, Box<dyn std::error::Error>> {
    let host = cpal::default_host();
    let device = host
        .default_output_device()
        .ok_or_else(|| "No default audio output device found")?;

    let config = cpal::StreamConfig {
        channels: 2,
        sample_rate: cpal::SampleRate(48000),
        buffer_size: cpal::BufferSize::Fixed(480),
    };

    let stream = device.build_output_stream(
        &config,
        move |data: &mut [f32], _: &cpal::OutputCallbackInfo| {
            for sample in data.iter_mut() {
                *sample = consumer.try_pop().unwrap_or(0.0);
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

struct PartialVideoFrame {
    width: u16,
    height: u16,
    total_chunks: u16,
    received_chunks: HashMap<u16, Vec<u8>>,
}

async fn run_media_loop<P: Producer<Item = f32>>(
    audio_prod: &mut P,
) -> Result<(), Box<dyn std::error::Error>> {
    let std_sock = std::net::UdpSocket::bind(format!("0.0.0.0:{}", UDP_MEDIA_PORT))?;
    let sock2 = socket2::SockRef::from(&std_sock);
    let _ = sock2.set_recv_buffer_size(8 * 1024 * 1024);
    std_sock.set_nonblocking(true)?;
    let socket = UdpSocket::from_std(std_sock)?;

    let mut buf = [0u8; 16384];
    let mut audio_clock = PtsClock::new(100);
    let mut video_frames: HashMap<u32, PartialVideoFrame> = HashMap::new();
    let mut last_stats = Instant::now();
    let mut audio_packets_rx: u64 = 0;
    let mut video_frames_rx: u64 = 0;
    let mut audio_dropped: u64 = 0;
    let video_dropped: u64 = 0;
    let mut total_jitter_abs: f64 = 0.0;
    let mut jitter_samples: u64 = 0;
    let mut bytes_in_window: u64 = 0;

    loop {
        let (len, _src) = socket.recv_from(&mut buf).await?;
        if len < NetPacketHeader::SIZE {
            continue;
        }
        bytes_in_window += len as u64;

        if let Some((nh, payload)) = NetPacketHeader::parse(&buf[..len]) {
            match nh.pkt_type {
                PacketType::Audio => {
                    if let Some(j) = audio_clock.jitter_ms(nh.pts_ms) {
                        total_jitter_abs += j.abs() as f64;
                        jitter_samples += 1;
                    }

                    if !audio_clock.is_packet_acceptable(nh.pts_ms) {
                        audio_dropped += 1;
                        continue;
                    }

                    if let Some((_ah, pcm_data)) = AudioSliceHeader::parse(payload) {
                        audio_packets_rx += 1;
                        let mut i = 0;
                        while i + 2 <= pcm_data.len() {
                            let sample_i16 = i16::from_le_bytes([pcm_data[i], pcm_data[i + 1]]);
                            let sample_f32 = (sample_i16 as f32) / 32768.0;
                            let _ = audio_prod.try_push(sample_f32);
                            i += 2;
                        }
                    }
                }
                PacketType::Video => {
                    if let Some((vh, chunk_data)) = VideoChunkHeader::parse(payload) {
                        let entry = video_frames.entry(vh.frame_index).or_insert_with(|| PartialVideoFrame {
                            width: vh.frame_width,
                            height: vh.frame_height,
                            total_chunks: vh.total_chunks,
                            received_chunks: HashMap::new(),
                        });

                        entry.received_chunks.insert(vh.chunk_index, chunk_data.to_vec());

                        if entry.received_chunks.len() == entry.total_chunks as usize {
                            let mut compressed = Vec::new();
                            for c in 0..entry.total_chunks {
                                if let Some(part) = entry.received_chunks.get(&c) {
                                    compressed.extend_from_slice(part);
                                }
                            }

                            if vh.codec == 1 {
                                // JPEG - just count, no decode in headless mode
                                video_frames_rx += 1;
                            } else if vh.codec == 2 {
                                let uncompressed_len = (entry.width as usize) * (entry.height as usize) * 4;
                                if let Ok(_decompressed) = lz4_flex::decompress(&compressed, uncompressed_len) {
                                    video_frames_rx += 1;
                                }
                            } else {
                                video_frames_rx += 1;
                            }

                            video_frames.remove(&vh.frame_index);
                        }
                    }
                }
                PacketType::Ping => {}
            }
        }

        if video_frames.len() > 100 {
            video_frames.clear();
        }

        if last_stats.elapsed() >= Duration::from_secs(2) {
            let elapsed = last_stats.elapsed().as_secs_f64();
            let kbps = (bytes_in_window as f64 * 8.0) / (elapsed * 1000.0);
            let avg_jitter = if jitter_samples > 0 { total_jitter_abs / jitter_samples as f64 } else { 0.0 };
            log::info!(
                "[Media Stream] Video: {} frames ({} dropped) | Audio: {} slices ({:.1} kbps, jitter: ±{:.2}ms, {} dropped)",
                video_frames_rx, video_dropped, audio_packets_rx, kbps, avg_jitter, audio_dropped
            );
            bytes_in_window = 0;
            total_jitter_abs = 0.0;
            jitter_samples = 0;
            last_stats = Instant::now();
        }
    }
}
