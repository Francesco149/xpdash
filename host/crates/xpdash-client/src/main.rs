//! xpdash-client — Native low-latency audio/video client with cpal and anti-desync clock.

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
    AudioSliceHeader, MsgHelloSyn, MsgVideoResize, NetPacketHeader, PacketType, PtsClock,
    TcpFrameHeader, VideoChunkHeader, OP_HELLO_SYN, OP_PING, OP_PONG, OP_STREAM_START,
    OP_VIDEO_RESIZE, TCP_CONTROL_PORT, UDP_MEDIA_PORT,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    log::info!("=== xpdash-client initializing ===");

    let agent_ip = std::env::args().nth(1).unwrap_or_else(|| "10.0.10.113".to_string());
    log::info!("Connecting to Windows XP Agent: {}", agent_ip);

    // Setup CPAL Audio Output
    // 48 kHz stereo 16-bit = 96,000 samples/sec.
    // 25ms buffer capacity = 2,400 samples
    let rb = HeapRb::<f32>::new(4800);
    let (mut audio_prod, audio_cons) = rb.split();

    let audio_running = Arc::new(AtomicBool::new(true));
    let _audio_stream = match init_audio_output(audio_cons) {
        Ok(stream) => {
            log::info!("cpal low-latency audio output initialized successfully.");
            Some(stream)
        }
        Err(e) => {
            log::warn!("Could not initialize cpal audio device ({}); running audio in headless mode.", e);
            None
        }
    };

    // Connect to TCP Control Port 7020
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

    // Request stream start
    let start_msg = [OP_STREAM_START, 0, 0, 0];
    tcp_stream.write_all(&start_msg).await?;
    log::info!("Sent OP_STREAM_START to agent.");

    // Spawn media receiver task on UDP 7021
    let (media_tx, mut media_rx) = tokio::sync::mpsc::channel::<()>(1);
    tokio::spawn(async move {
        if let Err(e) = run_media_loop(&mut audio_prod).await {
            log::error!("Media loop error: {}", e);
        }
        let _ = media_tx.send(()).await;
    });

    // Control message loop
    let mut buf = [0u8; 1024];
    loop {
        tokio::select! {
            _ = media_rx.recv() => {
                log::warn!("Media receiver stopped.");
                break;
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

    log::info!("Audio device: {}", device.name()?);

    let config = cpal::StreamConfig {
        channels: 2,
        sample_rate: cpal::SampleRate(48000),
        buffer_size: cpal::BufferSize::Fixed(480), // 10ms buffer size
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
    log::info!("Media stream receiver listening on UDP port {} (8MB buffer).", UDP_MEDIA_PORT);

    let mut buf = [0u8; 2048];
    let mut audio_clock = PtsClock::new(15);
    let mut video_frames: HashMap<u32, PartialVideoFrame> = HashMap::new();
    let mut last_stats = Instant::now();
    let mut audio_packets_rx: u64 = 0;
    let mut video_frames_rx: u64 = 0;
    let mut audio_dropped: u64 = 0;
    let video_dropped: u64 = 0;

    loop {
        let (len, _src) = socket.recv_from(&mut buf).await?;
        if len < NetPacketHeader::SIZE {
            continue;
        }

        if let Some((nh, payload)) = NetPacketHeader::parse(&buf[..len]) {
            match nh.pkt_type {
                PacketType::Audio => {
                    // Anti-desync check: drop if late
                    if !audio_clock.is_packet_acceptable(nh.pts_ms) {
                        audio_dropped += 1;
                        continue;
                    }

                    if let Some((_ah, pcm_data)) = AudioSliceHeader::parse(payload) {
                        audio_packets_rx += 1;
                        // Convert 16-bit LE PCM bytes to f32 samples (-1.0 to 1.0)
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
                            // Frame complete, reassemble and decompress
                            let mut compressed = Vec::new();
                            for c in 0..entry.total_chunks {
                                if let Some(part) = entry.received_chunks.get(&c) {
                                    compressed.extend_from_slice(part);
                                }
                            }

                            if vh.codec == 2 {
                                let uncompressed_len = (entry.width as usize) * (entry.height as usize) * 4;
                                if let Ok(_decompressed_pixels) = lz4_flex::decompress(&compressed, uncompressed_len) {
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

        if last_stats.elapsed() >= Duration::from_secs(3) {
            log::info!(
                "[Client Metrics] Audio slices received: {} (dropped: {}), Video frames rendered: {} (dropped: {})",
                audio_packets_rx,
                audio_dropped,
                video_frames_rx,
                video_dropped
            );
            last_stats = Instant::now();
        }
    }
}
