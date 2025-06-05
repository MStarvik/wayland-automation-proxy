#define _GNU_SOURCE

// #include <wayland-server.h>
#include <wayland-client.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_FDS 28
#define BUFFER_LEN 4096
#define CONTROL_LEN (CMSG_LEN(MAX_FDS * sizeof(int32_t)))

typedef enum {
    IDLE = 0, // Do not record or replay events
    CAPTURE = 1, // Record events that result from user input (pointer, keyboard, touch)
    REPLAY = 2, // Replay recorded events
} wap_mode_t;

volatile sig_atomic_t running = 1;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        running = 0;
    }
}

static void timespec_sub(struct timespec *result, const struct timespec *a, const struct timespec *b) {
    result->tv_sec = a->tv_sec - b->tv_sec;
    if (a->tv_nsec < b->tv_nsec) {
        result->tv_sec--;
        result->tv_nsec = a->tv_nsec + 1000000000 - b->tv_nsec;
    } else {
        result->tv_nsec = a->tv_nsec - b->tv_nsec;
    }
}

static bool timespec_leq(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) {
        return true;
    } else if (a->tv_sec > b->tv_sec) {
        return false;
    } else {
        return a->tv_nsec <= b->tv_nsec;
    }
}

static void print_usage(const char *progname) {
    fprintf(stderr, "Usage: %s [options] <command>\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c          Capture events (default behavior)\n");
    fprintf(stderr, "  -r          Replay captured events\n");
    fprintf(stderr, "  -h          Show this help message and exit\n");
}

