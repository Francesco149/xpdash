#ifndef XPDASH_DISCOVER_H
#define XPDASH_DISCOVER_H

#include <windows.h>
#include <stdint.h>

typedef struct {
    char ip[16];
    uint16_t control_port;
    uint16_t media_port;
    char server_name[33];
    uint8_t fingerprint[32];
} DiscoveredServer;

typedef void (*discover_server_cb)(const DiscoveredServer *server, void *user_data);

/* Initialize UDP beacon listener on port 7022 */
int discover_init(discover_server_cb callback, void *user_data);

/* Poll for incoming UDP beacons (non-blocking) */
void discover_poll(void);

/* Check if a server fingerprint is trusted in agent.ini or trusted_servers.ini */
int discover_is_trusted(const uint8_t *fingerprint);

/* Cleanup discovery socket */
void discover_shutdown(void);

#endif /* XPDASH_DISCOVER_H */
