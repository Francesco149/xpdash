#include "net.h"
#include <ws2tcpip.h>
#include "input.h"
#include "log.h"
#include "video.h"
#include <stdio.h>
#include <stdlib.h>

/* Use large UDP datagrams (8KB) to reduce per-frame sendto() syscall count.
   IP fragmentation handles link-layer splitting; reassembly is transparent
   to the receiver on a reliable LAN. 800x600 LZ4 ~1.3MB / 8000 = ~163 packets
   vs 971 packets at 1370 bytes. */
#define CHUNK_DATA_MAX 8000
#define TCP_RECV_BUF_SIZE 4096

static SOCKET g_sock_tcp = INVALID_SOCKET;
static SOCKET g_client_tcp = INVALID_SOCKET;
static SOCKET g_sock_udp = INVALID_SOCKET;
static CRITICAL_SECTION g_cs_udp;
static struct sockaddr_in g_media_dest;
static int g_has_media_dest = 0;
static int g_is_streaming = 0;
static int g_is_authenticated = 0;

static uint32_t g_seq_audio = 0;
static uint32_t g_seq_video = 0;

static uint8_t g_tcp_buf[TCP_RECV_BUF_SIZE];
static uint32_t g_tcp_buf_len = 0;

static int send_tcp_msg(uint8_t opcode, const void *payload, uint16_t len) {
    if (g_client_tcp == INVALID_SOCKET) return 0;

    TcpFrameHeader hdr;
    hdr.opcode = opcode;
    hdr.reserved = 0;
    hdr.payload_len = len;

    int r = send(g_client_tcp, (const char *)&hdr, sizeof(hdr), 0);
    if (r != sizeof(hdr)) return 0;

    if (len > 0 && payload != NULL) {
        r = send(g_client_tcp, (const char *)payload, len, 0);
        if (r != (int)len) return 0;
    }
    return 1;
}

static void send_hello_syn(void) {
    MsgHelloSyn msg;
    memset(&msg, 0, sizeof(msg));
    msg.agent_version = 1;
    msg.screen_width = (uint16_t)GetSystemMetrics(SM_CXSCREEN);
    msg.screen_height = (uint16_t)GetSystemMetrics(SM_CYSCREEN);
    int cur_bpp = video_get_bpp();
    msg.bpp = (cur_bpp > 0) ? (uint8_t)cur_bpp : 32;

    DWORD size = sizeof(msg.machine_name);
    if (GetComputerNameA(msg.machine_name, &size)) {
        msg.name_len = (uint8_t)size;
    } else {
        strcpy(msg.machine_name, "TIMEMACHINE-XP");
        msg.name_len = (uint8_t)strlen(msg.machine_name);
    }

    agent_log("Sending HELLO_SYN: %dx%d@%d '%s'", msg.screen_width, msg.screen_height, msg.bpp, msg.machine_name);
    send_tcp_msg(OP_HELLO_SYN, &msg, sizeof(msg));
}

int net_init(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        agent_log("WSAStartup failed!");
        return 0;
    }

    g_sock_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock_udp == INVALID_SOCKET) {
        agent_log("UDP socket create failed!");
        WSACleanup();
        return 0;
    }

    /* Bind to media port so we can both send media AND receive input */
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(NET_UDP_MEDIA_PORT);
    if (bind(g_sock_udp, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) != 0) {
        agent_log("net_init: UDP bind to port %d failed, err=%d", NET_UDP_MEDIA_PORT, WSAGetLastError());
        /* Non-fatal: sending still works, just no UDP input receive */
    }

    /* Non-blocking so poll doesn't stall */
    u_long nb = 1;
    ioctlsocket(g_sock_udp, FIONBIO, &nb);

    InitializeCriticalSection(&g_cs_udp);
    int sndbuf = 2 * 1024 * 1024;
    setsockopt(g_sock_udp, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));

    agent_log("net_init: Winsock and UDP socket ready (bound port %d)", NET_UDP_MEDIA_PORT);
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

    if (listen(g_sock_tcp, 8) != 0) {
        closesocket(g_sock_tcp);
        g_sock_tcp = INVALID_SOCKET;
        return 0;
    }

    u_long mode = 1;
    ioctlsocket(g_sock_tcp, FIONBIO, &mode);
    agent_log("net_listen_control: listening on TCP %d", port);
    return 1;
}

