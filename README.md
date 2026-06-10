# fusee-rp2040
This is an implementation of the fusée gelée exploit for RP2040-based boards.
It should also work with other similar RP2040 based boards. Board-specific values are selected through the Pico SDK `PICO_BOARD` setting instead of source edits.
It is heavily based on the [excellent implementation of fusee-launcher by Qyriad](https://github.com/Qyriad/fusee-launcher),
as well as on the [implementation of fusee-launcher for samd21 by blockfeed](https://github.com/blockfeed/sam-fusee-launcher-internal/).

This project builds with the Pico SDK and TinyUSB host stack. GitHub Actions also builds the UF2 artifact on every push and pull request to `main`.

## Build
To build fusee-rp2040, first install the [Pico SDK](https://github.com/raspberrypi/pico-sdk), as outlined in [this Getting Started document](https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf).

After you are done, make sure you set the `PICO_SDK_PATH` environment variable to the correct value, and run these commands:
```sh
git clone https://github.com/Kozova1/fusee-rp2040
cd fusee-rp2040
cmake -B build
cmake --build build --parallel
```

After that, copy `build/src/fusee.uf2` to your board's internal memory, and it should hopefully work.

The payload and intermezzo binaries are assembled into a complete RCM image at build time. To use different binaries without editing source, pass their paths during configuration:
```sh
cmake -B build -DFUSEE_PAYLOAD_BIN=/path/to/payload.bin -DFUSEE_INTERMEZZO_BIN=/path/to/intermezzo.bin
cmake --build build --parallel
```

The default RP2040 board target is `waveshare_rp2040_one` for the Waveshare RP2040-One. To build for another Pico SDK-supported board, pass its board name during configuration:
```sh
cmake -B build -DPICO_BOARD=seeed_xiao_rp2040
cmake --build build --parallel
```

If your board definition does not describe the status LED correctly, you can override it without editing source:
```sh
cmake -B build -DFUSEE_STATUS_LED_PIN=25
cmake -B build -DFUSEE_STATUS_LED_PIN=25 -DFUSEE_STATUS_LED_INVERTED=ON
cmake -B build -DFUSEE_STATUS_LED_ENABLED=OFF
```

On boards with a colored status LED, launch progress is shown in blue, success in green, and failure in red.
On boards with only a single-color status LED, launch progress is shown with a slow blink, success with a solid light, and failure with a fast blink.
Success and failure indications are held for 3000 ms by default so they remain visible after USB host reset recovery.
To change the hold time:
```sh
cmake -B build -DFUSEE_STATUS_LED_HOLD_MS=5000
cmake --build build --parallel
```

UART debug logging is enabled by default and uses the Pico SDK board's default UART pins at 115200 baud. On Raspberry Pi Pico-style boards this is typically UART0 TX/RX on GPIO0/GPIO1. To build without UART logging:
```sh
cmake -B build -DFUSEE_DEBUG_UART=OFF
cmake --build build --parallel
```
