#ifndef XPDASH_NET_H
#define XPDASH_NET_H

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#define NET_TCP_CONTROL_PORT 7020
#define NET_UDP_MEDIA_PORT   7021
#define NET_UDP_BEACON_PORT  7022

#define PKT_MAGIC 0x58 // 'X'
#define PKT_TYPE_VIDEO 0x01
#define PKT_TYPE_AUDIO 0x02
#define PKT_TYPE_PING  0x03

#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;       // 0x58
    uint8_t  pkt_type;    // PKT_TYPE_*
    uint16_t flags;
    uint32_t seq;
    uint32_t pts_ms;
    uint16_t payload_len;
    uint16_t reserved;
} NetPacketHeader;
#pragma pack(pop)

typedef void (*net_input_cb)(const uint8_t *data, uint32_t len, void *user_data);

/* Initialize network subsystem (Winsock) */
int net_init(void);

/* Bind and listen for control connection */
int net_listen_control(uint16_t port);

/* Set target client endpoint for UDP media stream */
void net_set_media_destination(const char *ip, uint16_t port);

/* Send an audio frame via UDP */
int net_send_audio(const uint8_t *data, uint32_t len, uint32_t pts_ms);

/* Send a video chunk via UDP */
int net_send_video_chunk(const uint8_t *data, uint32_t len, uint32_t pts_ms);

/* Process control network events */
void net_poll_control(net_input_cb on_input, void *user_data);

/* Cleanup network resources */
void net_shutdown(void);

#endif /* XPDASH_NET_H */
