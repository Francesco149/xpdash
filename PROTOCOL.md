# xpdash Wire Protocol Specification

Version: 1.0  
Endianness: Little-Endian for all multi-byte integers unless otherwise specified.

---

## 1. Network Endpoints & Ports

| Port | Transport | Purpose | Direction |
|---|---|---|---|
| **7022** | UDP Broadcast | LAN Auto-Discovery Beacon | Host Server → LAN Broadcast (255.255.255.255) |
| **7020** | TCP | Control, Authentication, Input & Resizing | Client ⇄ Agent (Bidirectional) |
| **7021** | UDP | Real-Time Media Stream (Audio & Video) | Agent → Client |

---

## 2. LAN Discovery Protocol (UDP Port 7022)

The host server periodically broadcasts a discovery beacon every 2,000 ms.

### 2.1 Beacon Packet Format
```
+---------------+---------------+---------------+---------------+
| Magic: 'X'    | Magic: 'P'    | Magic: 'D'    | Version: 0x01 |
+---------------+---------------+---------------+---------------+
| Control Port (u16 LE)         | Media Port (u16 LE)           |
+-------------------------------+-------------------------------+
| Server Name Length (u8)       | Server Name (UTF-8, <= 32 B)  |
+-------------------------------+-------------------------------+
| Fingerprint Length (u8 = 32)  | Ed25519 Public Key (32 bytes) |
+---------------------------------------------------------------+
```

When `xpdash-agent` receives this packet:
1. It compares the Ed25519 Public Key against its trusted fingerprint list (`agent.ini` / `trusted_servers.ini`).
2. If trusted, it immediately initiates a TCP connection to the server's IP address on the specified Control Port.
3. If untrusted, it prompts the interactive XP user or logs the discovery.

---

## 3. Control & Handshake Protocol (TCP Port 7020)

Frames over TCP use length-prefixed framing:
```
+---------------+---------------+---------------+---------------+
| Opcode (u8)   | Reserved (u8) | Payload Length (u16 LE)       |
+---------------+---------------+---------------+---------------+
| Payload Data ... (0 to 65535 bytes)                           |
+---------------------------------------------------------------+
```

### 3.1 Control Opcodes

| Opcode | Name | Direction | Description |
|---|---|---|---|
| `0x01` | `HELLO_SYN` | Agent → Server | Agent announces machine name, OS, screen res |
| `0x02` | `AUTH_CHALLENGE` | Server → Agent | Server sends 32-byte nonce challenge |
| `0x03` | `AUTH_RESPONSE` | Agent → Server | Agent signs challenge or sends pre-shared token |
| `0x04` | `AUTH_OK` | Server → Agent | Authentication successful; media streaming authorized |
| `0x05` | `AUTH_REJECT` | Server → Agent | Authentication failed / untrusted |
| `0x10` | `PING` | Either → Either | Heartbeat probe |
| `0x11` | `PONG` | Either → Either | Heartbeat reply |
| `0x20` | `VIDEO_RESIZE` | Agent → Server | Desktop resolution changed (`width`, `height`, `bpp`) |
| `0x30` | `INPUT_EVENT` | Server → Agent | Forwarded keyboard / mouse input |
| `0x40` | `AUDIO_CONFIG` | Server → Agent | Requested audio settings (channels, sample rate) |
| `0x50` | `STREAM_START` | Server → Agent | Request agent to begin UDP media transmission |
| `0x51` | `STREAM_STOP` | Server → Agent | Request agent to pause UDP media transmission |

### 3.2 Payload Details

#### `HELLO_SYN` (`0x01`)
```c
struct HelloSyn {
    uint32_t agent_version; // 1
    uint16_t screen_width;  // e.g. 1024
    uint16_t screen_height; // e.g. 768
    uint8_t  bpp;           // 32
    uint8_t  name_len;
    char     machine_name[32]; // e.g. "TIMEMACHINE-XP"
};
```

#### `VIDEO_RESIZE` (`0x20`)
Emitted by the agent when `WM_DISPLAYCHANGE` occurs:
```c
struct VideoResize {
    uint16_t new_width;
    uint16_t new_height;
    uint8_t  new_bpp;
};
```

