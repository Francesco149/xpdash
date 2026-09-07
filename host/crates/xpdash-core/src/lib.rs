//! xpdash-core — Shared protocol definitions, packet serialization, and timestamp math.

use byteorder::{ByteOrder, LittleEndian};
use std::time::{Duration, Instant};

pub mod security;
pub use security::{compute_fingerprint, format_fingerprint, HostIdentity};

pub const MAGIC: u8 = 0x58; // 'X'

pub const TCP_CONTROL_PORT: u16 = 7020;
pub const UDP_MEDIA_PORT: u16   = 7021;
pub const UDP_BEACON_PORT: u16  = 7022;

/* TCP Control Opcodes */
pub const OP_HELLO_SYN: u8      = 0x01;
pub const OP_AUTH_CHALLENGE: u8 = 0x02;
pub const OP_AUTH_RESPONSE: u8  = 0x03;
pub const OP_AUTH_OK: u8        = 0x04;
pub const OP_AUTH_REJECT: u8    = 0x05;
pub const OP_PING: u8           = 0x10;
pub const OP_PONG: u8           = 0x11;
pub const OP_VIDEO_RESIZE: u8   = 0x20;
pub const OP_INPUT_EVENT: u8    = 0x30;
pub const OP_AUDIO_CONFIG: u8   = 0x40;
pub const OP_STREAM_START: u8   = 0x50;
pub const OP_STREAM_STOP: u8    = 0x51;

/* Input Event Types */
pub const INPUT_TYPE_KEY: u8         = 1;
pub const INPUT_TYPE_MOUSE_REL: u8   = 2;
pub const INPUT_TYPE_MOUSE_ABS: u8   = 3;
pub const INPUT_TYPE_MOUSE_BTN: u8   = 4;
pub const INPUT_TYPE_MOUSE_WHEEL: u8 = 5;

/* Video Codecs */
pub const VIDEO_CODEC_RAW_RGB: u8 = 0;
pub const VIDEO_CODEC_JPEG: u8    = 1;
pub const VIDEO_CODEC_LZ4: u8     = 2;

pub const VIDEO_FLAG_KEYFRAME: u8 = 0x01;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PacketType {
    Video = 0x01,
    Audio = 0x02,
    Ping  = 0x03,
}

impl TryFrom<u8> for PacketType {
    type Error = ();
    fn try_from(val: u8) -> Result<Self, Self::Error> {
        match val {
            0x01 => Ok(PacketType::Video),
            0x02 => Ok(PacketType::Audio),
            0x03 => Ok(PacketType::Ping),
            _ => Err(()),
        }
    }
}

/// 16-byte UDP Datagram Header
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NetPacketHeader {
    pub pkt_type: PacketType,
    pub flags: u16,
    pub seq: u32,
    pub pts_ms: u32,
    pub payload_len: u16,
}

impl NetPacketHeader {
    pub const SIZE: usize = 16;

    pub fn parse(buf: &[u8]) -> Option<(Self, &[u8])> {
        if buf.len() < Self::SIZE || buf[0] != MAGIC {
            return None;
        }
        let pkt_type = PacketType::try_from(buf[1]).ok()?;
        let flags = LittleEndian::read_u16(&buf[2..4]);
        let seq = LittleEndian::read_u32(&buf[4..8]);
        let pts_ms = LittleEndian::read_u32(&buf[8..12]);
        let payload_len = LittleEndian::read_u16(&buf[12..14]);

        let payload = &buf[Self::SIZE..];
        Some((
            Self {
                pkt_type,
                flags,
                seq,
                pts_ms,
                payload_len,
            },
            payload,
        ))
    }

    pub fn encode(&self, buf: &mut [u8]) -> bool {
        if buf.len() < Self::SIZE {
            return false;
        }
        buf[0] = MAGIC;
        buf[1] = self.pkt_type as u8;
        LittleEndian::write_u16(&mut buf[2..4], self.flags);
        LittleEndian::write_u32(&mut buf[4..8], self.seq);
        LittleEndian::write_u32(&mut buf[8..12], self.pts_ms);
        LittleEndian::write_u16(&mut buf[12..14], self.payload_len);
        LittleEndian::write_u16(&mut buf[14..16], 0); // reserved
        true
    }
}

/// 6-byte Audio Slice Header
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AudioSliceHeader {
    pub sub_seq: u8,
    pub total_subs: u8,
    pub sample_rate: u16,
    pub channels: u8,
    pub bits_per_sample: u8,
}

impl AudioSliceHeader {
    pub const SIZE: usize = 6;

