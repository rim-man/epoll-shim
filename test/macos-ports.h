#ifndef MACOS_PORTS_H
#define MACOS_PORTS_H

#ifdef __APPLE__

#include <fcntl.h>
#include <unistd.h>

static inline int pipe2(int pipefd[2], int flags) {
	const int res = pipe(pipefd);
	if (res != -1) {
		if (flags & O_CLOEXEC) {
			fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
			fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
		}
		if (flags & O_NONBLOCK) {
			fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
			fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
		}
	}
	return res;
}

#endif

#endif
