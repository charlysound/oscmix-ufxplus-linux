#ifndef MDNS_H
#define MDNS_H

#include <stdint.h>

/* Register an OSC service via mDNS/DNS-SD.
 *
 * name  - human-readable service instance name
 *         e.g. "oscmix @ Fireface UCX II (12345678)"
 * port  - UDP port clients send commands to (recvport)
 * txt   - NULL-terminated array of "key=value" strings
 *
 * Returns 0 on success, -1 on error.
 * Must be called after device info is available (i.e. after init()). */
int mdns_register(const char *name, uint16_t port, const char *txt[]);

/* Unregister the previously registered service and release resources. */
void mdns_unregister(void);

#endif