    pub fn parse(buf: &[u8]) -> Option<(Self, &[u8])> {
        if buf.len() < Self::SIZE {
            return None;
        }
        let sub_seq = buf[0];
        let total_subs = buf[1];
        let sample_rate = LittleEndian::read_u16(&buf[2..4]);
        let channels = buf[4];
        let bits_per_sample = buf[5];

        Some((
            Self {
                sub_seq,
                total_subs,
                sample_rate,
                channels,
                bits_per_sample,
            },
            &buf[Self::SIZE..],
        ))
    }

    pub fn encode(&self, buf: &mut [u8]) -> bool {
        if buf.len() < Self::SIZE {
            return false;
        }
        buf[0] = self.sub_seq;
        buf[1] = self.total_subs;
        LittleEndian::write_u16(&mut buf[2..4], self.sample_rate);
        buf[4] = self.channels;
        buf[5] = self.bits_per_sample;
        true
    }
}

/// 14-byte Video Chunk Header
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VideoChunkHeader {
    pub frame_index: u32,
    pub chunk_index: u16,
    pub total_chunks: u16,
    pub frame_width: u16,
    pub frame_height: u16,
    pub codec: u8,
    pub flags: u8,
}

impl VideoChunkHeader {
    pub const SIZE: usize = 14;

    pub fn parse(buf: &[u8]) -> Option<(Self, &[u8])> {
        if buf.len() < Self::SIZE {
            return None;
        }
        let frame_index = LittleEndian::read_u32(&buf[0..4]);
        let chunk_index = LittleEndian::read_u16(&buf[4..6]);
        let total_chunks = LittleEndian::read_u16(&buf[6..8]);
        let frame_width = LittleEndian::read_u16(&buf[8..10]);
        let frame_height = LittleEndian::read_u16(&buf[10..12]);
        let codec = buf[12];
        let flags = buf[13];

        Some((
            Self {
                frame_index,
                chunk_index,
                total_chunks,
                frame_width,
                frame_height,
                codec,
                flags,
            },
            &buf[Self::SIZE..],
        ))
    }

    pub fn encode(&self, buf: &mut [u8]) -> bool {
        if buf.len() < Self::SIZE {
            return false;
        }
        LittleEndian::write_u32(&mut buf[0..4], self.frame_index);
        LittleEndian::write_u16(&mut buf[4..6], self.chunk_index);
        LittleEndian::write_u16(&mut buf[6..8], self.total_chunks);
        LittleEndian::write_u16(&mut buf[8..10], self.frame_width);
        LittleEndian::write_u16(&mut buf[10..12], self.frame_height);
        buf[12] = self.codec;
        buf[13] = self.flags;
        true
    }
}

/// 4-byte TCP Frame Header
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TcpFrameHeader {
    pub opcode: u8,
    pub reserved: u8,
    pub payload_len: u16,
}

impl TcpFrameHeader {
    pub const SIZE: usize = 4;

    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < Self::SIZE {
            return None;
        }
        Some(Self {
            opcode: buf[0],
            reserved: buf[1],
            payload_len: LittleEndian::read_u16(&buf[2..4]),
        })
    }

    pub fn encode(&self, buf: &mut [u8]) -> bool {
        if buf.len() < Self::SIZE {
            return false;
        }
        buf[0] = self.opcode;
        buf[1] = self.reserved;
        LittleEndian::write_u16(&mut buf[2..4], self.payload_len);
        true
    }
}

/// TCP Payload: HELLO_SYN (0x01)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MsgHelloSyn {
    pub agent_version: u32,
    pub screen_width: u16,
    pub screen_height: u16,
    pub bpp: u8,
    pub machine_name: String,
}

impl MsgHelloSyn {
    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < 10 {
            return None;
        }
        let agent_version = LittleEndian::read_u32(&buf[0..4]);
        let screen_width = LittleEndian::read_u16(&buf[4..6]);
        let screen_height = LittleEndian::read_u16(&buf[6..8]);
        let bpp = buf[8];
        let name_len = buf[9] as usize;

        let name_bytes = if buf.len() >= 10 + name_len {
            &buf[10..10 + name_len]
        } else {
            &buf[10..]
        };
        let machine_name = String::from_utf8_lossy(name_bytes).trim_matches('\0').to_string();

        Some(Self {
            agent_version,
            screen_width,
            screen_height,
            bpp,
            machine_name,
        })
    }
}

/// TCP Payload: VIDEO_RESIZE (0x20)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MsgVideoResize {
    pub new_width: u16,
    pub new_height: u16,
    pub new_bpp: u8,
}

