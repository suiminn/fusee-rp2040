# Third-Party Notices

This project is distributed under the GNU General Public License version 3.0;
see [LICENSE.txt](LICENSE.txt). Files that carry their own license notices
remain under those notices.

## Upstream project

- [Kozova1/fusee-rp2040](https://github.com/Kozova1/fusee-rp2040)
  - Role: upstream project this repository is forked from.
  - License: GNU General Public License version 3.0.
  - License text: [LICENSE.txt](LICENSE.txt).

## Implementation references

- [Qyriad/fusee-launcher](https://github.com/Qyriad/fusee-launcher)
  - Role: reference implementation for the fusee gelee launch flow.
  - License: GNU General Public License version 2.0.
  - License text: [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt).
  - Note: the previously bundled `fusee-launcher.py` copy was removed because
    it was not used by the build and carried a GPLv2-only notice.

- [blockfeed/sam-fusee-launcher-internal](https://github.com/blockfeed/sam-fusee-launcher-internal/)
  - Role: reference implementation for embedded fusee launching.
  - License: GNU General Public License version 3.0.
  - License text: [LICENSE.txt](LICENSE.txt).

- [rajkosto/TegraRcmSmash](https://github.com/rajkosto/TegraRcmSmash)
  - Role: reference implementation for RCM payload upload behavior.
  - License: GNU General Public License version 3.0.
  - License text: [LICENSE.txt](LICENSE.txt).

## Bundled source-derived files

- [TinyUSB](https://github.com/hathach/tinyusb)
  - Role: `src/tusb_config.h` is derived from TinyUSB configuration examples.
  - License: MIT.
  - License text: [LICENSES/MIT.txt](LICENSES/MIT.txt).

## Release payloads

- [CTCaer/hekate](https://github.com/CTCaer/hekate)
  - Role: automated release builds embed the selected standard
    `hekate_ctcaer_<version>.bin` payload.
  - License: GNU General Public License version 2.0.
  - License text: [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt).
  - Source: release notes identify the exact hekate tag and link to the
    corresponding upstream release.
