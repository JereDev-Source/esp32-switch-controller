# ESP32 Switch Controller

Experimental Nintendo Switch Pro Controller emulation for the original ESP32
(ESP32-WROOM / DevKit V1). It connects through Bluetooth Classic and can be
controlled from a PC over USB serial.

Created by [JereDev](https://github.com/JereDev-Source).


## Features

- Connects to Switch 2 as a Switch 1 Pro Controller.
- PC control for buttons and analog sticks.
- Amiibo library with search, custom names, and double-click presentation.
- Loads and presents a 540-byte amiibo dump from the PC.
- Read-only NFC support. Writing, owner registration, and nickname changes are
  not supported.

Controller input and NFC reading were tested on Switch 2. NFC was tested with
Zelda: Tears of the Kingdom. Other games and system versions may behave
differently.

This project does not include amiibo dumps, keys, logs, or compiled firmware.
It is not affiliated with Nintendo.

## Requirements

- Original ESP32 with Bluetooth Classic, such as ESP32-WROOM-32 or DevKit V1.
- ESP-IDF 5.4.4.
- Desktop Python 3 with Tcl/Tk and `pyserial` for the PC app.

## Build and flash

Run these commands from an ESP-IDF terminal:

```powershell
idf.py -B build_btstack -D SDKCONFIG=sdkconfig.btstack build
idf.py -B build_btstack -D SDKCONFIG=sdkconfig.btstack -p COM3 flash
```

Replace `COM3` with the board's serial port.

## PC control

Install the PC dependency using desktop Python (not ESP-IDF Python):

```powershell
python -m pip install -r pc/requirements.txt
.\pc\start_gui.cmd
```

1. Close any serial monitor, choose your port (for example `COM3`), and click
   **Conectar**.
2. Open **Change Grip/Order** on the Switch and click **L + R** in the app.
3. Click **Elegir carpeta…** to select your amiibo folder. Subfolders are included.
4. Open the game's amiibo feature and double-click a file to present it.
5. Double-click another file to switch, or use **Retirar** to remove the tag and
   **Volver a presentar** to present the last file again.

Search by name, filename, or subfolder. **Cambiar nombre…** changes the display
name without renaming the file. The app remembers the folder, port, and names.
Only one program can use the serial port at a time.

The app currently uses Spanish button labels. See the [PC guide](PC_CONTROL.md)
for keyboard controls, connection indicators, and troubleshooting.

## Amiibo

The GUI handles loading and removal. These commands are an optional alternative
when the GUI is closed:

```powershell
python pc/cli.py --port COM3 load "C:/amiibo/example.bin"
python pc/cli.py --port COM3 unload
```

The dump stays in RAM and is lost after a reset. Use only dumps you are allowed
to use.

See [PC control](PC_CONTROL.md) and [NFC notes](docs/NFC_REFERENCE.md) for more
details.

## License and credits

Original project code is provided under GPL-3.0-only. Third-party components
keep their own licenses. See [credits](CREDITS.md) and
[third-party notices](THIRD_PARTY_NOTICES.md).

BTstack is limited to personal, non-commercial use. Its terms may not be
compatible with the GPLv3 NFC-derived code when distributed together. See
[third-party notices](THIRD_PARTY_NOTICES.md).