int main(int argc, char *argv[]) {
    char in_buffer[BUFFER_LEN];
    char out_buffer[BUFFER_LEN];
    char control[CONTROL_LEN];

    int client_fd = -1;
    int upstream_fd = -1;

    uint32_t wl_registry_id = 0;
    uint32_t wl_seat_id = 0;
    uint32_t wl_pointer_id = 0;
    uint32_t wl_keyboard_id = 0;
    uint32_t wl_touch_id = 0;

    wap_mode_t mode = CAPTURE;

    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == '-' && argv[i][2] == '\0') {
                i++;
                break;
            } else if (argv[i][1] == 'c' && argv[i][2] == '\0') {
                mode = CAPTURE;
            } else if (argv[i][1] == 'r' && argv[i][2] == '\0') {
                mode = REPLAY;
            } else if (argv[i][1] == 'h' && argv[i][2] == '\0') {
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            } else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        } else {
            break;
        }
    }

    if (i >= argc) {
        print_usage(argv[0]);
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

        // Make the child process think we are the compositor
        if (setenv("WAYLAND_DISPLAY", downstream_display, 1) < 0) {
            perror("setenv");
            exit(EXIT_FAILURE);
        }

        // Redirect STDIN of the child process to /dev/null
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            perror("open /dev/null");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("dup2 stdin");
            exit(EXIT_FAILURE);
        }
        close(fd);

        // Redirect STDOUT of the child process to out.log
        fd = open("out.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            perror("open out.log");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2 stdout");
            exit(EXIT_FAILURE);
        }
        close(fd);

        // Redirect STDERR of the child process to err.log
        fd = open("err.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            perror("open err.log");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDERR_FILENO) < 0) {
            perror("dup2 stderr");
            exit(EXIT_FAILURE);
        }
        close(fd);

        if (execvp(argv[i], &argv[i]) < 0) {
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    }

    int log_fd = -1;
    struct timespec t1;
    if (mode == CAPTURE) {
        log_fd = open("events.bin", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (log_fd < 0) {
            perror("open event log for writing");
            close(server_fd);
            unlink(downstream_addr.sun_path);
            return EXIT_FAILURE;
        }
    } else if (mode == REPLAY) {
        log_fd = open("events.bin", O_RDONLY | O_CLOEXEC);
        if (log_fd < 0) {
            perror("open event log for reading");
            close(server_fd);
            unlink(downstream_addr.sun_path);
            return EXIT_FAILURE;
        }

        ssize_t n = read(log_fd, &t1, sizeof(t1));
        if (n == 0) {
            fprintf(stderr, "End of event log reached\n");
            mode = IDLE;
        } else if (n != sizeof(t1)) {
            perror("read event log");
            close(log_fd);
            close(server_fd);
            unlink(downstream_addr.sun_path);
            return EXIT_FAILURE;
        }
    }

    signal(SIGINT, signal_handler);

    struct timespec t0 = {0, 0}; // Initalize to silence compiler warnings
    struct timespec t;
    int ret = EXIT_SUCCESS;
    while (running) {
        int nfds = 1;
        struct timespec timeout;
        struct timespec *timeout_ptr = NULL;
        if (client_fd >= 0) {
            nfds = 3;

            if (mode == REPLAY) {
                struct timespec dt;
                timespec_sub(&dt, &t, &t0);
                timespec_sub(&timeout, &t1, &dt);
                timeout_ptr = &timeout;
            }
        }

        int nevents = ppoll(fds, nfds, timeout_ptr, NULL);
        if (nevents < 0) {
            if (errno == EINTR) {
                break;
            }
            perror("poll");
            ret = EXIT_FAILURE;
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &t);

        if (fds[0].revents & POLLIN) {
            if (client_fd >= 0) {
                fprintf(stderr, "Unexpected client connection while already connected\n");
                continue;
            }

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

            t0 = t;
        }

        // Handle messages from the client (requests)
        if (fds[1].revents & POLLIN) {
            struct iovec iov = {
                .iov_base = in_buffer,
                .iov_len = sizeof(in_buffer)
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
                uint32_t *p = (uint32_t *)in_buffer;
                uint32_t *end = (uint32_t *)(in_buffer + n);
                while (p < end) {
                    uint32_t id = p[0];
                    uint16_t opcode = p[1] & 0xFFFF;
                    uint16_t size = p[1] >> 16;
                    if (id == 1) { // wl_display
                        if (opcode == 1) { // wl_display.get_registry
                            wl_registry_id = p[2];
                        }
                    } else if (id == wl_registry_id) { // wl_registry
                        if (opcode == 0) { // wl_registry.bind
                            // uint32_t name = p[2];
                            uint32_t interface_len = p[3];
                            const char *interface = (const char *)(p + 4);
                            // uint32_t version = p[4 + (interface_len + 3) / 4];
                            uint32_t new_id = p[4 + (interface_len + 3) / 4 + 1];

                            if (strcmp(interface, wl_seat_interface.name) == 0) {
                                wl_seat_id = new_id;
                            }
                        }
                    } else if (id == wl_seat_id) { // wl_seat
                        if (opcode == 0) { // wl_seat.get_pointer
                            uint32_t new_id = p[2];
                            wl_pointer_id = new_id;
                        } else if (opcode == 1) { // wl_seat.get_keyboard
                            uint32_t new_id = p[2];
                            wl_keyboard_id = new_id;
                        } else if (opcode == 2) { // wl_seat.get_touch
                            uint32_t new_id = p[2];
                            wl_touch_id = new_id;
                        }
                    }

                    p += size / 4;
                }

                // Only forward the number of bytes we actually received from the client
                iov.iov_len = n;

                // Forward the message we received from the client to the compositor
                if (sendmsg(upstream_fd, &msg, 0) < 0) {
                    perror("sendmsg to upstream");
                    break;
                }

                // If we received any file descriptors, we need to close them
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

        // Handle messages from the compositor (events)
        if (fds[2].revents & POLLIN) {
            struct timespec dt;
            timespec_sub(&dt, &t, &t0);

            struct iovec in_iov = {
                .iov_base = in_buffer,
                .iov_len = sizeof(in_buffer)
            };
            struct iovec out_iov = {
                .iov_base = out_buffer,
                .iov_len = 0
            };
            struct msghdr msg = {
                .msg_name = NULL,
                .msg_namelen = 0,
                .msg_iov = &in_iov,
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
                uint32_t *p = (uint32_t *)in_buffer;
                uint32_t *end = (uint32_t *)(in_buffer + n);
                while (p < end) {
                    // When we are in TEST mode, user input events coming from the
                    // compositor are blocked to prevent the user from putting the
                    // program under test into an unexpected state.
                    bool accept = true;

                    uint32_t id = p[0];
                    uint16_t opcode = p[1] & 0xFFFF;
                    uint16_t size = p[1] >> 16;
                    if (id == wl_pointer_id) { // wl_pointer
                        if (mode == CAPTURE) {
                            write(log_fd, &dt, sizeof(dt));
                            write(log_fd, p, size);
                        } else if (mode == REPLAY) {
                            accept = false;
                        }
                    } else if (id == wl_keyboard_id) { // wl_keyboard
                        if (opcode >= 1 && opcode <= 4) { //wl_keyboard.{enter,leave,key,modifiers}
                            if (mode == CAPTURE) {
                                write(log_fd, &dt, sizeof(dt));
                                write(log_fd, p, size);
                            } else if (mode == REPLAY) {
                                accept = false;
                            }
                        }
                    } else if (id == wl_touch_id) { // wl_touch
                        if (mode == CAPTURE) {
                            write(log_fd, &dt, sizeof(dt));
                            write(log_fd, p, size);
                        } else if (mode == REPLAY) {
                            accept = false;
                        }
                    }

                    if (accept) {
                        memcpy(out_buffer + out_iov.iov_len, p, size);
                        out_iov.iov_len += size;
                    }

                    p += size / 4;
                }

                if (out_iov.iov_len > 0) {
                    // Important: None of the message types we care about
                    // contain file descriptors, so we don't touch the
                    // ancillary data. If we want to block messages that do,
                    // then we would have to keep track of which FD belongs to
                    // which message, and remove FDs that belong to blocked
                    // messages.

                    msg.msg_iov = &out_iov;

                    // Forward the message we received from the compositor to the client
                    if (sendmsg(client_fd, &msg, 0) < 0) {
                        perror("sendmsg to client");
                        ret = EXIT_FAILURE;
                        break;
                    }
                }

                // If we received any file descriptors, we need to close them
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

        // Playback of recorded events
        if (mode == REPLAY && client_fd >= 0) {
            struct timespec dt;
            timespec_sub(&dt, &t, &t0);

            while (timespec_leq(&t1, &dt)) {
                ssize_t n = read(log_fd, out_buffer, 8);
                if (n == 0) {
                    fprintf(stderr, "End of event log reached\n");
                    mode = IDLE;
                    break;
                } else if (n != 8) {
                    perror("read event log");
                    ret = EXIT_FAILURE;
                    goto cleanup;
                }

                uint32_t *p = (uint32_t *)out_buffer;
                // uint32_t id = p[0];
                // uint16_t opcode = p[1] & 0xFFFF;
                uint16_t size = p[1] >> 16;

                if (size < 8 || size > sizeof(in_buffer)) {
                    fprintf(stderr, "Invalid event size: %u\n", size);
                    ret = EXIT_FAILURE;
                    goto cleanup;
                }

                if (size > 8) {
                    n = read(log_fd, out_buffer + 8, size - 8);
                    if (n == 0) {
                        fprintf(stderr, "End of event log reached\n");
                        mode = IDLE;
                        break;
                    } else if (n != size - 8) {
                        perror("read event log");
                        ret = EXIT_FAILURE;
                        goto cleanup;
                    }
                }

                struct iovec iov = {
                    .iov_base = out_buffer,
                    .iov_len = size
                };
                struct msghdr msg = {
                    .msg_name = NULL,
                    .msg_namelen = 0,
                    .msg_iov = &iov,
                    .msg_iovlen = 1,
                    .msg_control = NULL,
                    .msg_controllen = 0,
                    .msg_flags = 0
                };
                if (sendmsg(client_fd, &msg, 0) < 0) {
                    perror("sendmsg to client");
                    ret = EXIT_FAILURE;
                    goto cleanup;
                }

                n = read(log_fd, &t1, sizeof(t1));
                if (n == 0) {
                    fprintf(stderr, "End of event log reached\n");
                    mode = IDLE;
                    break;
                } else if (n != sizeof(t1)) {
                    perror("read event log");
                    ret = EXIT_FAILURE;
                    goto cleanup;
                }
            }
        }
    }

    cleanup:

    if (log_fd >= 0) {
        close(log_fd);
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
