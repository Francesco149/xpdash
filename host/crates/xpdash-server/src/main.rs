//! xpdash-server — Host discovery beacon broadcaster, control session manager, and media receiver.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::{Duration, Instant};
use tokio::net::{TcpListener, TcpStream, UdpSocket};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use xpdash_core::{
    DiscoveryBeacon, NetPacketHeader, AudioSliceHeader, VideoChunkHeader,
    TcpFrameHeader, MsgHelloSyn, MsgVideoResize, PacketType, PtsClock,
    HostIdentity,
    TCP_CONTROL_PORT, UDP_MEDIA_PORT, UDP_BEACON_PORT,
    OP_STREAM_START, OP_HELLO_SYN, OP_VIDEO_RESIZE, OP_PING, OP_PONG,
    OP_AUTH_CHALLENGE, OP_AUTH_RESPONSE,
};
fn get_host_key_path() -> PathBuf {
    if let Ok(p) = std::env::var("XPDASH_HOST_KEY") {
        return PathBuf::from(p);
    }
    if let Some(config_dir) = std::env::var_os("XDG_CONFIG_HOME") {
        return PathBuf::from(config_dir).join("xpdash/host_key.bin");
    }
    if let Some(home) = std::env::var_os("HOME") {
        return PathBuf::from(home).join(".config/xpdash/host_key.bin");
    }
    PathBuf::from("host_key.bin")
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init_from_env(env_logger::Env::default().default_filter_or("info"));
    log::info!("=== xpdash-server starting ===");

    // 1. Initialize Host Security Identity
    let key_path = get_host_key_path();
    let identity = HostIdentity::load_or_generate(&key_path)?;
    log::info!("Host Ed25519 Key: {}", key_path.display());
    log::info!("Host Fingerprint: {}", identity.fingerprint_string());

    // Optional target XP agent address from args (e.g. "10.0.10.113")
    let outbound_target: Option<SocketAddr> = std::env::args().nth(1).and_then(|ip| {
        format!("{}:{}", ip, TCP_CONTROL_PORT).parse().ok()
    });

    if let Some(target) = outbound_target {
        log::info!("Outbound target XP Agent specified: {}", target);
    } else {
        log::info!("Running in zero-config auto-discovery mode (listening for LAN agents)");
    }

    // 2. Spawn UDP Discovery Beacon Broadcaster (Port 7022)
    let beacon_identity = identity.clone();
    tokio::spawn(async move {
        if let Err(e) = run_beacon_broadcaster(beacon_identity).await {
            log::warn!("Beacon broadcaster error: {}", e);
        }
    });

    // 3. Spawn UDP Media Receiver (Port 7021)
    tokio::spawn(async move {
        if let Err(e) = run_media_receiver().await {
            log::error!("Media receiver error: {}", e);
        }
    });

    // 4. Bind TCP Control Listener (Port 7020)
    let listener = match TcpListener::bind(format!("0.0.0.0:{}", TCP_CONTROL_PORT)).await {
        Ok(l) => {
            log::info!("Control channel listening on TCP 0.0.0.0:{}", TCP_CONTROL_PORT);
            Some(l)
        }
        Err(e) => {
            log::warn!("Could not bind TCP control listener on port {}: {}", TCP_CONTROL_PORT, e);
            None
        }
    };

    // 5. Connection manager loop
    loop {
        if let Some(l) = &listener {
            if let Some(target) = outbound_target {
                tokio::select! {
                    accept_res = l.accept() => {
                        match accept_res {
                            Ok((stream, peer)) => {
                                log::info!("Accepted incoming XP Agent connection from {}", peer);
                                if let Err(e) = handle_control_session(stream, peer, &identity).await {
                                    log::warn!("Session with {} ended: {}", peer, e);
                                }
                                log::info!("Waiting for next XP Agent connection...");
                            }
                            Err(e) => {
                                log::error!("TCP accept error: {}", e);
                                tokio::time::sleep(Duration::from_millis(500)).await;
                            }
                        }
                    }
                    _ = tokio::time::sleep(Duration::from_secs(3)) => {
                        log::debug!("Attempting outbound connection to {}", target);
                        match TcpStream::connect(target).await {
                            Ok(stream) => {
                                log::info!("Connected outbound to XP Agent at {}", target);
                                if let Err(e) = handle_control_session(stream, target, &identity).await {
                                    log::warn!("Outbound session with {} ended: {}", target, e);
                                }
                                log::info!("Waiting for next XP Agent connection...");
                            }
                            Err(_) => {
                                // Agent not yet up or listening; retry loop continues
                            }
                        }
                    }
                }
            } else {
                match l.accept().await {
                    Ok((stream, peer)) => {
                        log::info!("Accepted incoming XP Agent connection from {}", peer);
                        if let Err(e) = handle_control_session(stream, peer, &identity).await {
                            log::warn!("Session with {} ended: {}", peer, e);
                        }
                        log::info!("Waiting for next XP Agent connection...");
                    }
                    Err(e) => {
                        log::error!("TCP accept error: {}", e);
                        tokio::time::sleep(Duration::from_millis(500)).await;
                    }
                }
            }
        } else if let Some(target) = outbound_target {
            log::info!("Connecting outbound to XP Agent at {}...", target);
            match TcpStream::connect(target).await {
                Ok(stream) => {
                    log::info!("Connected outbound to XP Agent at {}", target);
                    if let Err(e) = handle_control_session(stream, target, &identity).await {
                        log::warn!("Outbound session with {} ended: {}", target, e);
                    }
                }
                Err(e) => {
                    log::debug!("Outbound connect failed: {}, retrying in 3s...", e);
                    tokio::time::sleep(Duration::from_secs(3)).await;
                }
            }
        } else {
            log::error!("Neither TCP listener nor outbound target available. Exiting.");
            break;
        }
    }

    Ok(())
}

