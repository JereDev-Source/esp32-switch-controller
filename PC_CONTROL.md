# PC control

The ESP32 accepts JSON commands over UART0 at 115200 baud. The firmware releases
buttons and centers the sticks after 500 ms without an update.

## Setup

```powershell
python -m pip install -r pc/requirements.txt
```

Close the ESP-IDF monitor before starting a PC client. Only one program can use
the serial port at a time.

## GUI

On Windows, use the launcher to select desktop Python even from an ESP-IDF
terminal. It searches user-installed Python versions, then the Python launcher
(`py`). The chosen Python must include Tcl/Tk and pyserial:

```powershell
.\pc\start_gui.cmd
```

If pyserial is missing, run `python -m pip install -r pc/requirements.txt`
using your desktop Python installation.
ESP-IDF Python does not include Tkinter. On other platforms, use
`python pc/controller.py` with a Python installation that includes Tkinter.

You can also double-click `pc/start_gui.cmd` in Windows Explorer.
The button labels below match the current Spanish interface.

1. Choose the board's port and click **Conectar** (connect). Use **Buscar puertos**
   to refresh the port list.
2. On the Switch, open **Change Grip/Order**, then click **L + R** in the app.
3. Click **Elegir carpeta…** (choose folder). All `.bin` files in the folder and
   its subfolders appear. Relative paths distinguish files with the same name.
4. Open the game's amiibo feature. Double-click a file, or select it and click
   **Presentar** (present). A single click only selects a file.
5. To switch amiibos, double-click another file. The app validates it, removes
   the previous tag, waits for confirmation and a short gap, then loads it.

Use **Retirar** (remove) to take the tag away. **Volver a presentar** (present
again) removes and reloads the last successfully presented file. It does not
bypass a game's own amiibo usage limits.

Search by display name, filename, or subfolder. **Cambiar nombre…** changes only
the display name; an empty name restores the original. Click **Actualizar** to
rescan after adding or moving files.

The app remembers the folder, port, and display names in
`%APPDATA%/ESP32SwitchController/settings.json` on Windows. It never renames or
modifies the original `.bin` files.

## Controller and status

Click the controller buttons or use the keyboard: arrow keys for the D-pad,
Z/X for B/A, A/S for Y/X, Q/W for L/R, Enter for Plus, and Backspace for Minus.
Sliders control the sticks. **Soltar / centrar** releases buttons and centers
the sticks. Typing in the search field or navigating the library does not send
controller buttons. Controls remain available while changing amiibos.

The status labels show PC-to-ESP32 connection, Switch connection, and the
presented file separately. A presented file is not proof that a game read it.
After reconnecting, a file already in the ESP32 can appear with an unknown name.
The app never loads a file automatically at startup.

## Troubleshooting

- **No module named tkinter:** use `pc/start_gui.cmd`. The ESP-IDF Python does
  not include Tkinter. Install desktop Python with Tcl/Tk if needed.
- **Missing pyserial:** install `pc/requirements.txt` with the same desktop
  Python used by the GUI.
- **Port is busy:** close the CLI, serial monitor, or other app using the port.
- **Invalid BIN:** use a raw 540-byte file with valid UID/BCC fields. An invalid
  replacement does not remove the current tag.
- **No confirmation for load/unload:** the command may have reached the ESP32.
  Check the presented-file indicator before trying again. NFC commands are not
  automatically repeated after a missing reply.
- **No control replies for 3 seconds:** reconnect the port. If it repeats, share
  the relevant **Actividad** lines, including `PC →`, `PC ←`, and the error.
  Firmware logs alone do not confirm that PC commands are being processed.

An isolated missing controller reply no longer stops state updates or disconnects
a device that continues answering. This recovery was checked with simulated
serial faults; behavior on your hardware may still need diagnosis.

If the previous controller/NFC firmware is already installed, these PC-app changes
do not require flashing the ESP32 again. NFC remains read-only and a reset clears
the loaded dump from RAM.

## CLI alternative

Close the GUI before using these commands:

```powershell
python pc/cli.py --port COM3 status
python pc/cli.py --port COM3 press L R
python pc/cli.py --port COM3 press A --seconds 0.2
python pc/cli.py --port COM3 release
python pc/cli.py --port COM3 load "C:/amiibo/example.bin"
python pc/cli.py --port COM3 unload
```

## Serial protocol

Each request is one JSON line with an integer `id`. Supported commands are
`status`, `state`, `release`, `load`, and `unload`. Responses start with `@PC`.

`state` uses button masks `r`, `s`, and `l`, plus 12-bit axes `lx`, `ly`, `rx`,
and `ry`. `load` sends a 540-byte dump as 1080 hexadecimal characters with its
CRC32 value.

See [NFC notes](docs/NFC_REFERENCE.md) for amiibo details and limitations.