void net_set_media_destination(const char *ip, uint16_t port) {
    memset(&g_media_dest, 0, sizeof(g_media_dest));
    g_media_dest.sin_family = AF_INET;
    g_media_dest.sin_addr.s_addr = inet_addr(ip);
    g_media_dest.sin_port = htons(port);
    g_has_media_dest = 1;
    agent_log("net_set_media_destination: target %s:%d", ip, port);
}

int net_is_connected(void) {
    return (g_client_tcp != INVALID_SOCKET);
}

int net_connect_to_server(const char *ip, uint16_t control_port, uint16_t media_port) {
    if (g_client_tcp != INVALID_SOCKET) {
        return 1; // Already connected
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        agent_log("net_connect_to_server: socket() failed");
        return 0;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(ip);
    sin.sin_port = htons(control_port);

    agent_log("net_connect_to_server: connecting to %s:%d...", ip, control_port);
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        agent_log("net_connect_to_server: connect failed (err=%d)", WSAGetLastError());
        closesocket(s);
        return 0;
    }

    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
    /* Disable Nagle — input events (mouse moves at ~125Hz) must not be
       coalesced into 40ms bursts by the TCP stack. */
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    g_client_tcp = s;
    net_set_media_destination(ip, media_port);
    g_tcp_buf_len = 0;
    g_is_authenticated = 1;
    g_is_streaming = 1;

    agent_log("net_connect_to_server: connected to %s:%d, streaming initialized", ip, control_port);
    send_hello_syn();
    return 1;
}

int net_is_streaming_active(void) {
    return (g_is_streaming && g_has_media_dest);
}

int net_send_audio(const uint8_t *pcm_data, uint32_t size, uint32_t pts_ms) {
    if (!net_is_streaming_active() || g_sock_udp == INVALID_SOCKET) return 0;

    uint32_t slice_size = (size + 1) / 2;
    uint8_t total_subs = (size > slice_size) ? 2 : 1;

    uint8_t packet[1500];
    uint32_t offset = 0;

    for (uint8_t sub = 0; sub < total_subs; sub++) {
        uint32_t this_slice = (size - offset > slice_size) ? slice_size : (size - offset);

        NetPacketHeader *nh = (NetPacketHeader *)packet;
        nh->magic = PKT_MAGIC;
        nh->pkt_type = PKT_TYPE_AUDIO;
        nh->flags = 0;
        nh->seq = g_seq_audio++;
        nh->pts_ms = pts_ms + (uint32_t)(sub * 5);
        nh->payload_len = (uint16_t)(sizeof(AudioSliceHeader) + this_slice);
        nh->reserved = 0;

        AudioSliceHeader *ah = (AudioSliceHeader *)(packet + sizeof(NetPacketHeader));
        ah->sub_seq = sub;
        ah->total_subs = total_subs;
        ah->sample_rate = 48000;
        ah->channels = 2;
        ah->bits_per_sample = 16;

        memcpy(packet + sizeof(NetPacketHeader) + sizeof(AudioSliceHeader), pcm_data + offset, this_slice);

        EnterCriticalSection(&g_cs_udp);
        int sent = sendto(g_sock_udp, (const char *)packet,
                          sizeof(NetPacketHeader) + sizeof(AudioSliceHeader) + this_slice,
                          0, (struct sockaddr *)&g_media_dest, sizeof(g_media_dest));
        if (sent <= 0) {
            Sleep(0);
            sendto(g_sock_udp, (const char *)packet,
                   sizeof(NetPacketHeader) + sizeof(AudioSliceHeader) + this_slice,
                   0, (struct sockaddr *)&g_media_dest, sizeof(g_media_dest));
        }
        LeaveCriticalSection(&g_cs_udp);
        offset += this_slice;
    }
    return 1;
}

