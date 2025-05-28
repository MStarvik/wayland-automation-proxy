CFLAGS += -Os -Wall -Wextra
LDFLAGS += -s

CFLAGS += `pkg-config --cflags wayland-client`
LDFLAGS += `pkg-config --libs wayland-client`

CFLAGS += `pkg-config --cflags wayland-server`
LDFLAGS += `pkg-config --libs wayland-server`

wayland-automation-proxy: main.o
	$(CC) -o $@ $^ $(LDFLAGS)