impl MsgVideoResize {
    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < 5 {
            return None;
        }
        Some(Self {
            new_width: LittleEndian::read_u16(&buf[0..2]),
            new_height: LittleEndian::read_u16(&buf[2..4]),
            new_bpp: buf[4],
        })
    }
}

/// TCP Payload: INPUT_EVENT (0x30)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MsgInputEvent {
    pub event_type: u8,
    pub param1: u16,
    pub param2: i16,
    pub param3: i16,
    pub key_down: u8,
}

impl MsgInputEvent {
    pub const SIZE: usize = 8;

    pub fn encode(&self, buf: &mut [u8]) -> bool {
        if buf.len() < Self::SIZE {
            return false;
        }
        buf[0] = self.event_type;
        LittleEndian::write_u16(&mut buf[1..3], self.param1);
        LittleEndian::write_i16(&mut buf[3..5], self.param2);
        LittleEndian::write_i16(&mut buf[5..7], self.param3);
        buf[7] = self.key_down;
        true
    }

    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < Self::SIZE {
            return None;
        }
        Some(Self {
            event_type: buf[0],
            param1: LittleEndian::read_u16(&buf[1..3]),
            param2: LittleEndian::read_i16(&buf[3..5]),
            param3: LittleEndian::read_i16(&buf[5..7]),
            key_down: buf[7],
        })
    }
}

/// Discovery Beacon (UDP Port 7022)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveryBeacon {
    pub control_port: u16,
    pub media_port: u16,
    pub server_name: String,
    pub fingerprint: [u8; 32],
}

impl DiscoveryBeacon {
    pub fn encode(&self) -> Vec<u8> {
        let name_bytes = self.server_name.as_bytes();
        let name_len = name_bytes.len().min(32) as u8;

        let mut out = Vec::with_capacity(9 + name_len as usize + 1 + 32);
        out.extend_from_slice(b"XPD\x01");
        out.extend_from_slice(&self.control_port.to_le_bytes());
        out.extend_from_slice(&self.media_port.to_le_bytes());
        out.push(name_len);
        out.extend_from_slice(&name_bytes[..name_len as usize]);
        out.push(32); // Fingerprint length
        out.extend_from_slice(&self.fingerprint);
        out
    }

    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < 10 || &buf[0..4] != b"XPD\x01" {
            return None;
        }
        let control_port = LittleEndian::read_u16(&buf[4..6]);
        let media_port = LittleEndian::read_u16(&buf[6..8]);
        let name_len = buf[8] as usize;

        if buf.len() < 9 + name_len {
            return None;
        }
        let server_name = String::from_utf8_lossy(&buf[9..9 + name_len]).to_string();

        let fp_offset = 9 + name_len;
        let mut fingerprint = [0u8; 32];
        if buf.len() >= fp_offset + 1 + 32 && buf[fp_offset] == 32 {
            fingerprint.copy_from_slice(&buf[fp_offset + 1..fp_offset + 33]);
        }

        Some(Self {
            control_port,
            media_port,
            server_name,
            fingerprint,
        })
    }
}

/// Audio specifications for xpdash
pub mod audio {
    pub const SAMPLE_RATE: u32 = 48000;
    pub const CHANNELS: u16 = 2;
    pub const BITS_PER_SAMPLE: u16 = 16;
    pub const FRAME_MS: u32 = 10;
    pub const SAMPLES_PER_FRAME: usize = (SAMPLE_RATE * FRAME_MS / 1000) as usize; // 480 per channel
    pub const BYTES_PER_FRAME: usize = SAMPLES_PER_FRAME * (CHANNELS as usize) * 2; // 1920 bytes
}

/// PTS-based Anti-Desync Clock Synchronization
pub struct PtsClock {
    base_remote_pts: Option<u32>,
    base_local_instant: Option<Instant>,
    last_slew_instant: Instant,
    max_jitter_ms: u32,
    consecutive_late: u32,
}

impl PtsClock {
    pub fn new(max_jitter_ms: u32) -> Self {
        Self {
            base_remote_pts: None,
            base_local_instant: None,
            last_slew_instant: Instant::now(),
            max_jitter_ms,
            consecutive_late: 0,
        }
    }

