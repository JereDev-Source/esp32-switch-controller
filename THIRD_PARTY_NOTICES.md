# Third-party notices

Copyright (c) 2026 JereDev and contributors.

Original project code is licensed under GPL-3.0-only. This license does not
replace the licenses of third-party files.

## joycontrol

`main/nfc_mcu.c` adapts the NFC flow from Poohl/joycontrol commit
`0b79bf7569c07d078576a6fa0e7fd0c212bb5ee3`. It keeps the GPLv3 attribution and
terms. See [docs/GPL-3.0.txt](docs/GPL-3.0.txt).

## BTstack

`components/btstack/` and `components/btstack_config/` keep BlueKitchen's
original license notices. BTstack permits personal, non-commercial use. See
[components/btstack/LICENSE](components/btstack/LICENSE).

Local changes include ESP-IDF integration, a larger HID MTU, L2CAP send result
handling, short HID report support, and extra diagnostics.


## Other components

- micro-ecc: BSD-2-Clause. Its notice is in
  [components/btstack/3rd-party/LICENSE.txt](components/btstack/3rd-party/LICENSE.txt).
- `main/esp_hid_gap.c` and `.h`: `Unlicense OR CC0-1.0`, as stated in their SPDX
  headers.
- ESP-IDF and pyserial are external dependencies and keep their own licenses.

Compiled files, logs, backups, dumps, keys, credentials, and unused legacy
sources are excluded from the repository.
