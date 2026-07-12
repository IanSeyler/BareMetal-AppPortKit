// webc.c -- based on helloc.c. Fetches a page over plain HTTP via the
// net_shim/lwIP TCP path and prints the raw response (headers + HTML)
// to stdout, proving musl -> posix_shim -> net_shim -> lwIP works
// end to end. No DNS in this port (see OPENISSUES.md), so the target
// is a literal IPv4 address. Build with build-app.sh.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST "1.1.1.1"
#define PORT 80

int main(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		printf("socket() failed: %d\n", fd);
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	if (inet_pton(AF_INET, HOST, &addr.sin_addr) != 1) {
		printf("inet_pton() failed for %s\n", HOST);
		return 1;
	}

	printf("Connecting to %s:%d...\n", HOST, PORT);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("connect() failed\n");
		close(fd);
		return 1;
	}
	printf("Connected. Sending request...\n\n");

	static const char req[] =
		"GET / HTTP/1.0\r\n"
		"Host: " HOST "\r\n"
		"Connection: close\r\n"
		"\r\n";

	size_t sent = 0;
	while (sent < sizeof(req) - 1) {
		long n = send(fd, req + sent, sizeof(req) - 1 - sent, 0);
		if (n <= 0) {
			printf("send() failed\n");
			close(fd);
			return 1;
		}
		sent += (size_t)n;
	}

	char buf[512];
	long n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = '\0';
		printf("%s", buf);
	}

	printf("\n\n-- connection closed --\n");
	close(fd);

	return 0;
}
