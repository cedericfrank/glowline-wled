# Glowline usermod

Minimal diagnostic usermod. It does not touch LEDs or WLED state — every 5 seconds
it prints a heartbeat line and the current free heap to Serial:

```
glowline usermod alive, free heap: 192972
```

## Installation

Copy `platformio_override.ini.sample` to the repository root as `platformio_override.ini`
(the same directory as `platformio.ini`). That file is gitignored, so this is a
one-time local step needed to include the usermod in a build — without it, `glowline`
is not compiled in.

```
cp usermods/glowline/platformio_override.ini.sample platformio_override.ini
```

This defines a new `esp32dev_glowline` PlatformIO environment that extends `esp32dev`
and adds `glowline` to `custom_usermods`. Build and flash it with:

```
pio run -e esp32dev_glowline -t upload
```