int net_send_video_frame(const uint8_t *comp_data, uint32_t comp_size,
                         uint32_t frame_index, uint16_t width, uint16_t height,
                         uint8_t codec, uint8_t flags, uint32_t pts_ms) {
    if (!net_is_streaming_active() || g_sock_udp == INVALID_SOCKET) return 0;

    uint16_t total_chunks = (uint16_t)((comp_size + CHUNK_DATA_MAX - 1) / CHUNK_DATA_MAX);
    if (total_chunks == 0) total_chunks = 1;

    static int s_logged_video_send = 0;
    if (!s_logged_video_send) {
        agent_log("net_send_video_frame: first send! size=%d, chunks=%d, dest_active=%d",
                  comp_size, total_chunks, g_has_media_dest);
        s_logged_video_send = 1;
    }

    uint8_t packet[sizeof(NetPacketHeader) + sizeof(VideoChunkHeader) + CHUNK_DATA_MAX];
    uint32_t offset = 0;

    for (uint16_t c = 0; c < total_chunks; c++) {
        uint32_t this_chunk = (comp_size - offset > CHUNK_DATA_MAX) ? CHUNK_DATA_MAX : (comp_size - offset);

        NetPacketHeader *nh = (NetPacketHeader *)packet;
        nh->magic = PKT_MAGIC;
        nh->pkt_type = PKT_TYPE_VIDEO;
        nh->flags = (flags & VIDEO_FLAG_KEYFRAME) ? 1 : 0;
        nh->seq = g_seq_video++;
        nh->pts_ms = pts_ms;
        nh->payload_len = (uint16_t)(sizeof(VideoChunkHeader) + this_chunk);
        nh->reserved = 0;

        VideoChunkHeader *vh = (VideoChunkHeader *)(packet + sizeof(NetPacketHeader));
        vh->frame_index = frame_index;
        vh->chunk_index = c;
        vh->total_chunks = total_chunks;
        vh->frame_width = width;
        vh->frame_height = height;
        vh->codec = codec;
        vh->flags = flags;

        if (this_chunk > 0) {
            memcpy(packet + sizeof(NetPacketHeader) + sizeof(VideoChunkHeader), comp_data + offset, this_chunk);
        }

        EnterCriticalSection(&g_cs_udp);
        int sent = sendto(g_sock_udp, (const char *)packet,
                          sizeof(NetPacketHeader) + sizeof(VideoChunkHeader) + this_chunk,
                          0, (struct sockaddr *)&g_media_dest, sizeof(g_media_dest));
        if (sent <= 0) {
            Sleep(0);
            sendto(g_sock_udp, (const char *)packet,
                   sizeof(NetPacketHeader) + sizeof(VideoChunkHeader) + this_chunk,
                   0, (struct sockaddr *)&g_media_dest, sizeof(g_media_dest));
        }
        LeaveCriticalSection(&g_cs_udp);
        offset += this_chunk;

        // Micro-pace every 16 chunks to avoid overflowing the 1 Gbps NIC FIFO queue
        if ((c & 15) == 15) {
            Sleep(0);
        }

    }
    return 1;
}

int net_send_video_resize(uint16_t width, uint16_t height, uint8_t bpp) {
    MsgVideoResize msg;
    msg.new_width = width;
    msg.new_height = height;
    msg.new_bpp = bpp;
    agent_log("net_send_video_resize: %dx%d@%d", width, height, bpp);
    return send_tcp_msg(OP_VIDEO_RESIZE, &msg, sizeof(msg));
}

