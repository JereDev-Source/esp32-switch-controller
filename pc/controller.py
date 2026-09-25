"""PC controller and local amiibo library. Run: python pc/controller.py"""
import queue
import time
from pathlib import Path
import tkinter as tk
from tkinter import ttk, filedialog, simpledialog

from library import Library, Presenter
from protocol import BUTTONS, KEYS, state_for
from serial_link import SerialLink, parse_reply


class App:
    def __init__(self, root, library=None):
        self.root = root
        self.library = library if library is not None else Library()
        self.link = None
        self.events = queue.Queue()
        self.pending = {}
        self.seq = 0
        self.connected = False
        self.ready = False
        self.pressed = set()
        self.axes = dict.fromkeys(("lx", "ly", "rx", "ry"), 2048)
        self.rows = {}
        self.sliders = {}
        self.last_status = 0
        self.last_reply = 0
        self.last_retry_log = 0
        self.closed = False
        self.port = tk.StringVar(value=self.library.port)
        self.pc_status = tk.StringVar(value="PC → ESP32: desconectado")
        self.switch_status = tk.StringVar(value="Switch: estado desconocido")
        self.file_status = tk.StringVar(value="Archivo presentado: estado desconocido")
        self.operation = tk.StringVar(value="Conecta el ESP32 para presentar un amiibo.")
        self.folder = tk.StringVar(value=self.library.folder or "Selecciona una carpeta con archivos .bin")
        self.search = tk.StringVar()
        self.count = tk.StringVar()
        root.title("ESP32 · Mando y biblioteca amiibo")
        root.geometry("1050x800")
        root.minsize(850, 770)
        main = ttk.Frame(root, padding=12)
        main.pack(fill="both", expand=True)
        connection = ttk.Frame(main)
        connection.pack(fill="x")
        ttk.Label(connection, text="Puerto").pack(side="left")
        self.port_box = ttk.Combobox(connection, textvariable=self.port, width=16)
        self.port_box.pack(side="left", padx=6)
        ttk.Button(connection, text="Buscar puertos", command=self.refresh_ports).pack(side="left")
        self.connect_button = ttk.Button(connection, text="Conectar", command=self.connect)
        self.connect_button.pack(side="left", padx=6)
        ttk.Button(connection, text="Desconectar", command=self.disconnect).pack(side="left")
        for variable in (self.pc_status, self.switch_status, self.file_status):
            ttk.Label(main, textvariable=variable, wraplength=960).pack(anchor="w", pady=2)
        panels = ttk.Frame(main)
        panels.pack(fill="both", expand=True, pady=8)
        panels.columnconfigure(0, weight=1)
        panels.columnconfigure(1, weight=2)
        panels.rowconfigure(0, weight=1)
        controls = ttk.LabelFrame(panels, text="Mando", padding=8)
        controls.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        for column in range(5):
            controls.columnconfigure(column, weight=1)
        for i, name in enumerate(BUTTONS):
            button = ttk.Button(controls, text=name, width=7)
            button.grid(row=i // 5, column=i % 5, sticky="ew", padx=1, pady=3)
            button.bind("<ButtonPress-1>", lambda e, n=name: self.pressed.add(n))
            button.bind("<ButtonRelease-1>", lambda e, n=name: self.pressed.discard(n))
        for i, key in enumerate(self.axes):
            ttk.Label(controls, text=key.upper()).grid(row=4 + i, column=0)
            slider = tk.Scale(controls, from_=0, to=4095, orient="horizontal",
                              command=lambda v, k=key: self.axes.update({k: int(v)}))
            slider.set(2048)
            slider.grid(row=4 + i, column=1, columnspan=4, sticky="ew")
            self.sliders[key] = slider
        ttk.Button(controls, text="Soltar / centrar", command=self.release_all).grid(
            row=8, column=0, columnspan=3, sticky="ew", pady=10)
        ttk.Button(controls, text="L + R", command=self.grip).grid(row=8, column=3, columnspan=2)
        ttk.Label(controls, text="Flechas: cruceta · Z/X: B/A · Q/W: L/R\n"
                  "Al escribir o salir de la ventana se liberan los controles.",
                  wraplength=340).grid(row=9, column=0, columnspan=5, sticky="w")
        browser = ttk.LabelFrame(panels, text="Biblioteca amiibo", padding=8)
        browser.grid(row=0, column=1, sticky="nsew")
        folder_bar = ttk.Frame(browser)
        folder_bar.pack(fill="x")
        ttk.Button(folder_bar, text="Elegir carpeta…", command=self.choose_folder).pack(side="left")
        ttk.Button(folder_bar, text="Actualizar", command=self.refresh_library).pack(side="left", padx=6)
        ttk.Label(browser, textvariable=self.folder, wraplength=500).pack(anchor="w", pady=5)
        ttk.Label(browser, text="Buscar por nombre, archivo o subcarpeta").pack(anchor="w")
        self.search_box = ttk.Entry(browser, textvariable=self.search)
        self.search_box.pack(fill="x", pady=(0, 5))
        listing = ttk.Frame(browser)
        listing.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(listing, columns=("name", "file"), show="headings", selectmode="browse", height=10)
        self.tree.heading("name", text="Nombre")
        self.tree.heading("file", text="Ruta del archivo")
        self.tree.column("name", width=180, minwidth=100)
        self.tree.column("file", width=220, minwidth=100)
        self.tree.pack(side="left", fill="both", expand=True)
        scrollbar = ttk.Scrollbar(listing, orient="vertical", command=self.tree.yview)
        scrollbar.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=scrollbar.set)
        self.tree.bind("<Double-1>", self.double_click)
        self.tree.bind("<Return>", lambda e: self.present_selected())
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.update_buttons())
        ttk.Label(browser, textvariable=self.count).pack(anchor="w", pady=3)
        actions = ttk.Frame(browser)
        actions.pack(fill="x", pady=4)
        self.present_button = ttk.Button(actions, text="Presentar", command=self.present_selected)
        self.present_button.pack(side="left")
        self.remove_button = ttk.Button(actions, text="Retirar", command=self.remove)
        self.remove_button.pack(side="left", padx=4)
        self.repeat_button = ttk.Button(actions, text="Volver a presentar", command=self.repeat)
        self.repeat_button.pack(side="left")
        ttk.Button(browser, text="Cambiar nombre…", command=self.rename).pack(anchor="w", pady=4)
        ttk.Label(browser, text="Doble clic para presentar. Cambiar nombre no modifica el .bin.",
                  wraplength=500).pack(anchor="w")
        ttk.Label(main, textvariable=self.operation, wraplength=960).pack(anchor="w", pady=4)
        ttk.Label(main, text="Presentado significa cargado en el ESP32; no confirma una lectura del juego.",
                  wraplength=960).pack(anchor="w")
        diagnostics = ttk.LabelFrame(main, text="Actividad", padding=4)
        diagnostics.pack(fill="x", pady=(8, 0))
        self.log = tk.Text(diagnostics, height=5, state="disabled", wrap="word")
        self.log.pack(fill="x")
        self.presenter = Presenter(self.send, root.after, self.presentation_changed, self.show_error)
        self.search.trace_add("write", lambda *args: self.refresh_library())
        root.bind("<KeyPress>", self.keydown)
        root.bind("<KeyRelease>", self.keyup)
        root.bind("<FocusOut>", lambda e: root.after(50, self.check_focus))
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh_ports()
        self.refresh_library()
        if self.library.warning:
            self.append(self.library.warning)
        self.tick_id = root.after(50, self.tick)

    def save_preferences(self):
        try:
            self.library.save()
        except OSError as exc:
            self.show_error("No se pudieron guardar las preferencias: " + str(exc))

    def refresh_ports(self):
        try:
            from serial.tools import list_ports
            self.port_box["values"] = [p.device for p in list_ports.comports()]
        except ImportError:
            self.append("Instala pyserial: python -m pip install -r pc/requirements.txt")

    def selected(self):
        selection = self.tree.selection()
        return self.rows.get(selection[0]) if selection else None

    def refresh_library(self):
        previous = self.selected()
        self.tree.delete(*self.tree.get_children())
        self.rows.clear()
        try:
            entries = self.library.entries(self.search.get())
        except OSError as exc:
            self.count.set(str(exc))
            self.update_buttons()
            return
        for i, path in enumerate(entries):
            iid = str(i)
            self.rows[iid] = path
            self.tree.insert("", "end", iid=iid,
                             values=(self.library.name(path), self.library.relative_path(path)))
            if path == previous:
                self.tree.selection_set(iid)
        self.count.set(f"{len(entries)} archivo(s) · doble clic para presentar")
        self.update_buttons()

    def choose_folder(self):
        self.release_all()
        folder = filedialog.askdirectory(parent=self.root, initialdir=self.library.folder or None)
        if folder:
            self.library.folder = str(Path(folder).resolve())
            self.folder.set(self.library.folder)
            self.save_preferences()
            self.search.set("")
            self.refresh_library()

    def rename(self):
        path = self.selected()
        if path is None:
            return
        self.release_all()
        name = simpledialog.askstring("Nombre de amiibo", "Nombre visible (vacío restaura el original):",
                                      initialvalue=self.library.name(path), parent=self.root)
        if name is not None:
            try:
                self.library.rename(path, name)
            except OSError as exc:
                self.show_error(str(exc))
            self.refresh_library()
            self.presentation_changed()

    def double_click(self, event):
        row = self.tree.identify_row(event.y)
        if row and self.tree.identify_region(event.x, event.y) == "cell":
            self.tree.selection_set(row)
            self.present_selected()
        return "break"

    def present_selected(self):
        self.present(self.selected())

    def present(self, path):
        if path is None or not self.ready or self.presenter.busy:
            return
        try:
            self.presenter.present(path)
        except (OSError, ValueError) as exc:
            self.show_error(str(exc))

    def repeat(self):
        self.present(self.presenter.last_path)

    def remove(self):
        if self.ready:
            self.presenter.remove()

    def presentation_changed(self):
        model = self.presenter
        if model.loaded is None:
            label = "estado desconocido"
        elif not model.loaded:
            label = "ninguno"
        elif model.path is not None:
            label = self.library.name(model.path)
        else:
            label = "hay un archivo en el ESP32 (nombre desconocido)"
        self.file_status.set("Archivo presentado: " + label)
        if model.busy:
            self.operation.set(model.phase)
        elif self.ready and not self.operation.get().startswith("Error:"):
            self.operation.set("Listo. Abre la función amiibo del juego y presenta un archivo.")
        self.update_buttons()

    def update_buttons(self):
        available = self.ready and not self.presenter.busy
        self.present_button["state"] = "normal" if available and self.selected() else "disabled"
        self.remove_button["state"] = "normal" if available and self.presenter.loaded else "disabled"
        self.repeat_button["state"] = "normal" if available and self.presenter.last_path else "disabled"

    def text_focus(self, widget):
        return isinstance(widget, (tk.Entry, ttk.Entry, ttk.Combobox, tk.Text, ttk.Treeview))

    def check_focus(self):
        if not self.closed and (self.root.focus_displayof() is None or self.text_focus(self.root.focus_get())):
            if self.pressed or any(v != 2048 for v in self.axes.values()):
                self.release_all()

    def keydown(self, event):
        if not self.text_focus(event.widget) and event.keysym in KEYS:
            self.pressed.add(KEYS[event.keysym])

    def keyup(self, event):
        if event.keysym in KEYS:
            self.pressed.discard(KEYS[event.keysym])

    def grip(self):
        self.pressed.update(("L", "R"))
        self.root.after(300, lambda: self.pressed.difference_update(("L", "R")))

    def release_all(self):
        self.pressed.clear()
        for key, slider in self.sliders.items():
            slider.set(2048)
            self.axes[key] = 2048
        if self.link:
            self.send({"cmd": "release"})

    def connect(self):
        self.disconnect()
        port = None
        try:
            import serial
            port = serial.Serial(port=None, baudrate=115200, timeout=0.02, write_timeout=0.3)
            port.dtr = False
            port.rts = False
            port.port = self.port.get().strip()
            port.open()
            self.link = SerialLink(port, self.events)
            self.last_reply = time.monotonic()
            self.library.port = port.port
            self.save_preferences()
            self.pc_status.set("PC → ESP32: esperando respuesta…")
            self.connect_button["state"] = "disabled"
            self.send({"cmd": "status"})
        except Exception as exc:
            if port is not None:
                port.close()
            self.disconnect()
            self.show_error("Conexión: " + str(exc))

    def send(self, command, callback=None):
        if self.link is None:
            if callback:
                callback({"ok": False, "message": "Conecta primero el ESP32."})
            return
        self.seq = self.seq % 2147483646 + 1
        try:
            self.link.send(dict(command, id=self.seq))
            self.pending[self.seq] = (command["cmd"], time.monotonic(), callback)
            if command["cmd"] in ("load", "unload"):
                self.append(f"PC → {command['cmd']} #{self.seq}")
        except Exception as exc:
            self.disconnect()
            self.show_error("Error serie: " + str(exc))

    def disconnect(self):
        link, self.link = self.link, None
        self.ready = self.connected = False
        self.pending.clear()
        self.pressed.clear()
        for key, slider in self.sliders.items():
            slider.set(2048)
            self.axes[key] = 2048
        self.presenter.reset()
        if link:
            link.close()
        self.pc_status.set("PC → ESP32: desconectado")
        self.switch_status.set("Switch: estado desconocido")
        self.operation.set("Conecta el ESP32 para presentar un amiibo.")
        self.connect_button["state"] = "normal"

    def handle_reply(self, reply):
        if not isinstance(reply, dict) or not isinstance(reply.get("id"), int):
            return
        pending = self.pending.pop(reply["id"], None)
        if pending is None:
            return
        kind, _, callback = pending
        self.last_reply = time.monotonic()
        if kind in ("load", "unload"):
            self.append(f"PC ← {kind} #{reply['id']}: " + ("OK" if reply.get("ok") else "ERROR"))
        self.ready = True
        self.pc_status.set("PC → ESP32: conectado")
        connected = reply.get("connected") is True
        if self.connected and not connected:
            self.release_all()
        self.connected = connected
        self.switch_status.set("Switch: " + ("conectada" if connected else "desconectada"))
        self.presenter.observe(reply)
        if callback:
            callback(reply)
        elif not reply.get("ok") and kind != "state":
            self.show_error(reply.get("message", "Error del ESP32"))

    def tick(self):
        if self.closed:
            return
        for _ in range(150):
            try:
                link, line = self.events.get_nowait()
            except queue.Empty:
                break
            if link is not self.link:
                continue
            if isinstance(line, Exception):
                self.disconnect()
                self.show_error("Conexión interrumpida: " + str(line))
            elif "@PC " in line:
                reply = parse_reply(line)
                if reply is not None:
                    self.handle_reply(reply)
                else:
                    self.append("Respuesta serie incompleta o inválida.")
            else:
                self.append(line)
        now = time.monotonic()
        # A missing individual ACK is not a dead device. State is a heartbeat:
        # replace stale states with the latest input, never replay old buttons.
        for seq, (kind, sent, callback) in list(self.pending.items()):
            limit = 0.25 if kind in ("state", "release") else (1 if kind == "status" else 3)
            if now - sent <= limit:
                continue
            self.pending.pop(seq, None)
            if callback:
                # Do not blindly retry NFC commands: the first may have succeeded.
                # Abort this workflow and obtain the current device state instead.
                callback({"ok": False, "message": f"Sin confirmación de {kind} #{seq}. "
                          "Consulta el archivo presentado antes de intentarlo de nuevo."})
                if self.link:
                    self.send({"cmd": "status"})
            elif now - self.last_retry_log > 2:
                self.append(f"Sin confirmación de {kind} #{seq}; consultando el estado actual.")
                self.last_retry_log = now
        if self.link and now - self.last_reply > 3:
            self.disconnect()
            self.show_error("Sin respuestas de control @PC durante 3 segundos. Reconecta el puerto.")
        if self.link:
            if self.connected and not any(kind == "state" for kind, _, _ in self.pending.values()):
                self.send(dict(cmd="state", **state_for(self.pressed, self.axes)))
            elif (not self.connected and now - self.last_status >= 0.5
                  and not any(kind == "status" for kind, _, _ in self.pending.values())):
                self.send({"cmd": "status"})
                self.last_status = now
        self.tick_id = self.root.after(50, self.tick)

    def show_error(self, message):
        self.operation.set("Error: " + message)
        self.append("Error: " + message)

    def append(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 250:
            self.log.delete("1.0", "100.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def close(self):
        self.closed = True
        self.root.after_cancel(self.tick_id)
        self.disconnect()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