async fn handle_control_session(
    mut stream: TcpStream,
    peer: SocketAddr,
    identity: &HostIdentity,
) -> Result<(), Box<dyn std::error::Error>> {
    // Request media streaming start
    let start_frame = [OP_STREAM_START, 0, 0, 0];
    stream.write_all(&start_frame).await?;
    log::info!("Sent OP_STREAM_START to XP Agent at {}", peer);

    let mut buf = [0u8; 2048];
    let mut buffered_len = 0;

    loop {
        let n = stream.read(&mut buf[buffered_len..]).await?;
        if n == 0 {
            log::warn!("Control channel closed by XP Agent at {}", peer);
            break;
        }
        buffered_len += n;

        let mut offset = 0;
        while offset + TcpFrameHeader::SIZE <= buffered_len {
            if let Some(hdr) = TcpFrameHeader::parse(&buf[offset..]) {
                let payload_start = offset + TcpFrameHeader::SIZE;
                let payload_end = payload_start + hdr.payload_len as usize;

                if payload_end <= buffered_len {
                    let payload = &buf[payload_start..payload_end];
                    handle_agent_control_msg(&hdr, payload, &mut stream, identity).await?;
                    offset = payload_end;
                } else {
                    break;
                }
            } else {
                break;
            }
        }

        if offset > 0 {
            buf.copy_within(offset..buffered_len, 0);
            buffered_len -= offset;
        }
    }

    Ok(())
}

async fn handle_agent_control_msg(
    hdr: &TcpFrameHeader,
    payload: &[u8],
    stream: &mut TcpStream,
    identity: &HostIdentity,
) -> Result<(), Box<dyn std::error::Error>> {
    match hdr.opcode {
        OP_HELLO_SYN => {
            if let Some(syn) = MsgHelloSyn::parse(payload) {
                log::info!(
                    "Agent HELLO_SYN: Machine='{}', Res={}x{}@{}bpp, Version={}",
                    syn.machine_name, syn.screen_width, syn.screen_height, syn.bpp, syn.agent_version
                );
            }
        }
        OP_VIDEO_RESIZE => {
            if let Some(resize) = MsgVideoResize::parse(payload) {
                log::info!(
                    "Agent VIDEO_RESIZE: New resolution: {}x{}@{}bpp",
                    resize.new_width, resize.new_height, resize.new_bpp
                );
            }
        }
        OP_PING => {
            let pong_frame = [OP_PONG, 0, 0, 0];
            stream.write_all(&pong_frame).await?;
        }
        OP_AUTH_CHALLENGE => {
            log::info!("Received OP_AUTH_CHALLENGE from Agent ({} bytes)", payload.len());
            let sig = identity.sign(payload);
            let mut resp = Vec::with_capacity(4 + sig.len());
            resp.push(OP_AUTH_RESPONSE);
            resp.push(0);
            resp.extend_from_slice(&(sig.len() as u16).to_le_bytes());
            resp.extend_from_slice(&sig);
            stream.write_all(&resp).await?;
        }
        _ => {
            log::debug!("Received opcode 0x{:02X}, len {}", hdr.opcode, hdr.payload_len);
        }
    }
    Ok(())
}