static void handle_control_frame(uint8_t opcode, const uint8_t *payload, uint16_t len,
                                net_stream_state_cb on_state_change, void *user_data) {
    switch (opcode) {
        case OP_AUTH_CHALLENGE: {
            agent_log("Control: OP_AUTH_CHALLENGE (len=%d)", len);
            send_tcp_msg(OP_AUTH_RESPONSE, payload, len);
            break;
        }
        case OP_AUTH_OK: {
            agent_log("Control: OP_AUTH_OK");
            g_is_authenticated = 1;
            g_is_streaming = 1;
            if (on_state_change) on_state_change(1, user_data);
            break;
        }
        case OP_AUTH_REJECT: {
            agent_log("Control: OP_AUTH_REJECT");
            g_is_authenticated = 0;
            g_is_streaming = 0;
            if (on_state_change) on_state_change(0, user_data);
            break;
        }
        case OP_PING: {
            send_tcp_msg(OP_PONG, payload, len);
            break;
        }
        case OP_STREAM_START: {
            agent_log("Control: OP_STREAM_START received!");
            g_is_streaming = 1;
            if (on_state_change) on_state_change(1, user_data);
            break;
        }
        case OP_STREAM_STOP: {
            agent_log("Control: OP_STREAM_STOP received!");
            g_is_streaming = 0;
            if (on_state_change) on_state_change(0, user_data);
            break;
        }
        case OP_INPUT_EVENT: {
            if (len >= sizeof(MsgInputEvent)) {
                const MsgInputEvent *ev = (const MsgInputEvent *)payload;
                switch (ev->event_type) {
                    case INPUT_TYPE_KEY:
                        input_inject_key(ev->param1, ev->key_down, 0);
                        break;
                    case INPUT_TYPE_MOUSE_REL:
                        input_inject_mouse_rel(ev->param2, ev->param3);
                        break;
                    case INPUT_TYPE_MOUSE_ABS:
                        input_inject_mouse_abs((uint16_t)ev->param2, (uint16_t)ev->param3);
                        break;
                    case INPUT_TYPE_MOUSE_BTN:
                        input_inject_mouse_btn(ev->param1, ev->key_down);
                        break;
                    case INPUT_TYPE_MOUSE_WHEEL:
                        input_inject_mouse_wheel((int16_t)ev->param1);
                        break;
                }
            }
            break;
        }
        default:
            break;
    }
}

void net_poll_control(net_stream_state_cb on_state_change, void *user_data) {
    if (g_sock_tcp == INVALID_SOCKET) return;

    if (g_client_tcp == INVALID_SOCKET) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET s = accept(g_sock_tcp, (struct sockaddr *)&client_addr, &addr_len);
        if (s != INVALID_SOCKET) {
            g_client_tcp = s;
            u_long mode = 1;
            ioctlsocket(g_client_tcp, FIONBIO, &mode);
            /* Disable Nagle for immediate input delivery */
            int nodelay = 1;
            setsockopt(g_client_tcp, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
            char *ip = inet_ntoa(client_addr.sin_addr);
            agent_log("Control: Client connected from %s", ip);

            net_set_media_destination(ip, NET_UDP_MEDIA_PORT);
            g_tcp_buf_len = 0;
            g_is_authenticated = 0;
            g_is_streaming = 0; // Wait for OP_STREAM_START before streaming

            send_hello_syn();
        }
    } else {
        int space = sizeof(g_tcp_buf) - g_tcp_buf_len;
        if (space > 0) {
            int r = recv(g_client_tcp, (char *)g_tcp_buf + g_tcp_buf_len, space, 0);
            if (r > 0) {
                g_tcp_buf_len += (uint32_t)r;

                while (g_tcp_buf_len >= sizeof(TcpFrameHeader)) {
                    TcpFrameHeader *hdr = (TcpFrameHeader *)g_tcp_buf;
                    uint32_t total_frame_len = sizeof(TcpFrameHeader) + hdr->payload_len;

                    if (g_tcp_buf_len >= total_frame_len) {
                        handle_control_frame(hdr->opcode, g_tcp_buf + sizeof(TcpFrameHeader),
                                             hdr->payload_len, on_state_change, user_data);

                        memmove(g_tcp_buf, g_tcp_buf + total_frame_len, g_tcp_buf_len - total_frame_len);
                        g_tcp_buf_len -= total_frame_len;
                    } else {
                        break;
                    }
                }
            } else if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
                agent_log("Control: Client disconnected");
                closesocket(g_client_tcp);
                g_client_tcp = INVALID_SOCKET;
                g_has_media_dest = 0;
                g_is_streaming = 0;
                g_is_authenticated = 0;
                g_tcp_buf_len = 0;
                if (on_state_change) on_state_change(0, user_data);
            }
        }
    }
}

