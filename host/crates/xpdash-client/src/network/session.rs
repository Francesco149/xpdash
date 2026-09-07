//! Active streaming session management: TCP control and UDP media receiver.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use parking_lot::RwLock;
use std::time::{Duration, Instant};

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpStream, UdpSocket};

use xpdash_core::{
    AudioSliceHeader, MsgHelloSyn, MsgInputEvent, MsgVideoResize, NetPacketHeader, PacketType,
    PtsClock, TcpFrameHeader, VideoChunkHeader, OP_HELLO_SYN, OP_INPUT_EVENT, OP_PING, OP_PONG,
    OP_STREAM_START, OP_VIDEO_RESIZE, TCP_CONTROL_PORT, UDP_MEDIA_PORT,
};

use crate::audio::SharedAudioProducer;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StreamState {
    Idle,
    Connecting,
    Connected,
    Disconnected(String),
}

#[derive(Clone)]
pub struct VideoFrame {
    pub width: u16,
    pub height: u16,
    pub frame_index: u32,
    pub rgba_pixels: Arc<Vec<u8>>,
}

#[derive(Debug, Clone, Default)]
pub struct SessionMetrics {
    pub rtt_ms: f64,
    pub fps: f64,
    pub kbps: f64,
    pub audio_jitter_ms: f64,
    pub audio_slices_rx: u64,
    pub audio_slices_dropped: u64,
    pub video_frames_rx: u64,
    pub video_frames_dropped: u64,
    pub screen_width: u16,
    pub screen_height: u16,
    pub machine_name: String,
}

pub struct ClientSession {
    state: Arc<RwLock<StreamState>>,
    latest_frame: Arc<RwLock<Option<VideoFrame>>>,
    metrics: Arc<RwLock<SessionMetrics>>,
    input_tx: tokio::sync::mpsc::UnboundedSender<MsgInputEvent>,
    running: Arc<AtomicBool>,
}

impl ClientSession {
    pub fn connect(agent_ip: String, audio_prod: Option<SharedAudioProducer>) -> Self {
        let state = Arc::new(RwLock::new(StreamState::Connecting));
        let latest_frame = Arc::new(RwLock::new(None));
        let metrics = Arc::new(RwLock::new(SessionMetrics::default()));
        let (input_tx, mut input_rx) = tokio::sync::mpsc::unbounded_channel::<MsgInputEvent>();
        let running = Arc::new(AtomicBool::new(true));

        let state_clone = state.clone();
        let frame_clone = latest_frame.clone();
        let metrics_clone = metrics.clone();
        let running_clone = running.clone();

        tokio::spawn(async move {
            if let Err(e) = run_session(
                agent_ip,
                state_clone.clone(),
                frame_clone,
                metrics_clone,
                &mut input_rx,
                audio_prod,
                running_clone,
            ).await {
                log::warn!("Session ended with error: {}", e);
                *state_clone.write() = StreamState::Disconnected(e.to_string());
            }
        });

        Self {
            state,
            latest_frame,
            metrics,
            input_tx,
            running,
        }
    }

    pub fn state(&self) -> StreamState {
        self.state.read().clone()
    }

    pub fn latest_frame(&self) -> Option<VideoFrame> {
        self.latest_frame.read().clone()
    }

    pub fn metrics(&self) -> SessionMetrics {
        self.metrics.read().clone()
    }

    pub fn send_input(&self, event: MsgInputEvent) {
        let _ = self.input_tx.send(event);
    }

    pub fn disconnect(&self) {
        self.running.store(false, Ordering::SeqCst);
        *self.state.write() = StreamState::Idle;
    }
}

