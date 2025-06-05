# wayland-automation-proxy
wayland-automation-proxy enables automation of wayland applications by acting as a middleman between the application and the compositor. The proxy blocks keyboard and pointer events from the user, and injects events from a file instead.

There are (at least) two ways to do automated testing of graphical applications under Wayland:
1. Using a proxy between the application and the compositor
2. Using a (possibly nested) compositor that supports automation

The latter approach is far more powerful, as the compositor has full control over the events are sent to the application, including user input. The compositor also controls the size and scale of the application's toplevels and which outputs they are rendered on. A purpose-built compositor can expose any number of outputs with any size and scale, without the need for a physical (or virtual) display. This makes it possible to do automated testing of graphical applications on machines without display capabilities like virtual machines and containers. As far as the author is aware, no such compositor currently exists. This project is an example of the former approach, which is simpler to implement, but more limited in its capabilities.

Both approaches have some advantages over tools such as `ydotool` in the context of automated testing, most notably that testing can be done without interfering with the user's session. This means that multiple tests can be run at the same time, and that the user can continue to use the system while tests are running.

## Features
- Capture user input (mouse, keyboard, touch)
- Replay captured input
- Block user input while replaying

## Limitations
- Currently, only a single wayland connection is supported. The first connection that is established must be used for the entire lifetime of the application. Multiple connections may be supported in the future. 
- Object ids are recorded and replayed as-is, which means that the application must assign the same object ids to the same objects every time it is run. This seems to be the case for most applications, but it is in no way required by the Wayland protocol. If 

## Building
The only dependency is libwayland-client, which should be installed on all systems with a wayland compositor. To build, just run `make`.

## Usage
```bash
Usage: wayland-automation-proxy [options] <command>
Options:
  -c          Capture events (default behavior)
  -r          Replay captured events
  -h          Show this help message and exit
```
In capture mode, events are stored in `events.bin` in the current directory. STDOUT and STERR of the application are redirected to `out.log` and `err.log` respectively. In replay mode, events are read from `events.bin`. After all events have been replayed, the application starts 
