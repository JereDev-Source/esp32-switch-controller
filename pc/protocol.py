from pathlib import Path
import zlib

BUTTONS = {
    "Y": ("r", 1), "X": ("r", 2), "B": ("r", 4), "A": ("r", 8),
    "R": ("r", 64), "ZR": ("r", 128), "L": ("l", 64), "ZL": ("l", 128),
    "Down": ("l", 1), "Up": ("l", 2), "Right": ("l", 4), "Left": ("l", 8),
    "Minus": ("s", 1), "Plus": ("s", 2), "RS": ("s", 4), "LS": ("s", 8),
    "Home": ("s", 16), "Capture": ("s", 32),
}
KEYS = {"Up":"Up","Down":"Down","Left":"Left","Right":"Right",
        "z":"B","x":"A","a":"Y","s":"X","q":"L","w":"R",
        "Return":"Plus","BackSpace":"Minus"}
def state_for(pressed, axes):
    state = dict(r=0, s=0, l=0, **axes)
    for name in pressed:
        field, bit = BUTTONS[name]
        state[field] |= bit
    return state
def load_bin(path):
    # One extra byte detects oversized files without loading arbitrary BINs.
    with Path(path).open("rb") as source:
        data = source.read(541)
    if len(data) != 540:
        raise ValueError("Se requiere un dump raw de 540 bytes; no se recorta ni convierte.")
    if data[0] != 4 or data[3] != (0x88 ^ data[0] ^ data[1] ^ data[2]) or data[8] != (data[4] ^ data[5] ^ data[6] ^ data[7]):
        raise ValueError("UID/BCC del archivo NTAG inválido.")
    return dict(cmd="load", hex=data.hex(), crc=zlib.crc32(data))

