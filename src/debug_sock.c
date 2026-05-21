/* Bare-minimum TCP debugger socket for KEGS.
 *
 *   - Listens on 127.0.0.1:<port>.  Single client, posix-only.
 *   - Each line received from the client is run through do_debug_cmd(),
 *     so any KEGS monitor command works ('e1/c50aB' to set a breakpoint,
 *     'e1/0010.0020' to dump memory, 'g' to go, 's' to step, 'q' to dump
 *     registers, etc.  See debugger_help in debugger.c for the full list.)
 *   - Two extra commands handled before do_debug_cmd:
 *           halt           -> force the emulator into the debugger
 *           bye / quit     -> close the client socket
 *   - All debugger output is mirrored to the client via the tap in
 *     debug_add_output_string (debugger.c).  That means breakpoint hits
 *     ("Hit breakpoint at ...") are pushed asynchronously without polling.
 *   - After each command completes, the line "(kegs)\n" is sent as a
 *     prompt marker so the client knows the response is finished.
 */

#include "defc.h"

#include "debug_sock.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

#define DBG_SOCK_LINE_SIZE      1024

static int  g_dbg_listen_fd = -1;
static int  g_dbg_client_fd = -1;
static char g_dbg_line[DBG_SOCK_LINE_SIZE];
static int  g_dbg_line_pos = 0;

static void
dbg_sock_close_client(void)
{
	if(g_dbg_client_fd >= 0) {
		close(g_dbg_client_fd);
		g_dbg_client_fd = -1;
	}
	g_dbg_line_pos = 0;
}

static void
dbg_sock_raw_send(const char *buf, int len)
{
	ssize_t n;

	while(len > 0) {
		n = send(g_dbg_client_fd, buf, len, 0);
		if(n < 0) {
			if(errno == EINTR) {
				continue;
			}
			if(errno == EAGAIN || errno == EWOULDBLOCK) {
				/* Client is too slow; drop the rest of this
				 * write rather than blocking the emulator. */
				return;
			}
			dbg_sock_close_client();
			return;
		}
		if(n == 0) {
			dbg_sock_close_client();
			return;
		}
		buf += n;
		len -= (int)n;
	}
}

static void
dbg_sock_send_str(const char *s)
{
	if(g_dbg_client_fd < 0) {
		return;
	}
	dbg_sock_raw_send(s, (int)strlen(s));
}

static void
dbg_sock_send_prompt(void)
{
	dbg_sock_send_str("(kegs)\n");
}

void
debug_sock_init(int port)
{
	struct sockaddr_in sa;
	int on, flags;

	if(g_dbg_listen_fd >= 0) {
		close(g_dbg_listen_fd);
		g_dbg_listen_fd = -1;
	}

	/* Make sure a closed-client send doesn't take down the emulator. */
	signal(SIGPIPE, SIG_IGN);

	g_dbg_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(g_dbg_listen_fd < 0) {
		printf("debug_sock: socket() failed, errno=%d\n", errno);
		return;
	}

	on = 1;
	setsockopt(g_dbg_listen_fd, SOL_SOCKET, SO_REUSEADDR,
				(char *)&on, sizeof(on));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if(bind(g_dbg_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("debug_sock: bind(127.0.0.1:%d) failed, errno=%d\n",
							port, errno);
		close(g_dbg_listen_fd);
		g_dbg_listen_fd = -1;
		return;
	}

	if(listen(g_dbg_listen_fd, 1) < 0) {
		printf("debug_sock: listen() failed, errno=%d\n", errno);
		close(g_dbg_listen_fd);
		g_dbg_listen_fd = -1;
		return;
	}

	flags = fcntl(g_dbg_listen_fd, F_GETFL, 0);
	if(flags >= 0) {
		fcntl(g_dbg_listen_fd, F_SETFL, flags | O_NONBLOCK);
	}

	printf("debug_sock: listening on 127.0.0.1:%d\n", port);
}

static void
dbg_sock_accept(void)
{
	int fd, flags;

	fd = accept(g_dbg_listen_fd, NULL, NULL);
	if(fd < 0) {
		return;
	}
	if(g_dbg_client_fd >= 0) {
		/* Already have a client - reject this one. */
		const char *msg = "kegs: busy, debugger already attached\n";
		(void)send(fd, msg, strlen(msg), 0);
		close(fd);
		return;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if(flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	g_dbg_client_fd = fd;
	g_dbg_line_pos = 0;
	dbg_sock_send_str("kegs debug socket connected\n");
	dbg_sock_send_prompt();
}

static void
dbg_sock_run_cmd(char *line)
{
	while(*line == ' ' || *line == '\t') {
		line++;
	}
	if(line[0] == 0) {
		dbg_sock_send_prompt();
		return;
	}
	if(strcmp(line, "halt") == 0) {
		set_halt(1);
		dbg_sock_send_str("halted\n");
		dbg_sock_send_prompt();
		return;
	}
	if(strcmp(line, "bye") == 0 || strcmp(line, "quit") == 0) {
		dbg_sock_send_str("bye\n");
		dbg_sock_close_client();
		return;
	}
	do_debug_cmd(line);
	dbg_sock_send_prompt();
}

static void
dbg_sock_recv(void)
{
	char buf[1024];
	ssize_t n;
	int i;
	char c;

	n = recv(g_dbg_client_fd, buf, sizeof(buf), 0);
	if(n == 0) {
		dbg_sock_close_client();
		return;
	}
	if(n < 0) {
		if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return;
		}
		dbg_sock_close_client();
		return;
	}
	for(i = 0; i < n; i++) {
		c = buf[i];
		if(c == '\r') {
			continue;
		}
		if(c == '\n') {
			g_dbg_line[g_dbg_line_pos] = 0;
			g_dbg_line_pos = 0;
			dbg_sock_run_cmd(g_dbg_line);
			if(g_dbg_client_fd < 0) {
				return;
			}
		} else if(g_dbg_line_pos < DBG_SOCK_LINE_SIZE - 1) {
			g_dbg_line[g_dbg_line_pos++] = c;
		}
		/* Overlong lines silently truncate. */
	}
}

void
debug_sock_poll(void)
{
	if(g_dbg_listen_fd < 0) {
		return;
	}
	if(g_dbg_client_fd < 0) {
		dbg_sock_accept();
	} else {
		dbg_sock_recv();
	}
}

void
debug_sock_write(const char *str, int len)
{
	if(g_dbg_client_fd < 0) {
		return;
	}
	if(len <= 0) {
		len = (int)strlen(str);
	}
	dbg_sock_raw_send(str, len);
	dbg_sock_raw_send("\n", 1);
}
