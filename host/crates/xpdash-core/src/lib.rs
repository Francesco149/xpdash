//! xpdash-core — Shared protocol definitions, packet serialization, and timestamp math.

use byteorder::{ByteOrder, LittleEndian};

pub const MAGIC: u8 = 0x58; // 'X'

pub const TCP_CONTROL_PORT: u16 = 7020;
pub const UDP_MEDIA_PORT: u16   = 7021;
pub const UDP_BEACON_PORT: u16  = 7022;

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

/// 12-byte UDP Datagram Header
#[derive(Debug, Clone, Copy)]
pub struct NetPacketHeader {
    pub pkt_type: PacketType,
    pub flags: u16,
    pub seq: u32,
    pub pts_ms: u32,
    pub payload_len: u16,
}

impl NetPacketHeader {
    pub const SIZE: usize = 12;

    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < Self::SIZE || buf[0] != MAGIC {
            return None;
        }
        let pkt_type = PacketType::try_from(buf[1]).ok()?;
        let flags = LittleEndian::read_u16(&buf[2..4]);
        let seq = LittleEndian::read_u32(&buf[4..8]);
        let pts_ms = LittleEndian::read_u32(&buf[8..12]);
        let payload_len = LittleEndian::read_u16(&buf[12..14]);

        Some(Self {
            pkt_type,
            flags,
            seq,
            pts_ms,
            payload_len,
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