#### `INPUT_EVENT` (`0x30`)
```c
struct InputEvent {
    uint8_t  event_type; // 1 = Key, 2 = MouseMoveRel, 3 = MouseMoveAbs, 4 = MouseButton, 5 = MouseWheel
    uint16_t param1;     // Key scancode / Mouse button flags / Wheel delta
    int16_t  param2;     // Mouse dx / Abs X
    int16_t  param3;     // Mouse dy / Abs Y
    uint8_t  key_down;   // 1 = Pressed, 0 = Released
};
```

---

## 4. Media Streaming Protocol (UDP Port 7021)

All audio and video data is packetized into UDP datagrams with an unfragmented payload (MTU <= 1400 bytes).

### 4.1 Datagram Header (12 bytes)
```
+---------------+---------------+---------------+---------------+
| Magic: 0x58   | Type (u8)     | Flags (u16 LE)                |
+---------------+---------------+---------------+---------------+
| Stream Sequence Number (u32 LE)                               |
+---------------+---------------+---------------+---------------+
| Presentation Timestamp in ms (PTS u32 LE)                     |
+---------------+---------------+---------------+---------------+
| Payload Length (u16 LE)       | Reserved (u16)                |
+---------------+---------------+---------------+---------------+
```

- **Magic**: `0x58` ('X')
- **Type**:
  - `0x01` = Video Slice / Chunk
  - `0x02` = Audio Frame (PCM)
  - `0x03` = Media Ping/Keepalive

### 4.2 Audio Frame Payload (`Type = 0x02`)
For 48 kHz 16-bit stereo PCM with a 10ms slice:
- Samples per channel: 480
- Total samples: 960
- Payload bytes: 1,920 bytes (split across 2 datagrams of 960 bytes, or single packet if local MTU permits).
- Datagram carries:
  ```c
  struct AudioSliceHeader {
      uint8_t  sub_seq;       // 0 or 1 (fragment index)
      uint8_t  total_subs;    // total fragments for this audio PTS
      uint16_t sample_rate;   // 48000
      uint8_t  channels;      // 2
      uint8_t  bits_per_sample; // 16
  };
  // Followed by raw little-endian signed 16-bit interleaved PCM samples
  ```

### 4.3 Video Chunk Payload (`Type = 0x01`)
```c
struct VideoChunkHeader {
    uint32_t frame_index;    // Monotonically increasing frame counter
    uint16_t chunk_index;    // 0 to total_chunks - 1
    uint16_t total_chunks;   // Total chunks composing this full frame
    uint16_t frame_width;    // Current width
    uint16_t frame_height;   // Current height
    uint8_t  codec;          // 0 = Raw RGB, 1 = JPEG, 2 = Fast LZ4
    uint8_t  flags;          // bit 0 = Keyframe (full refresh)
};
// Followed by compressed frame data for this chunk
```

---

## 5. Reconnection & Dropout State Machine

```
      ┌─────────────┐
      │ Disconnected│ ◄────────────────────────┐
      └──────┬──────┘                          │
             │ Beacon heard / User connect     │ Timeout / Network drop
             ▼                                 │
      ┌─────────────┐                          │
      │  Connecting │ ─────────────────────────┤
      └──────┬──────┘                          │
             │ Auth OK                         │
             ▼                                 │
      ┌─────────────┐                          │
      │  Streaming  │ ─────────────────────────┘
      └──────┬──────┘
             │ WM_DISPLAYCHANGE
             ▼
      ┌─────────────┐
      │  Resizing   │ (Emits VIDEO_RESIZE, pauses capture,
      └──────┬──────┘  resizes buffers, resumes Streaming)
             │ Ack
             ▼
      ┌─────────────┐
      │  Streaming  │
      └─────────────┘
```

- If TCP drops, media transmission pauses immediately.
- The agent enters `Disconnected` state and returns to listening for beacons.
- The client retains the last displayed video frame with an "Offline / Reconnecting" overlay, preserving window layout and audio device binding without crashing.
