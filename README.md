# [WIP] wayland-automation-proxy
wayland-automation-proxy enables automation of wayland applications by acting as a middleman between the application and the compositor. The proxy blocks keyboard and pointer events from the user, and injects events from a file instead.

There are (at least) two ways to do automated testing of graphical applications under Wayland:
1. Using a proxy between the application and the compositor
2. Using a (possibly nested) compositor that supports automation

The latter approach is far more powerful, as the compositor has full control over the events are sent to the application, including user input. The compositor also controls the size and scale of the application's toplevels and which outputs they are rendered on. A purpose-built compositor can expose any number of outputs with any size and scale, without the need for a physical (or virtual) display. This makes it possible to do automated testing of graphical applications on machines without display capabilities like virtual machines and containers. As far as the author is aware, no such compositor currently exists. This project is an example of the former approach, which is simpler to implement, but more limited in its capabilities.

## Features
- Capture user input (mouse, keyboard, touch)
- Replay captured input
- Block user input while replaying

## Planned features
- Support for blocking or downgrading specific interfaces to simulate older or more limited compositors