async fn run_beacon_broadcaster(identity: HostIdentity) -> Result<(), Box<dyn std::error::Error>> {
    let socket = UdpSocket::bind("0.0.0.0:0").await?;
    socket.set_broadcast(true)?;

    let target: SocketAddr = format!("255.255.255.255:{}", UDP_BEACON_PORT).parse()?;
    log::info!(
        "Broadcasting LAN discovery beacons on UDP {} -> Control: {}, Media: {}, Fingerprint: {}",
        UDP_BEACON_PORT, TCP_CONTROL_PORT, UDP_MEDIA_PORT, identity.fingerprint_string()
    );

    let beacon = DiscoveryBeacon {
        control_port: TCP_CONTROL_PORT,
        media_port: UDP_MEDIA_PORT,
        server_name: "xpdash-server".to_string(),
        fingerprint: identity.public_key_bytes(),
    };
    let payload = beacon.encode();

    let mut interval = tokio::time::interval(Duration::from_secs(2));
    loop {
        interval.tick().await;
        let _ = socket.send_to(&payload, target).await;
    }
}

#[allow(dead_code)]
struct PartialFrame {
    width: u16,
    height: u16,
    total_chunks: u16,
    received_chunks: HashMap<u16, Vec<u8>>,
    _pts_ms: u32,
}

async fn run_media_receiver() -> Result<(), Box<dyn std::error::Error>> {
    let socket = UdpSocket::bind(format!("0.0.0.0:{}", UDP_MEDIA_PORT)).await?;
    log::info!("UDP Media receiver listening on 0.0.0.0:{}", UDP_MEDIA_PORT);

    // Expand socket buffer to 8 MB to avoid packet drop on high-FPS video bursts
    let sock_ref = socket2::SockRef::from(&socket);
    let _ = sock_ref.set_recv_buffer_size(8 * 1024 * 1024);

    let mut buf = vec![0u8; 65536];
    let mut frames: HashMap<u32, PartialFrame> = HashMap::new();
    let mut last_log = Instant::now();
    let mut video_frames_complete = 0u64;
    let mut audio_slices_count = 0u64;
    let mut audio_bytes_total = 0u64;
    let mut pts_clock = PtsClock::new(20);

    loop {
        let (len, _src) = socket.recv_from(&mut buf).await?;
        if len < NetPacketHeader::SIZE {
            continue;
        }

        let (hdr, payload) = match NetPacketHeader::parse(&buf[..len]) {
            Some((h, p)) => (h, p),
            None => continue,
        };

        // Timestamp jitter / anti-desync clock check
        let is_acceptable = pts_clock.is_packet_acceptable(hdr.pts_ms);

        match hdr.pkt_type {
            PacketType::Audio => {
                if let Some((_slice_hdr, pcm_data)) = AudioSliceHeader::parse(payload) {
                    audio_slices_count += 1;
                    audio_bytes_total += pcm_data.len() as u64;
                }
            }
            PacketType::Video => {
                if !is_acceptable {
                    // Drop late video frames to prevent buffer bloat
                    continue;
                }
                if let Some((vh, chunk_data)) = VideoChunkHeader::parse(payload) {
                    let entry = frames.entry(vh.frame_index).or_insert_with(|| PartialFrame {
                        width: vh.frame_width,
                        height: vh.frame_height,
                        total_chunks: vh.total_chunks,
                        received_chunks: HashMap::new(),
                        _pts_ms: hdr.pts_ms,
                    });

                    entry.received_chunks.insert(vh.chunk_index, chunk_data.to_vec());
                        if entry.received_chunks.len() == entry.total_chunks as usize {
                            // Frame complete! Assemble and decompress LZ4
                            let mut full_compressed = Vec::new();
                            for c in 0..entry.total_chunks {
                                if let Some(data) = entry.received_chunks.get(&c) {
                                    full_compressed.extend_from_slice(data);
                                }
                            }

                            // Decompress if LZ4
                            let _decompressed = if vh.codec == xpdash_core::VIDEO_CODEC_LZ4 {
                                lz4_flex::decompress_size_prepended(&full_compressed)
                                    .unwrap_or_default()
                            } else {
                                full_compressed
                            };

                            video_frames_complete += 1;
                            frames.remove(&vh.frame_index);

                            // Clean up frames older than this frame
                            frames.retain(|&k, _| k > vh.frame_index.saturating_sub(10));
                        }
                }
            }
            PacketType::Ping => {}
        }

        if last_log.elapsed() >= Duration::from_secs(5) {
            log::info!(
                "[Stream Stats] Complete Video Frames: {}, Audio Slices: {} (~{:.1} kbps)",
                video_frames_complete,
                audio_slices_count,
                (audio_bytes_total as f64 * 8.0) / (last_log.elapsed().as_secs_f64() * 1000.0)
            );
            audio_bytes_total = 0;
            last_log = Instant::now();
        }
    }
}
