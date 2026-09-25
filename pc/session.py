"""One serial connection for commands and continuous diagnostic capture."""
import json
import math
import queue
import threading
import time
from pathlib import Path
from protocol import load_bin, state_for

def parse_command(line):
    cmd, _, rest = line.strip().partition(" ")
    rest = rest.strip()
    if cmd in ("status", "release", "unload", "help", "quit", "exit"):
        if rest: raise ValueError("Esta orden no lleva argumentos.")
        return cmd, None
    if cmd == "load":
        if len(rest)>1 and rest[0]==rest[-1] and rest[0] in ("'", '"'): rest=rest[1:-1]
        if not rest: raise ValueError('Uso: load "D:\\ruta\\archivo.bin"')
        return cmd, rest
    if cmd == "press":
        parts=rest.split()
        duration=.3
        if "--seconds" in parts:
            i=parts.index("--seconds")
            if i!=len(parts)-2: raise ValueError("Uso: press L R --seconds 0.3")
            duration=float(parts[-1]);parts=parts[:i]
        if not parts or not math.isfinite(duration) or not 0<duration<=60:
            raise ValueError("Indica botones y duración de 0 a 60 segundos.")
        state=state_for(set(parts),dict(lx=2048,ly=2048,rx=2048,ry=2048))
        return cmd,(state,duration)
    raise ValueError("Orden desconocida. Escribe help.")

class SerialSession:
    def __init__(self, port, log_path):
        self.port=port
        self.seq=0
        self.replies=queue.Queue(maxsize=128)
        self.stop=threading.Event()
        self.error=None
        self.lock=threading.Lock()
        self.log=Path(log_path).open("a",encoding="utf-8",buffering=1)
        self.record("SESSION","opened")
        self.thread=threading.Thread(target=self.reader,daemon=True)
        self.thread.start()
    def record(self,direction,line):
        with self.lock:
            self.log.write(f"{time.time():.3f} {direction} {line}\n")
    def reader(self):
        pending=bytearray()
        try:
            while not self.stop.is_set():
                chunk=self.port.read(1024)
                if not chunk: continue
                pending.extend(chunk)
                while b"\n" in pending:
                    raw,_,pending=pending.partition(b"\n")
                    line=raw.decode("utf-8","replace").rstrip("\r")
                    self.record("RX",line)
                    if line.startswith("@PC "):
                        try: reply=json.loads(line[4:])
                        except ValueError: continue
                        try: self.replies.put_nowait(reply)
                        except queue.Full:
                            self.replies.get_nowait();self.replies.put_nowait(reply)
                if len(pending)>16384:
                    self.record("RX_PARTIAL",pending.decode("utf-8","replace"))
                    pending.clear()
        except Exception as exc:
            self.error=exc
    def send(self,command):
        self.seq+=1
        # Do not put the full amiibo contents in the diagnostic log.
        display={k:v for k,v in command.items() if k!="hex"}
        self.record("TX",json.dumps(dict(display,id=self.seq)))
        wire=json.dumps(dict(command,id=self.seq),separators=(",",":"))+"\n"
        self.port.write(wire.encode())
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            if self.error: raise RuntimeError(f"Error serie: {self.error}")
            try: reply=self.replies.get(timeout=.1)
            except queue.Empty: continue
            if reply.get("id")==self.seq:
                if not reply.get("ok"): raise RuntimeError(reply.get("message","Error"))
                return reply
        raise TimeoutError("ESP32 sin respuesta; revisa conexión y firmware.")
    def press(self,state,duration):
        try:
            end=time.monotonic()+duration
            while time.monotonic()<end:
                self.send(dict(cmd="state",**state))
                time.sleep(.05)
        finally: self.send({"cmd":"release"})
    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        if self.thread.is_alive():
            self.port.close()
            self.thread.join(timeout=1)
        self.port.close()
        if not self.thread.is_alive(): self.log.close()

def run_session(port, log_path):
    session=SerialSession(port,log_path)
    print("Sesión única: botones y captura simultáneos.")
    print("Registro:",Path(log_path).resolve())
    print('Órdenes: press L | press A | press L R --seconds 0.5')
    print('load "D:\\ruta\\Kirby.bin" | status | unload | release | quit')
    print("Los mensajes del firmware se guardan en el archivo mientras escribes.")
    try:
        print(session.send({"cmd":"status"}))
        while True:
            try:
                command,args=parse_command(input("esp32> "))
                if command in ("quit","exit"): break
                if command=="help":
                    print('press BOTONES [--seconds N], load "ruta.bin", status, unload, release, quit')
                elif command=="press":
                    session.press(*args);print("Botones liberados.")
                elif command=="load": print(session.send(load_bin(args)))
                else: print(session.send({"cmd":command}))
            except (ValueError,KeyError,RuntimeError,TimeoutError) as exc:
                print("Error:",exc)
    except (KeyboardInterrupt,EOFError):
        print("\nCerrando sesión.")
    finally:
        try: session.send({"cmd":"release"})
        except Exception: pass
        session.close()
