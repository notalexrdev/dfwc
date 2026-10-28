/* See LICENSE file for copyright and license details.
 *
 * dfwcctl: run Lua in a running dfwc, or watch it. dfwc answers with one
 * status byte ('0' ok, '1' error) and the output, then hangs up; "watch"
 * instead keeps streaming a line whenever the workspace, mode or focus changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
die(const char *msg)
{
	perror(msg);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *path = getenv("DFWC_SOCK");
	char buf[4096];
	ssize_t n;
	int fd, first = 1, failed = 0;

	if (argc < 2 || !strcmp(argv[1], "-h")) {
		fprintf(stderr, "usage: dfwcctl 'lua code' | dfwcctl watch\n"
			"  dfwcctl 'dfwc.workspace(2)'   dfwcctl 'dfwc.windows()'\n");
		return argc < 2;
	}
	if (!path) {
		if (!getenv("XDG_RUNTIME_DIR") || !getenv("WAYLAND_DISPLAY")) {
			fputs("dfwcctl: not inside dfwc (no DFWC_SOCK)\n", stderr);
			return 1;
		}
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/dfwc-%s.sock",
			getenv("XDG_RUNTIME_DIR"), getenv("WAYLAND_DISPLAY"));
	} else {
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	}
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		die("dfwcctl: connect");
	/* the arguments, joined by spaces, are the whole request */
	for (int i = 1; i < argc; i++)
		if ((i > 1 && write(fd, " ", 1) != 1) || write(fd, argv[i], strlen(argv[i])) != (ssize_t)strlen(argv[i]))
			die("dfwcctl: write");
	shutdown(fd, SHUT_WR);
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (first && strcmp(argv[1], "watch")) { /* the status byte */
			failed = buf[0] == '1';
			fwrite(buf + 1, 1, n - 1, failed ? stderr : stdout);
		} else {
			fwrite(buf, 1, n, failed ? stderr : stdout);
		}
		first = 0;
		fflush(stdout);
	}
	return failed;
}
