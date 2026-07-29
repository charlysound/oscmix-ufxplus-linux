#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>

/* Open a socket described by addr.
 * addr format: "udp!host!port"  or a bare file descriptor number.
 * NOTE: sockopen modifies addr in-place. */
int sockopen(char *addr, int passive);

/* Extract the port number from an address string ("udp!host!port")
 * without modifying the string. Returns 0 if no port is found. */
uint16_t sockaddrport(const char *addr);

#endif
