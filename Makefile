CFLAGS += -Os -Wall -Wextra
LDFLAGS += -s

CFLAGS += `pkg-config --cflags wayland-client`
LDFLAGS += `pkg-config --libs wayland-client`

wayland-automation-proxy: wayland-automation-proxy.o

.PHONY: clean
clean:
	rm -f wayland-automation-proxy wayland-automation-proxy.o
