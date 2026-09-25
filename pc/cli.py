"""Console control using the Python shipped with ESP-IDF (pyserial)."""
import argparse
import json
import time
from pathlib import Path
from protocol import load_bin, state_for

def main():
    import serial
    parser=argparse.ArgumentParser()
    parser.add_argument("--port",required=True)
    sub=parser.add_subparsers(dest="command",required=True)
    sub.add_parser("monitor"); sub.add_parser("status"); sub.add_parser("release"); sub.add_parser("unload")
    press=sub.add_parser("press")
    press.add_argument("buttons",nargs="+")
    press.add_argument("--seconds",type=float,default=.3)
    load=sub.add_parser("load");load.add_argument("file")
    shell=sub.add_parser("session")
    shell.add_argument("--log",default="nfc-session.log")
    args=parser.parse_args()
    port=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=.5)
    port.dtr=False;port.rts=False;port.port=args.port;port.open()
    seq=0
    def send(command):
        nonlocal seq
        seq+=1
        port.write((json.dumps(dict(command,id=seq),separators=(",",":"))+"\n").encode())
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            line=port.readline().decode("utf-8","replace").strip()
            if line.startswith("@PC "):
                try: result=json.loads(line[4:])
                except ValueError: continue
                if result.get("id")==seq:
                    if not result["ok"]: raise RuntimeError(result["message"])
                    return result
        raise TimeoutError("ESP32 sin respuesta; cierra idf.py monitor y verifica el firmware.")
    try:
        if args.command=="session":
            from session import run_session
            run_session(port,args.log)
        elif args.command=="monitor":
            print("Monitor sin reinicio. Ctrl+C para salir.")
            try:
                while True:
                    line=port.readline().decode("utf-8","replace").strip()
                    if line: print(line,flush=True)
            except KeyboardInterrupt: pass
        elif args.command=="press":
            if not 0<args.seconds<=60: raise ValueError("Duración permitida: 0–60 segundos")
            state=state_for(set(args.buttons),dict(lx=2048,ly=2048,rx=2048,ry=2048))
            try:
                deadline=time.monotonic()+args.seconds
                while time.monotonic()<deadline:
                    send(dict(cmd="state",**state));time.sleep(.05)
            finally: send(dict(cmd="release"))
            print("Botones liberados.")
        elif args.command=="load": print(send(load_bin(args.file)))
        else: print(send(dict(cmd=args.command)))
    finally: port.close()
if __name__=="__main__":
    main()
