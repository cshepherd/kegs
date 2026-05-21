/* Bare-minimum TCP debugger socket for KEGS.  Posix only, localhost,
 * single-client.  See debug_sock.c for the wire protocol. */

#ifndef KEGS_DEBUG_SOCK_H
#define KEGS_DEBUG_SOCK_H

void debug_sock_init(int port);
void debug_sock_poll(void);
void debug_sock_write(const char *str, int len);

#endif