void net_poll_udp_input(void) {
    if (g_sock_udp == INVALID_SOCKET) return;

    /* Drain pending UDP input datagrams without blocking.
       Coalesce mouse moves within the batch to prevent Win32 input queue flooding. */
    uint8_t buf[64];
    struct sockaddr_in from_addr;
    int from_len;

    int has_abs = 0;
    uint16_t last_abs_x = 0, last_abs_y = 0;
    int32_t accum_rel_dx = 0, accum_rel_dy = 0;

    for (int batch = 0; batch < 64; batch++) {
        from_len = sizeof(from_addr);
        int n = recvfrom(g_sock_udp, (char *)buf, sizeof(buf), 0,
                         (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0) break;

        /* Minimal validation: magic + pkt_type + at least sizeof(MsgInputEvent) payload */
        if (n < 3 || buf[0] != PKT_MAGIC) continue;

        if (buf[1] == PKT_TYPE_INPUT) {
            /* Layout: [magic(1)][pkt_type(1)][MsgInputEvent(8)] = 10 bytes */
            if (n >= 2 + (int)sizeof(MsgInputEvent)) {
                const MsgInputEvent *ev = (const MsgInputEvent *)(buf + 2);
                switch (ev->event_type) {
                    case INPUT_TYPE_KEY:
                        input_inject_key(ev->param1, ev->key_down, 0);
                        break;
                    case INPUT_TYPE_MOUSE_REL:
                        accum_rel_dx += ev->param2;
                        accum_rel_dy += ev->param3;
                        break;
                    case INPUT_TYPE_MOUSE_ABS:
                        has_abs = 1;
                        last_abs_x = (uint16_t)ev->param2;
                        last_abs_y = (uint16_t)ev->param3;
                        break;
                    case INPUT_TYPE_MOUSE_BTN:
                        // Flush any pending mouse motion before click
                        if (has_abs) {
                            input_inject_mouse_abs(last_abs_x, last_abs_y);
                            has_abs = 0;
                        }
                        if (accum_rel_dx != 0 || accum_rel_dy != 0) {
                            input_inject_mouse_rel((int16_t)accum_rel_dx, (int16_t)accum_rel_dy);
                            accum_rel_dx = 0;
                            accum_rel_dy = 0;
                        }
                        input_inject_mouse_btn(ev->param1, ev->key_down);
                        break;
                    case INPUT_TYPE_MOUSE_WHEEL:
                        input_inject_mouse_wheel((int16_t)ev->param1);
                        break;
                }
            }
        }
    }

    // Flush coalesced mouse motion at the end of the batch
    if (has_abs) {
        input_inject_mouse_abs(last_abs_x, last_abs_y);
    }
    if (accum_rel_dx != 0 || accum_rel_dy != 0) {
        if (accum_rel_dx > 32767) accum_rel_dx = 32767;
        if (accum_rel_dx < -32768) accum_rel_dx = -32768;
        if (accum_rel_dy > 32767) accum_rel_dy = 32767;
        if (accum_rel_dy < -32768) accum_rel_dy = -32768;
        input_inject_mouse_rel((int16_t)accum_rel_dx, (int16_t)accum_rel_dy);
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
    DeleteCriticalSection(&g_cs_udp);
    WSACleanup();
}
