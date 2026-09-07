#ifndef XPDASH_NET_H
#define XPDASH_NET_H

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#define NET_TCP_CONTROL_PORT 7020
#define NET_UDP_MEDIA_PORT   7021
#define NET_UDP_BEACON_PORT  7022

#define PKT_MAGIC 0x58 // 'X'

/* UDP Media Packet Types */
#define PKT_TYPE_VIDEO 0x01
#define PKT_TYPE_AUDIO 0x02
#define PKT_TYPE_PING  0x03

/* Video Codecs */
#define VIDEO_CODEC_RAW_RGB 0
#define VIDEO_CODEC_JPEG    1
#define VIDEO_CODEC_LZ4     2

/* Video Flags */
#define VIDEO_FLAG_KEYFRAME 0x01

/* TCP Control Opcodes */
#define OP_HELLO_SYN      0x01
#define OP_AUTH_CHALLENGE 0x02
#define OP_AUTH_RESPONSE  0x03
#define OP_AUTH_OK        0x04
#define OP_AUTH_REJECT    0x05
#define OP_PING           0x10
#define OP_PONG           0x11
#define OP_VIDEO_RESIZE   0x20
#define OP_INPUT_EVENT    0x30
#define OP_AUDIO_CONFIG   0x40
#define OP_STREAM_START   0x50
#define OP_STREAM_STOP    0x51

/* Input Event Types */
#define INPUT_TYPE_KEY          1
#define INPUT_TYPE_MOUSE_REL    2
#define INPUT_TYPE_MOUSE_ABS    3
#define INPUT_TYPE_MOUSE_BTN    4
#define INPUT_TYPE_MOUSE_WHEEL  5

#pragma pack(push, 1)

/* 16-byte UDP Media Datagram Header */
typedef struct {
    uint8_t  magic;       // 0x58
    uint8_t  pkt_type;    // PKT_TYPE_*
    uint16_t flags;       // Flags (bit 0 = keyframe for video)
    uint32_t seq;         // Sequence counter
    uint32_t pts_ms;      // Presentation timestamp
    uint16_t payload_len; // Length of payload following this header
    uint16_t reserved;
} NetPacketHeader;

/* 6-byte Audio Slice Header */
typedef struct {
    uint8_t  sub_seq;         // 0 or 1
    uint8_t  total_subs;      // e.g. 2
    uint16_t sample_rate;     // 48000
    uint8_t  channels;        // 2
    uint8_t  bits_per_sample; // 16
} AudioSliceHeader;

/* 14-byte Video Chunk Header */
typedef struct {
    uint32_t frame_index;
    uint16_t chunk_index;
    uint16_t total_chunks;
    uint16_t frame_width;
    uint16_t frame_height;
    uint8_t  codec; // 2 = LZ4
    uint8_t  flags; // bit 0 = keyframe
} VideoChunkHeader;

/* TCP Frame Header (4 bytes) */
typedef struct {
    uint8_t  opcode;
    uint8_t  reserved;
    uint16_t payload_len;
} TcpFrameHeader;

/* TCP Payload: HELLO_SYN (0x01) */
typedef struct {
    uint32_t agent_version;
    uint16_t screen_width;
    uint16_t screen_height;
    uint8_t  bpp;
    uint8_t  name_len;
    char     machine_name[32];
} MsgHelloSyn;

/* TCP Payload: VIDEO_RESIZE (0x20) */
typedef struct {
    uint16_t new_width;
    uint16_t new_height;
    uint8_t  new_bpp;
} MsgVideoResize;

/* TCP Payload: INPUT_EVENT (0x30) */
typedef struct {
    uint8_t  event_type; // 1 = Key, 2 = MouseMoveRel, 3 = MouseMoveAbs, 4 = MouseButton, 5 = MouseWheel
    uint16_t param1;     // Key scancode / Mouse button flags / Wheel delta
    int16_t  param2;     // Mouse dx / Abs X
    int16_t  param3;     // Mouse dy / Abs Y
    uint8_t  key_down;   // 1 = Pressed, 0 = Released
} MsgInputEvent;

#pragma pack(pop)

typedef void (*net_stream_state_cb)(int is_streaming, void *user_data);

/* Initialize network subsystem (Winsock) */
int net_init(void);

/* Bind and listen for control connection */
int net_listen_control(uint16_t port);

/* Set target client endpoint for UDP media stream */
void net_set_media_destination(const char *ip, uint16_t port);

/* Check if streaming is active and authorized */
int net_is_streaming_active(void);

/* Send an audio frame via UDP (splits into slices <= 1400 bytes MTU) */
int net_send_audio(const uint8_t *pcm_data, uint32_t size, uint32_t pts_ms);

/* Send a compressed video frame split into chunks */
int net_send_video_frame(const uint8_t *comp_data, uint32_t comp_size,
                         uint32_t frame_index, uint16_t width, uint16_t height,
                         uint8_t codec, uint8_t flags, uint32_t pts_ms);

/* Send VIDEO_RESIZE notification over TCP */
int net_send_video_resize(uint16_t width, uint16_t height, uint8_t bpp);

/* Process control network events */
void net_poll_control(net_stream_state_cb on_state_change, void *user_data);

/* Cleanup network resources */
void net_shutdown(void);

#endif /* XPDASH_NET_H */
