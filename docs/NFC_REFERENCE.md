# NFC read support

The firmware emulates the controller NFC MCU and presents a 540-byte dump loaded
from the PC. It supports reading only.

The implementation adapts the NFC flow from Poohl/joycontrol commit
`0b79bf7569c07d078576a6fa0e7fd0c212bb5ee3` under GPL-3.0. See
[CREDITS.md](../CREDITS.md) for the source links.

## Use

```powershell
python pc/cli.py --port COM3 load "C:/amiibo/example.bin"
python pc/cli.py --port COM3 unload
```

Open the amiibo feature inside the game after loading the file. The dump stays
in RAM and is lost after a reset. `nfc_ready=true` means the feature is available;
it does not confirm that the console completed a read.

Reading was tested on Switch 2 with Zelda: Tears of the Kingdom. Compatibility
with other games and system versions is not guaranteed.

## Limits

- No NFC writing, owner registration, or nickname changes.
- No decryption or signing of dump data.
- Fixed NTAG215 metadata; this is not a general NFC reader.
- Short or incomplete requests are rejected.

## Diagnostics

Use one serial session for commands and logs:

```powershell
python pc/cli.py --port COM3 session --log nfc-session.log
```

Available session commands include `press L`, `press A`, `load "file.bin"`,
`status`, `unload`, and `quit`.