    pub fn reset(&mut self) {
        self.base_remote_pts = None;
        self.base_local_instant = None;
        self.last_slew_instant = Instant::now();
        self.consecutive_late = 0;
    }
    /// Check if packet with `pts_ms` is late or in sync.
    /// Returns `true` if frame is fresh/acceptable, `false` if late and should be dropped.
    pub fn is_packet_acceptable(&mut self, pts_ms: u32) -> bool {
        let now = Instant::now();

        if self.base_remote_pts.is_none() || self.base_local_instant.is_none() {
            self.base_remote_pts = Some(pts_ms);
            self.base_local_instant = Some(now);
            self.last_slew_instant = now;
            self.consecutive_late = 0;
            return true;
        }

        let base_remote = self.base_remote_pts.unwrap();
        let base_local = self.base_local_instant.unwrap();

        let elapsed_local_ms = now.duration_since(base_local).as_millis() as u32;
        let expected_remote_pts = base_remote.wrapping_add(elapsed_local_ms);

        let delta = pts_ms as i64 - expected_remote_pts as i64;

        // Check for large forward or backward timestamp jump (e.g. server restart)
        if delta > 300 || delta < -500 {
            self.base_remote_pts = Some(pts_ms);
            self.base_local_instant = Some(now);
            self.last_slew_instant = now;
            self.consecutive_late = 0;
            return true;
        }

        // Check if packet is late
        if delta < -(self.max_jitter_ms as i64) {
            self.consecutive_late += 1;
            // If we receive sustained late packets, it's clock drift rather than network jitter: resync
            if self.consecutive_late >= 5 {
                self.base_remote_pts = Some(pts_ms);
                self.base_local_instant = Some(now);
                self.last_slew_instant = now;
                self.consecutive_late = 0;
                return true;
            }
            // Isolated late packet: drop to preserve bounded latency
            return false;
        }

        // Packet is on time
        self.consecutive_late = 0;

        // Rate-limited baseline slew if remote clock runs faster than local clock.
        // Max 1ms adjustment every 500ms prevents runaway drift while easily tracking crystal drift (~0.05ms/s).
        if delta > 10 && now.duration_since(self.last_slew_instant) >= Duration::from_millis(500) {
            self.base_remote_pts = Some(base_remote.wrapping_add(1));
            self.last_slew_instant = now;
        }
        true
    }

    /// Calculate the current packet jitter relative to expected clock (in milliseconds).
    pub fn jitter_ms(&self, pts_ms: u32) -> Option<i32> {
        let base_remote = self.base_remote_pts?;
        let base_local = self.base_local_instant?;
        let elapsed_local_ms = Instant::now().duration_since(base_local).as_millis() as u32;
        let expected_remote_pts = base_remote.wrapping_add(elapsed_local_ms);
        Some((pts_ms as i64 - expected_remote_pts as i64) as i32)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_net_packet_header() {
        let hdr = NetPacketHeader {
            pkt_type: PacketType::Audio,
            flags: 0,
            seq: 1234,
            pts_ms: 56789,
            payload_len: 966,
        };
        let mut buf = [0u8; 16];
        assert!(hdr.encode(&mut buf));

        let (parsed, rest) = NetPacketHeader::parse(&buf).expect("parse failed");
        assert_eq!(parsed, hdr);
        assert_eq!(rest.len(), 0);
    }

    #[test]
    fn test_discovery_beacon() {
        let beacon = DiscoveryBeacon {
            control_port: 7020,
            media_port: 7021,
            server_name: "TestServer".to_string(),
            fingerprint: [0x42; 32],
        };
        let encoded = beacon.encode();
        let parsed = DiscoveryBeacon::parse(&encoded).expect("parse beacon failed");
        assert_eq!(parsed, beacon);
    }

    #[test]
    fn test_video_chunk_header() {
        let vh = VideoChunkHeader {
            frame_index: 42,
            chunk_index: 3,
            total_chunks: 10,
            frame_width: 1024,
            frame_height: 768,
            codec: 2,
            flags: 1,
        };
        let mut buf = [0u8; 14];
        assert!(vh.encode(&mut buf));

        let (parsed, _) = VideoChunkHeader::parse(&buf).expect("parse video chunk failed");
        assert_eq!(parsed, vh);
    }

    #[test]
    fn test_pts_clock_normal_and_drift() {
        let mut clock = PtsClock::new(25);
        // First packet initializes baseline
        assert!(clock.is_packet_acceptable(1000));
        assert_eq!(clock.jitter_ms(1000), Some(0));

        // Packets on time
        assert!(clock.is_packet_acceptable(1010));

        // Isolated late packet should be dropped
        assert!(!clock.is_packet_acceptable(950));

        // 5 consecutive late packets triggers resync
        assert!(!clock.is_packet_acceptable(950));
        assert!(!clock.is_packet_acceptable(950));
        assert!(!clock.is_packet_acceptable(950));
        assert!(clock.is_packet_acceptable(950)); // 5th consecutive packet resyncs

        // Large jump immediately resyncs
        assert!(clock.is_packet_acceptable(50000));
    }
}
