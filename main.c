#define _GNU_SOURCE

#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_FDS 28
#define BLEN 4096
#define CLEN (CMSG_LEN(MAX_FDS * sizeof(int32_t)))

volatile sig_atomic_t running = 1;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        running = 0;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *upstream_display = getenv("WAYLAND_DISPLAY");
    if (upstream_display == NULL) {
        fprintf(stderr, "WAYLAND_DISPLAY is not set\n");
        return EXIT_FAILURE;
    }

    const char *downstream_display = "wayland-2";

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == NULL) {
        fprintf(stderr, "XDG_RUNTIME_DIR is not set\n");
        return EXIT_FAILURE;
    }

    int client_fd = -1;
    int upstream_fd = -1;

    int server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        perror("socket downstream");
        return EXIT_FAILURE;
    }

    struct sockaddr_un downstream_addr;
    downstream_addr.sun_family = AF_UNIX;
    snprintf(downstream_addr.sun_path, sizeof(downstream_addr.sun_path), "%s/%s", runtime_dir, downstream_display);
    unlink(downstream_addr.sun_path);

    if (bind(server_fd, (struct sockaddr *)&downstream_addr, sizeof(downstream_addr)) < 0) {
        perror("bind downstream");
        close(server_fd);
        unlink(downstream_addr.sun_path);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen downstream");
        close(server_fd);
        unlink(downstream_addr.sun_path);
        return EXIT_FAILURE;
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl");
        close(server_fd);
        unlink(downstream_addr.sun_path);
        return EXIT_FAILURE;
    }

    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl set O_NONBLOCK");
        close(server_fd);
        unlink(downstream_addr.sun_path);
        return EXIT_FAILURE;
    }

    struct pollfd fds[3];
    fds[0].fd = server_fd;
    fds[0].events = POLLIN;
    fds[1].fd = client_fd;
    fds[1].events = POLLIN;
    fds[2].fd = upstream_fd;
    fds[2].events = POLLIN;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(server_fd);
        unlink(downstream_addr.sun_path);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        close(server_fd);

        if (setenv("WAYLAND_DISPLAY", downstream_display, 1) < 0) {
            perror("setenv");
            exit(EXIT_FAILURE);
        }

        if (execvp(argv[1], &argv[1]) < 0) {
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    }

    signal(SIGINT, signal_handler);

    int ret = EXIT_SUCCESS;
    while (running) {
        int nfds = client_fd >= 0 ? 3 : 1;
        int nevents = ppoll(fds, nfds, NULL, NULL);
        if (nevents < 0) {
            if (errno == EINTR) {
                break;
            }
            perror("poll");
            ret = EXIT_FAILURE;
            break;
        }
            
        if (fds[0].revents & POLLIN) {
            client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                perror("accept");
                ret = EXIT_FAILURE;
                break;
            }

            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags < 0) {
                perror("fcntl");
                ret = EXIT_FAILURE;
                break;
            }

            if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                perror("fcntl set O_NONBLOCK");
                ret = EXIT_FAILURE;
                break;
            }

            upstream_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (upstream_fd < 0) {
                perror("socket upstream");
                ret = EXIT_FAILURE;
                break;
            }

            flags = fcntl(upstream_fd, F_GETFL, 0);
            if (flags < 0) {
                perror("fcntl");
                ret = EXIT_FAILURE;
                break;
            }

            if (fcntl(upstream_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                perror("fcntl set O_NONBLOCK");
                ret = EXIT_FAILURE;
                break;
            }

            struct sockaddr_un upstream_addr;
            upstream_addr.sun_family = AF_UNIX;
            snprintf(upstream_addr.sun_path, sizeof(upstream_addr.sun_path), "%s/%s", runtime_dir, upstream_display);
            if (connect(upstream_fd, (struct sockaddr *)&upstream_addr, sizeof(upstream_addr)) < 0) {
                perror("connect upstream");
                ret = EXIT_FAILURE;
                break;
            }

            fds[1].fd = client_fd;
            fds[2].fd = upstream_fd;
        }

        if (fds[1].revents & POLLIN) {
            char buffer[BLEN];
            char control[CLEN];

            struct iovec iov = {
                .iov_base = buffer,
                .iov_len = sizeof(buffer)
            };

            struct msghdr msg = {
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = control,
                .msg_controllen = sizeof(control)
            };

            ssize_t n = recvmsg(client_fd, &msg, 0);
            if (n < 0) {
                perror("recvmsg from client");
                ret = EXIT_FAILURE;
                break;
            } else if (n == 0) {
                close(client_fd);
                client_fd = -1;
                fds[1].fd = -1;

                close(upstream_fd);
                upstream_fd = -1;
                fds[2].fd = -1;

                break;
            } else {
                iov.iov_len = n;

                if (sendmsg(upstream_fd, &msg, 0) < 0) {
                    perror("sendmsg to upstream");
                    break;
                }

                struct cmsghdr *cmsg;
                for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                        int *fds = (int *)CMSG_DATA(cmsg);
                        int nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int32_t);
                        for (int i = 0; i < nfds; i++) {
                            close(fds[i]);
                        }
                    }
                }
            }
        }

        if (fds[2].revents & POLLIN) {
            char buffer[BLEN];
            char control[CLEN];

            struct iovec iov = {
                .iov_base = buffer,
                .iov_len = sizeof(buffer)
            };

            struct msghdr msg = {
                .msg_name = NULL,
                .msg_namelen = 0,
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = control,
                .msg_controllen = sizeof(control),
                .msg_flags = 0
            };

            ssize_t n = recvmsg(upstream_fd, &msg, 0);
            if (n < 0) {
                perror("recvmsg from upstream");
                ret = EXIT_FAILURE;
                break;
            } else if (n == 0) {
                close(upstream_fd);
                upstream_fd = -1;
                fds[2].fd = -1;

                close(client_fd);
                client_fd = -1;
                fds[1].fd = -1;

                break;
            } else {
                iov.iov_len = n;

                if (sendmsg(client_fd, &msg, 0) < 0) {
                    perror("sendmsg to client");
                    ret = EXIT_FAILURE;
                    break;
                }

                struct cmsghdr *cmsg;
                for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                        int *fds = (int *)CMSG_DATA(cmsg);
                        int nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int32_t);
                        for (int i = 0; i < nfds; i++) {
                            close(fds[i]);
                        }
                    }
                }
            }
        }
    }

    if (upstream_fd >= 0) {
        close(upstream_fd);
    }

    if (client_fd >= 0) {
        close(client_fd);
    }

    close(server_fd);
    unlink(downstream_addr.sun_path);

    return ret;
}
