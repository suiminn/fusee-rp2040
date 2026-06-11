# fusee-rp2040

fusee-rp2040 turns an RP2040-based board into a small USB host that launches a
Nintendo Switch RCM payload using the fusee gelee exploit.

The default build embeds the latest standard
[`hekate_ctcaer_<version>.bin`](https://github.com/CTCaer/hekate/releases)
payload at build time. You can also pin a hekate release or provide your own
payload binary.

This project builds with the Raspberry Pi Pico SDK and TinyUSB host stack. The
default board target is `waveshare_rp2040_one`, and other Pico SDK-supported
RP2040 boards can be selected with `PICO_BOARD`.

## Prebuilt firmware

Automated releases are created for upstream hekate releases when possible. To
use a prebuilt firmware image, download the latest `fusee-rp2040-vX.Y.Z.uf2`
from the [releases page](https://github.com/suiminn/fusee-rp2040/releases),
put your RP2040 board into bootloader mode, and copy the UF2 file to it.

## Build from source

Install these build requirements first:

- Raspberry Pi Pico SDK
- CMake
- Python 3
- ARM GNU toolchain, such as `gcc-arm-none-eabi`

If the Pico SDK is installed locally, set `PICO_SDK_PATH` and build:

```sh
git clone https://github.com/suiminn/fusee-rp2040
cd fusee-rp2040
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

You can also let CMake fetch the Pico SDK:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_FETCH_FROM_GIT=on -DPICO_SDK_FETCH_FROM_GIT_TAG=2.2.0
cmake --build build --parallel
```

The USB host stack is timing-sensitive, so production builds should use an
optimized CMake build type such as `Release`.

The firmware is written to:

```text
build/src/fusee.uf2
```

The first default build downloads the latest standard hekate payload into the
build directory and reuses it on later builds. That requires network access to
GitHub unless you provide `FUSEE_PAYLOAD_BIN`.

## Payload selection

Pin a specific hekate release:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_HEKATE_TAG=v6.5.2
cmake --build build --parallel
```

Pinning a tag is recommended for repeatable builds.

Use a local payload binary instead of downloading hekate:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_PAYLOAD_BIN=/path/to/payload.bin
cmake --build build --parallel
```

The bundled intermezzo binary is used by default. To provide a different one:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_INTERMEZZO_BIN=/path/to/intermezzo.bin
cmake --build build --parallel
```

## Board configuration

The default board target is `waveshare_rp2040_one`. To build for another Pico
SDK-supported board, pass its board name during configuration. Use a fresh build
directory when switching board targets.

```sh
cmake -B build-seeed -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=seeed_xiao_rp2040
cmake --build build-seeed --parallel
```

If the board definition does not describe the status LED correctly, override it
without editing source:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_STATUS_LED_PIN=25
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_STATUS_LED_PIN=25 -DFUSEE_STATUS_LED_INVERTED=ON
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_STATUS_LED_ENABLED=OFF
```

## Status LED

On boards with a colored status LED, launch progress is shown in blue, success
in green, and failure in red.

On boards with a single-color status LED, launch progress is a slow blink,
success is solid on, and failure is a fast blink.

Success and failure indications are held for 3000 ms by default so they remain
visible after USB host reset recovery. To change the hold time:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_STATUS_LED_HOLD_MS=5000
cmake --build build --parallel
```

If a colored LED shows red and green swapped, override the byte order:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_STATUS_LED_RGB_ORDER=ON
cmake --build build --parallel
```

## Debug logging

UART debug logging is disabled by default. To enable it:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFUSEE_DEBUG_UART=ON
cmake --build build --parallel
```

The UART uses the Pico SDK board's default UART pins at 115200 baud. On
Raspberry Pi Pico-style boards, this is typically UART0 TX/RX on GPIO0/GPIO1.

## Tests

The Python helper scripts have unit tests:

```sh
python3 -m unittest discover -s tests
```

## Automated hekate releases

Two GitHub Actions workflows keep releases aligned with
[CTCaer/hekate releases](https://github.com/CTCaer/hekate/releases).

`Detect hekate release` checks the latest upstream hekate release every six
hours. If this repository does not already have a matching `vX.Y.Z` release, it
dispatches `Release`.

`Release` downloads the matching `hekate_ctcaer_<version>.bin` payload, verifies
the release asset digest when GitHub provides one, runs the tests, builds
`fusee.uf2`, and creates a release in this repository tagged `vX.Y.Z`. Release
artifacts include `THIRD_PARTY_NOTICES.md`; full license texts live in the
source tree.

GitHub Actions cannot directly subscribe to another public repository's release
event unless that repository or a relay sends this repository an event. For
webhook-style triggering, send a `repository_dispatch` event with type
`hekate-release` and an optional `tag` payload to run the release workflow
directly:

```sh
gh api repos/OWNER/fusee-rp2040/dispatches \
  --method POST \
  -f event_type=hekate-release \
  -F client_payload[tag]=v6.5.2
```

The release workflow can also be run manually. Leave the `hekate_tag` input
empty to build the current upstream latest release.

## License

This repository is distributed under the GNU General Public License version 3.0;
see [LICENSE.txt](LICENSE.txt).

Third-party notices and license texts are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). In particular, automated
release builds embed a hekate payload from
[CTCaer/hekate](https://github.com/CTCaer/hekate), which is distributed under
the GNU General Public License version 2.0.

## Credits

This repository is forked from
[Kozova1/fusee-rp2040](https://github.com/Kozova1/fusee-rp2040).

The implementation is heavily based on
[Qyriad's fusee-launcher](https://github.com/Qyriad/fusee-launcher),
[blockfeed's samd21 fusee launcher](https://github.com/blockfeed/sam-fusee-launcher-internal/),
and was also informed by
[rajkosto's TegraRcmSmash](https://github.com/rajkosto/TegraRcmSmash).
