#include "net.h"
#include <stdio.h>
#include <stdlib.h>

static SOCKET g_sock_tcp = INVALID_SOCKET;
static SOCKET g_client_tcp = INVALID_SOCKET;
static SOCKET g_sock_udp = INVALID_SOCKET;
static struct sockaddr_in g_media_dest;
static int g_has_media_dest = 0;
static uint32_t g_seq_audio = 0;
static uint32_t g_seq_video = 0;

int net_init(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }

    g_sock_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock_udp == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    // Set non-blocking on UDP socket
    u_long mode = 1;
    ioctlsocket(g_sock_udp, FIONBIO, &mode);
    return 1;
}

int net_listen_control(uint16_t port) {
    g_sock_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock_tcp == INVALID_SOCKET) return 0;

    int opt = 1;
    setsockopt(g_sock_tcp, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    if (bind(g_sock_tcp, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        closesocket(g_sock_tcp);
        g_sock_tcp = INVALID_SOCKET;
        return 0;
    }

    if (listen(g_sock_tcp, 1) != 0) {
        closesocket(g_sock_tcp);
        g_sock_tcp = INVALID_SOCKET;
        return 0;
    }

    u_long mode = 1;
    ioctlsocket(g_sock_tcp, FIONBIO, &mode);
    return 1;
}

void net_set_media_destination(const char *ip, uint16_t port) {
    memset(&g_media_dest, 0, sizeof(g_media_dest));
    g_media_dest.sin_family = AF_INET;
    g_media_dest.sin_addr.s_addr = inet_addr(ip);
    g_media_dest.sin_port = htons(port);
    g_has_media_dest = 1;
}

int net_send_audio(const uint8_t *data, uint32_t len, uint32_t pts_ms) {
    if (!g_has_media_dest || g_sock_udp == INVALID_SOCKET) return 0;

    uint8_t packet[2048];
    if (len + sizeof(NetPacketHeader) > sizeof(packet)) return 0;

    NetPacketHeader *hdr = (NetPacketHeader *)packet;
    hdr->magic = PKT_MAGIC;
    hdr->pkt_type = PKT_TYPE_AUDIO;
    hdr->flags = 0;
    hdr->seq = g_seq_audio++;
    hdr->pts_ms = pts_ms;
    hdr->payload_len = (uint16_t)len;
    hdr->reserved = 0;

    memcpy(packet + sizeof(NetPacketHeader), data, len);

    int sent = sendto(g_sock_udp, (const char *)packet, sizeof(NetPacketHeader) + len, 0,
                      (struct sockaddr *)&g_media_dest, sizeof(g_media_dest));
    return (sent > 0);
}

int net_send_video_chunk(const uint8_t *data, uint32_t len, uint32_t pts_ms) {
    if (!g_has_media_dest || g_sock_udp == INVALID_SOCKET) return 0;

    uint8_t packet[2048];
    if (len + sizeof(NetPacketHeader) > sizeof(packet)) return 0;

    NetPacketHeader *hdr = (NetPacketHeader *)packet;
    hdr->magic = PKT_MAGIC;
    hdr->pkt_type = PKT_TYPE_VIDEO;
    hdr->flags = 0;
    hdr->seq = g_seq_video++;
    hdr->pts_ms = pts_ms;
    hdr->payload_len = (uint16_t)len;
    hdr->reserved = 0;

    memcpy(packet + sizeof(NetPacketHeader), data, len);

    int sent = sendto(g_sock_udp, (const char *)packet, sizeof(NetPacketHeader) + len, 0,
                      (struct sockaddr *)&g_media_dest, sizeof(g_media_dest));
    return (sent > 0);
}

void net_poll_control(net_input_cb on_input, void *user_data) {
    if (g_sock_tcp == INVALID_SOCKET) return;

    if (g_client_tcp == INVALID_SOCKET) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET s = accept(g_sock_tcp, (struct sockaddr *)&client_addr, &addr_len);
        if (s != INVALID_SOCKET) {
            g_client_tcp = s;
            u_long mode = 1;
            ioctlsocket(g_client_tcp, FIONBIO, &mode);
            char *ip = inet_ntoa(client_addr.sin_addr);
            net_set_media_destination(ip, NET_UDP_MEDIA_PORT);
        }
    } else {
        uint8_t buf[1024];
        int r = recv(g_client_tcp, (char *)buf, sizeof(buf), 0);
        if (r > 0) {
            if (on_input) on_input(buf, (uint32_t)r, user_data);
        } else if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
            closesocket(g_client_tcp);
            g_client_tcp = INVALID_SOCKET;
            g_has_media_dest = 0;
        }
    }
}

void net_shutdown(void) {
    if (g_client_tcp != INVALID_SOCKET) {
        closesocket(g_client_tcp);
        g_client_tcp = INVALID_SOCKET;
    }
    if (g_sock_tcp != INVALID_SOCKET) {
        closesocket(g_sock_tcp);
        g_sock_tcp = INVALID_SOCKET;
    }
    if (g_sock_udp != INVALID_SOCKET) {
        closesocket(g_sock_udp);
        g_sock_udp = INVALID_SOCKET;
    }
    WSACleanup();
}
