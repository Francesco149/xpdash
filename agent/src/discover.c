#include <winsock2.h>
#include <windows.h>
#include "discover.h"
#include "sha256.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CACHED_FP 32

static SOCKET g_sock_beacon = INVALID_SOCKET;
static discover_server_cb g_cb = NULL;
static void *g_cb_userdata = NULL;

static char s_session_whitelist[MAX_CACHED_FP][72];
static int s_whitelist_count = 0;

static char s_session_blacklist[MAX_CACHED_FP][72];
static int s_blacklist_count = 0;

static void get_ini_paths(char *agent_ini, char *trusted_ini, size_t max_len) {
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) > 0) {
        char *last_slash = strrchr(exe_path, '\\');
        if (last_slash) {
            *(last_slash + 1) = '\0';
        } else {
            strcpy(exe_path, ".\\");
        }
    } else {
        strcpy(exe_path, "C:\\xpdash\\");
    }
    snprintf(agent_ini, max_len, "%sagent.ini", exe_path);
    snprintf(trusted_ini, max_len, "%strusted_servers.ini", exe_path);
}

static int is_in_cache(const char cache[MAX_CACHED_FP][72], int count, const char *fp) {
    for (int i = 0; i < count; i++) {
        if (_stricmp(cache[i], fp) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_to_cache(char cache[MAX_CACHED_FP][72], int *count, const char *fp) {
    if (is_in_cache(cache, *count, fp)) return;
    if (*count < MAX_CACHED_FP) {
        strncpy(cache[*count], fp, 71);
        cache[*count][71] = '\0';
        (*count)++;
    }
}

static int list_contains_fp(const char *list, const char *fp_str, const char *hex_hash, const char *raw_hex) {
    char buf[1024];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char *delim = ",; \t\r\n";
    char *token = strtok(buf, delim);
    while (token != NULL) {
        while (*token == ' ' || *token == '\t') token++;
        if (strcmp(token, "*") == 0 || _stricmp(token, "all") == 0) {
            return 1;
        }
        if (_stricmp(token, fp_str) == 0 ||
            _stricmp(token, hex_hash) == 0 ||
            _stricmp(token, raw_hex) == 0) {
            return 1;
        }
        token = strtok(NULL, delim);
    }
    return 0;
}

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
    agent_log("discover_init: listening for UDP beacons on port 7022");
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

int discover_is_trusted(const DiscoveredServer *server) {
    if (!server) return 0;

    // Compute SHA-256 fingerprint of the 32-byte Ed25519 public key
    uint8_t fp_hash[32];
    sha256_hash(server->fingerprint, 32, fp_hash);

    char fp_str[72];
    sha256_format_fingerprint(fp_hash, fp_str);

    char hex_hash[65];
    sha256_format_hex(fp_hash, hex_hash);

    char raw_hex[65];
    sha256_format_hex(server->fingerprint, raw_hex);

    // 1. Check in-memory session cache
    if (is_in_cache(s_session_whitelist, s_whitelist_count, fp_str)) {
        return 1;
    }
    if (is_in_cache(s_session_blacklist, s_blacklist_count, fp_str)) {
        return 0;
    }

    char agent_ini[MAX_PATH];
    char trusted_ini[MAX_PATH];
    get_ini_paths(agent_ini, trusted_ini, sizeof(agent_ini));

    // 2. Check agent.ini
    int allow_all = GetPrivateProfileIntA("security", "allow_all", 0, agent_ini);
    if (allow_all) {
        agent_log("Security: allow_all=1 in agent.ini, trusting server %s (%s)", server->server_name, fp_str);
        add_to_cache(s_session_whitelist, &s_whitelist_count, fp_str);
        return 1;
    }

    char trusted_list[1024];
    GetPrivateProfileStringA("security", "trusted_fingerprints", "", trusted_list, sizeof(trusted_list), agent_ini);
    if (trusted_list[0] != '\0') {
        if (list_contains_fp(trusted_list, fp_str, hex_hash, raw_hex)) {
            agent_log("Security: Matched trusted_fingerprints in agent.ini for %s (%s)", server->server_name, fp_str);
            add_to_cache(s_session_whitelist, &s_whitelist_count, fp_str);
            return 1;
        }
    }

    // 3. Check trusted_servers.ini
    char desc_buf[128];
    GetPrivateProfileStringA("trusted_servers", fp_str, "", desc_buf, sizeof(desc_buf), trusted_ini);
    if (desc_buf[0] != '\0') {
        agent_log("Security: Found %s in trusted_servers.ini (%s)", fp_str, desc_buf);
        add_to_cache(s_session_whitelist, &s_whitelist_count, fp_str);
        return 1;
    }

    GetPrivateProfileStringA("trusted_servers", hex_hash, "", desc_buf, sizeof(desc_buf), trusted_ini);
    if (desc_buf[0] != '\0') {
        agent_log("Security: Found %s in trusted_servers.ini (%s)", hex_hash, desc_buf);
        add_to_cache(s_session_whitelist, &s_whitelist_count, fp_str);
        return 1;
    }

    // 4. Interactive user confirmation dialog (public mode)
    int prompt_user = GetPrivateProfileIntA("security", "prompt_user", 1, agent_ini);
    if (!prompt_user) {
        agent_log("Security: Untrusted server %s (%s) and prompt_user=0, ignoring", server->server_name, fp_str);
        add_to_cache(s_session_blacklist, &s_blacklist_count, fp_str);
        return 0;
    }

    char prompt[512];
    snprintf(prompt, sizeof(prompt),
             "xpdash Security Alert\n\n"
             "Incoming connection from: \"%s\" (%s)\n"
             "Fingerprint: %s\n\n"
             "Do you want to trust this machine?\n\n"
             "• Yes: Trust Always (Save to trusted_servers.ini)\n"
             "• No: Trust Once (This session only)\n"
             "• Cancel: Reject",
             server->server_name, server->ip, fp_str);

    int ret = MessageBoxA(NULL, prompt, "xpdash Security Alert",
                          MB_YESNOCANCEL | MB_ICONQUESTION | MB_SYSTEMMODAL | MB_SETFOREGROUND);

    if (ret == IDYES) {
        agent_log("Security: User selected Trust Always for %s (%s)", server->server_name, fp_str);
        char entry[128];
        snprintf(entry, sizeof(entry), "%s (%s)", server->server_name, server->ip);
        WritePrivateProfileStringA("trusted_servers", fp_str, entry, trusted_ini);
        add_to_cache(s_session_whitelist, &s_whitelist_count, fp_str);
        return 1;
    } else if (ret == IDNO) {
        agent_log("Security: User selected Trust Once for %s (%s)", server->server_name, fp_str);
        add_to_cache(s_session_whitelist, &s_whitelist_count, fp_str);
        return 1;
    } else {
        agent_log("Security: User rejected connection from %s (%s)", server->server_name, fp_str);
        add_to_cache(s_session_blacklist, &s_blacklist_count, fp_str);
        return 0;
    }
}

void discover_shutdown(void) {
    if (g_sock_beacon != INVALID_SOCKET) {
        closesocket(g_sock_beacon);
        g_sock_beacon = INVALID_SOCKET;
    }
}
