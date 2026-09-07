#include <winsock2.h>
#include <windows.h>
#include "discover.h"
#include <stdio.h>
#include <stdlib.h>

static SOCKET g_sock_beacon = INVALID_SOCKET;
static discover_server_cb g_cb = NULL;
static void *g_cb_userdata = NULL;

int discover_init(discover_server_cb callback, void *user_data) {
    g_cb = callback;
    g_cb_userdata = user_data;

    g_sock_beacon = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock_beacon == INVALID_SOCKET) return 0;

    int opt = 1;
    setsockopt(g_sock_beacon, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(7022); // Discovery port

    if (bind(g_sock_beacon, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        closesocket(g_sock_beacon);
        g_sock_beacon = INVALID_SOCKET;
        return 0;
    }

    u_long mode = 1;
    ioctlsocket(g_sock_beacon, FIONBIO, &mode);
    return 1;
}

void discover_poll(void) {
    if (g_sock_beacon == INVALID_SOCKET) return;

    uint8_t buf[512];
    struct sockaddr_in from;
    int from_len = sizeof(from);

    int r = recvfrom(g_sock_beacon, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
    if (r >= 10 && buf[0] == 'X' && buf[1] == 'P' && buf[2] == 'D' && buf[3] == 0x01) {
        DiscoveredServer srv;
        memset(&srv, 0, sizeof(srv));
        strncpy(srv.ip, inet_ntoa(from.sin_addr), sizeof(srv.ip) - 1);
        srv.control_port = *(uint16_t *)(buf + 4);
        srv.media_port = *(uint16_t *)(buf + 6);

        uint8_t name_len = buf[8];
        if (name_len > 32) name_len = 32;
        if (r >= 9 + name_len) {
            memcpy(srv.server_name, buf + 9, name_len);
            srv.server_name[name_len] = '\0';

            int fp_offset = 9 + name_len;
            if (r >= fp_offset + 1 + 32) {
                uint8_t fp_len = buf[fp_offset];
                if (fp_len == 32) {
                    memcpy(srv.fingerprint, buf + fp_offset + 1, 32);
                }
            }

            if (g_cb) {
                g_cb(&srv, g_cb_userdata);
            }
        }
    }
}

int discover_is_trusted(const uint8_t *fingerprint) {
    (void)fingerprint;
    // Default: accept discovered servers in local subnet
    return 1;
}

void discover_shutdown(void) {
    if (g_sock_beacon != INVALID_SOCKET) {
        closesocket(g_sock_beacon);
        g_sock_beacon = INVALID_SOCKET;
    }
}