async fn run_session(
    agent_ip: String,
    state: Arc<RwLock<StreamState>>,
    latest_frame: Arc<RwLock<Option<VideoFrame>>>,
    metrics: Arc<RwLock<SessionMetrics>>,
    input_rx: &mut tokio::sync::mpsc::UnboundedReceiver<MsgInputEvent>,
    audio_prod: Option<SharedAudioProducer>,
    running: Arc<AtomicBool>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let agent_addr: SocketAddr = format!("{}:{}", agent_ip, TCP_CONTROL_PORT).parse()?;
    log::info!("Connecting TCP control channel to {}", agent_addr);

    let mut tcp_stream = None;
    for _ in 0..5 {
        match tokio::time::timeout(Duration::from_millis(800), TcpStream::connect(agent_addr)).await {
            Ok(Ok(s)) => {
                tcp_stream = Some(s);
                break;
            }
            Ok(Err(e)) => {
                log::warn!("Connect attempt error: {}", e);
                tokio::time::sleep(Duration::from_millis(200)).await;
            }
            Err(elapsed) => {
                log::warn!("Connect attempt timed out: {}", elapsed);
                tokio::time::sleep(Duration::from_millis(200)).await;
            }
        }
    }
    let mut tcp_stream = tcp_stream.ok_or("Failed to connect to agent after retries")?;
    log::info!("Connected to agent control port {}.", agent_addr);

    // Request stream start
    let start_msg = [OP_STREAM_START, 0, 0, 0];
    tcp_stream.write_all(&start_msg).await?;

    *state.write() = StreamState::Connected;

    // Spawn Media Receiver
    let frame_sink = latest_frame.clone();
    let metrics_media = metrics.clone();
    let running_media = running.clone();

    tokio::spawn(async move {
        if let Err(e) = run_media_receiver(frame_sink, metrics_media, audio_prod, running_media).await {
            log::error!("Media receiver error: {}", e);
        }
    });

    let mut ping_interval = tokio::time::interval(Duration::from_millis(1000));
    let mut last_ping_sent: Option<Instant> = None;
    let mut read_buf = [0u8; 2048];

    while running.load(Ordering::Relaxed) {
        tokio::select! {
            _ = ping_interval.tick() => {
                last_ping_sent = Some(Instant::now());
                let ping_frame = [OP_PING, 0, 0, 0];
                let _ = tcp_stream.write_all(&ping_frame).await;
            }

            Some(ev) = input_rx.recv() => {
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

            res = tcp_stream.read(&mut read_buf) => {
                let n = match res {
                    Ok(n) if n > 0 => n,
                    _ => {
                        log::warn!("TCP connection closed by agent.");
                        break;
                    }
                };

                let mut offset = 0;
                while offset + TcpFrameHeader::SIZE <= n {
                    if let Some(hdr) = TcpFrameHeader::parse(&read_buf[offset..]) {
                        let payload_start = offset + TcpFrameHeader::SIZE;
                        let payload_end = payload_start + hdr.payload_len as usize;

                        if payload_end <= n {
                            let payload = &read_buf[payload_start..payload_end];
                            match hdr.opcode {
                                OP_HELLO_SYN => {
                                    if let Some(syn) = MsgHelloSyn::parse(payload) {
                                        log::info!(
                                            "[Agent Connected] Name='{}' Screen={}x{}@{}bpp AgentVer={}",
                                            syn.machine_name, syn.screen_width, syn.screen_height, syn.bpp, syn.agent_version
                                        );
                                        let mut m = metrics.write();
                                        m.machine_name = syn.machine_name;
                                        m.screen_width = syn.screen_width;
                                        m.screen_height = syn.screen_height;
                                    }
                                }
                                OP_VIDEO_RESIZE => {
                                    if let Some(res) = MsgVideoResize::parse(payload) {
                                        log::info!("[Display Change] Agent resized to {}x{}@{}bpp", res.new_width, res.new_height, res.new_bpp);
                                        let mut m = metrics.write();
                                        m.screen_width = res.new_width;
                                        m.screen_height = res.new_height;
                                    }
                                }
                                OP_PONG => {
                                    if let Some(sent) = last_ping_sent.take() {
                                        let rtt = sent.elapsed().as_secs_f64() * 1000.0;
                                        metrics.write().rtt_ms = rtt;
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

    *state.write() = StreamState::Disconnected("Clean Disconnect".to_string());
    Ok(())
}

struct PartialVideoFrame {
    width: u16,
    height: u16,
    total_chunks: u16,
    received_chunks: HashMap<u16, Vec<u8>>,
}

async fn run_media_receiver(
    latest_frame: Arc<RwLock<Option<VideoFrame>>>,
    metrics: Arc<RwLock<SessionMetrics>>,
    audio_prod: Option<SharedAudioProducer>,
    running: Arc<AtomicBool>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let std_sock = std::net::UdpSocket::bind(format!("0.0.0.0:{}", UDP_MEDIA_PORT))?;
    let sock2 = socket2::SockRef::from(&std_sock);
    let _ = sock2.set_recv_buffer_size(8 * 1024 * 1024);
    std_sock.set_nonblocking(true)?;
    let socket = UdpSocket::from_std(std_sock)?;
    log::info!("Media receiver listening on UDP port {} (8MB buffer).", UDP_MEDIA_PORT);

    let mut audio_clock = PtsClock::new(25);
    let mut video_frames: HashMap<u32, PartialVideoFrame> = HashMap::new();

    let mut buf = [0u8; 2048];
    let mut last_stats = Instant::now();
    let mut bytes_in_window: u64 = 0;
    let mut frames_in_window: u64 = 0;
    let mut total_jitter_abs: f64 = 0.0;
    let mut jitter_samples: u64 = 0;

    while running.load(Ordering::Relaxed) {
        let (len, _src) = match socket.recv_from(&mut buf).await {
            Ok(res) => res,
            Err(_) => break,
        };

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
                        metrics.write().audio_slices_dropped += 1;
                        continue;
                    }

                    if let Some((_ah, pcm_data)) = AudioSliceHeader::parse(payload) {
                        if let Some(ref prod) = audio_prod {
                            crate::audio::push_pcm16_samples(&mut *prod.lock(), pcm_data);
                        }
                        metrics.write().audio_slices_rx += 1;
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
                            // Reassemble and decompress
                            let mut compressed = Vec::new();
                            for c in 0..entry.total_chunks {
                                if let Some(part) = entry.received_chunks.get(&c) {
                                    compressed.extend_from_slice(part);
                                }
                            }

                            if vh.codec == 2 {
                                let uncompressed_len = (entry.width as usize) * (entry.height as usize) * 4;
                                if let Ok(decompressed) = lz4_flex::decompress(&compressed, uncompressed_len) {
                                    frames_in_window += 1;
                                    // Convert BGRA to RGBA in-place into an Arc buffer
                                    let mut rgba = decompressed;
                                    for chunk in rgba.chunks_exact_mut(4) {
                                        let b = chunk[0];
                                        let r = chunk[2];
                                        chunk[0] = r;
                                        chunk[2] = b;
                                        chunk[3] = 255;
                                    }

                                    let frame = VideoFrame {
                                        width: entry.width,
                                        height: entry.height,
                                        frame_index: vh.frame_index,
                                        rgba_pixels: Arc::new(rgba),
                                    };

                                    *latest_frame.write() = Some(frame);
                                    metrics.write().video_frames_rx += 1;
                                }
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

        if last_stats.elapsed() >= Duration::from_millis(500) {
            let elapsed = last_stats.elapsed().as_secs_f64();
            let kbps = (bytes_in_window as f64 * 8.0) / (elapsed * 1000.0);
            let fps = frames_in_window as f64 / elapsed;
            let avg_jitter = if jitter_samples > 0 { total_jitter_abs / jitter_samples as f64 } else { 0.0 };

            let mut m = metrics.write();
            m.kbps = kbps;
            m.fps = fps;
            m.audio_jitter_ms = avg_jitter;

            bytes_in_window = 0;
            frames_in_window = 0;
            total_jitter_abs = 0.0;
            jitter_samples = 0;
            last_stats = Instant::now();
        }
    }

    Ok(())
}
