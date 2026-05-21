import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import serial
import serial.tools.list_ports
import threading
import json
import time
 
# ── colour palette ───────────────────────────────────────────────────────────
BG       = "#0d1117"
PANEL    = "#161b22"
BORDER   = "#30363d"
ACCENT   = "#58a6ff"
ACCENT2  = "#3fb950"
WARN     = "#f85149"
TEXT     = "#e6edf3"
MUTED    = "#8b949e"
ENTRY_BG = "#21262d"
 
FONT_MONO  = ("Courier New", 10)
FONT_LABEL = ("Courier New", 9, "bold")
FONT_HEAD  = ("Courier New", 13, "bold")
FONT_BTN   = ("Courier New", 10, "bold")
 
 
class BeaconGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Beacon UART Tester")
        self.configure(bg=BG)
        self.resizable(True, True)
        self.geometry("900x720")
 
        self.ser: serial.Serial | None = None
        self.read_thread: threading.Thread | None = None
        self.running = False
 
        self._build_ui()
 
    # ── UI construction ───────────────────────────────────────────────────────
 
    def _build_ui(self):
        # title bar
        title = tk.Label(self, text="◈  BEACON UART TESTER", font=FONT_HEAD,
                         bg=BG, fg=ACCENT)
        title.pack(pady=(14, 0))
 
        sep = tk.Frame(self, bg=BORDER, height=1)
        sep.pack(fill="x", padx=16, pady=8)
 
        # main layout: left controls | right log
        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True, padx=16, pady=(0, 16))
        body.columnconfigure(0, weight=0)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(0, weight=1)
 
        left  = tk.Frame(body, bg=BG, width=340)
        left.grid(row=0, column=0, sticky="ns", padx=(0, 12))
        left.pack_propagate(False)
 
        right = tk.Frame(body, bg=BG)
        right.grid(row=0, column=1, sticky="nsew")
 
        self._build_connection(left)
        self._build_mode(left)
        self._build_list(left)
        self._build_add(left)
        self._build_remove(left)
        self._build_raw(left)
        self._build_log(right)
 
    def _section(self, parent, title):
        f = tk.LabelFrame(parent, text=f" {title} ", font=FONT_LABEL,
                          bg=PANEL, fg=ACCENT, bd=1, relief="flat",
                          highlightbackground=BORDER, highlightthickness=1)
        f.pack(fill="x", pady=5)
        return f
 
    def _entry(self, parent, placeholder=""):
        e = tk.Entry(parent, bg=ENTRY_BG, fg=TEXT, insertbackground=ACCENT,
                     relief="flat", font=FONT_MONO, bd=4)
        if placeholder:
            e.insert(0, placeholder)
            e.config(fg=MUTED)
            def on_focus_in(ev, ent=e, ph=placeholder):
                if ent.get() == ph:
                    ent.delete(0, "end")
                    ent.config(fg=TEXT)
            def on_focus_out(ev, ent=e, ph=placeholder):
                if not ent.get():
                    ent.insert(0, ph)
                    ent.config(fg=MUTED)
            e.bind("<FocusIn>",  on_focus_in)
            e.bind("<FocusOut>", on_focus_out)
        return e
 
    def _btn(self, parent, text, cmd, color=ACCENT):
        return tk.Button(parent, text=text, command=cmd,
                         bg=PANEL, fg=color, activebackground=BORDER,
                         activeforeground=color, relief="flat",
                         font=FONT_BTN, cursor="hand2", bd=0,
                         padx=10, pady=5)
 
    # ── sections ──────────────────────────────────────────────────────────────
 
    def _build_connection(self, parent):
        sec = self._section(parent, "CONNECTION")
 
        row1 = tk.Frame(sec, bg=PANEL)
        row1.pack(fill="x", padx=8, pady=(6, 2))
 
        tk.Label(row1, text="Port", bg=PANEL, fg=MUTED, font=FONT_LABEL,
                 width=6, anchor="w").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb  = ttk.Combobox(row1, textvariable=self.port_var,
                                     font=FONT_MONO, width=18)
        self.port_cb.pack(side="left", padx=(0, 4))
        self._btn(row1, "↺", self._refresh_ports, MUTED).pack(side="left")
 
        row2 = tk.Frame(sec, bg=PANEL)
        row2.pack(fill="x", padx=8, pady=(2, 6))
 
        tk.Label(row2, text="Baud", bg=PANEL, fg=MUTED, font=FONT_LABEL,
                 width=6, anchor="w").pack(side="left")
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(row2, textvariable=self.baud_var,
                     values=["9600","38400","57600","115200","230400"],
                     font=FONT_MONO, width=10).pack(side="left", padx=(0, 8))
 
        self.conn_btn = self._btn(row2, "CONNECT", self._toggle_connection, ACCENT2)
        self.conn_btn.pack(side="left")
 
        self.status_lbl = tk.Label(sec, text="● disconnected", bg=PANEL,
                                   fg=WARN, font=FONT_LABEL)
        self.status_lbl.pack(padx=8, pady=(0, 6))
 
        self._refresh_ports()
 
    def _build_mode(self, parent):
        sec = self._section(parent, "SET MODE")
        row = tk.Frame(sec, bg=PANEL)
        row.pack(fill="x", padx=8, pady=8)
 
        self.mode_var = tk.StringVar(value="sniffer")
 
        tk.Radiobutton(row, text="sniffer", variable=self.mode_var, value="sniffer",
                       bg=PANEL, fg=ACCENT, selectcolor=ENTRY_BG, activebackground=PANEL,
                       activeforeground=ACCENT, font=FONT_BTN).pack(side="left", padx=(0, 8))
 
        tk.Radiobutton(row, text="base", variable=self.mode_var, value="base",
                       bg=PANEL, fg=ACCENT2, selectcolor=ENTRY_BG, activebackground=PANEL,
                       activeforeground=ACCENT2, font=FONT_BTN).pack(side="left", padx=(0, 8))
 
        self._btn(row, "set mode →", self._send_mode_selected, ACCENT).pack(side="left")
 
    def _build_list(self, parent):
        sec = self._section(parent, "LIST BEACONS")
        row = tk.Frame(sec, bg=PANEL)
        row.pack(fill="x", padx=8, pady=8)
        self._btn(row, "list_beacons →", self._send_list, ACCENT).pack(side="left")
 
    def _build_add(self, parent):
        sec = self._section(parent, "ADD BEACON")
 
        fields = [
            ("Name",       "MyBeacon"),
            ("MAC",        "AA:BB:CC:DD:EE:FF"),
            ("Major",      "1"),
            ("Minor",      "1"),
            ("RSSI Ref",   "-60"),
            ("Left Name",  ""),
            ("Right Name", ""),
        ]
        self._add_entries = {}
        for label, ph in fields:
            r = tk.Frame(sec, bg=PANEL)
            r.pack(fill="x", padx=8, pady=2)
            tk.Label(r, text=label, bg=PANEL, fg=MUTED, font=FONT_LABEL,
                     width=11, anchor="w").pack(side="left")
            e = self._entry(r, ph)
            e.pack(side="left", fill="x", expand=True)
            self._add_entries[label] = e
 
        self._btn(sec, "add_beacon →", self._send_add, ACCENT2).pack(
            padx=8, pady=(4, 8), anchor="w")
 
    def _build_remove(self, parent):
        sec = self._section(parent, "REMOVE BEACON")
        row = tk.Frame(sec, bg=PANEL)
        row.pack(fill="x", padx=8, pady=8)
        tk.Label(row, text="MAC", bg=PANEL, fg=MUTED, font=FONT_LABEL,
                 width=5, anchor="w").pack(side="left")
        self.rem_mac = self._entry(row, "AA:BB:CC:DD:EE:FF")
        self.rem_mac.pack(side="left", fill="x", expand=True, padx=(0,6))
        self._btn(row, "remove →", self._send_remove, WARN).pack(side="left")
 
    def _build_raw(self, parent):
        sec = self._section(parent, "RAW JSON")
        self.raw_entry = self._entry(sec, '{"Command":"...","Mode":"..."}')
        self.raw_entry.pack(fill="x", padx=8, pady=(6, 2))
        self._btn(sec, "send →", self._send_raw, MUTED).pack(
            padx=8, pady=(2, 8), anchor="w")
 
    def _build_log(self, parent):
        hdr = tk.Frame(parent, bg=BG)
        hdr.pack(fill="x")
        tk.Label(hdr, text="SERIAL LOG", font=FONT_LABEL,
                 bg=BG, fg=ACCENT).pack(side="left")
        self._btn(hdr, "clear", self._clear_log, MUTED).pack(side="right")
 
        self.log = scrolledtext.ScrolledText(
            parent, bg=PANEL, fg=TEXT, font=FONT_MONO,
            relief="flat", bd=0, wrap="word",
            insertbackground=ACCENT, state="disabled")
        self.log.pack(fill="both", expand=True, pady=(4, 0))
 
        self.log.tag_config("tx",   foreground=ACCENT)
        self.log.tag_config("rx",   foreground=ACCENT2)
        self.log.tag_config("err",  foreground=WARN)
        self.log.tag_config("info", foreground=MUTED)
 
    # ── helpers ───────────────────────────────────────────────────────────────
 
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb["values"] = ports
        if ports:
            self.port_var.set(ports[0])
 
    def _log(self, msg, tag="info"):
        self.log.config(state="normal")
        ts = time.strftime("%H:%M:%S")
        prefix = {"tx": "→ TX", "rx": "← RX", "err": "✖ ERR", "info": "·"}[tag]
        self.log.insert("end", f"[{ts}] {prefix}  {msg}\n", tag)
        self.log.see("end")
        self.log.config(state="disabled")
 
    def _clear_log(self):
        self.log.config(state="normal")
        self.log.delete("1.0", "end")
        self.log.config(state="disabled")
 
    def _send(self, payload: dict):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("Not connected", "Connect to a serial port first.")
            return
        raw = json.dumps(payload, separators=(',', ':')) + "\n"  # compact, no spaces
        print(f"SENDING ({len(raw)} bytes): {raw}")  # print to terminal
        self._log(raw.strip(), "tx")
        self.ser.write(raw.encode())
 
    def _get_entry(self, key):
        e = self._add_entries[key]
        val = e.get()
        # return empty string if still showing placeholder
        placeholders = {"Left Name": "", "Right Name": ""}
        defaults = {"MyBeacon","AA:BB:CC:DD:EE:FF","1","-60",""}
        return "" if val in defaults and key in ("Left Name","Right Name") else val
 
    # ── command senders ───────────────────────────────────────────────────────
 
    def _send_mode_selected(self):
        self._send({"Command": "set_mode", "Mode": self.mode_var.get()})
 
    def _send_add(self):
        e = self._add_entries
        def val(k): return e[k].get()
        try:
            payload = {
                "Command":    "add_beacon",
                "Mode":       self.mode_var.get(),  # add this
                "name":       val("Name"),
                "mac":        val("MAC"),
                "major":      int(val("Major")),
                "minor":      int(val("Minor")),
                "rssi_ref":   int(val("RSSI Ref")),
                "left_name":  val("Left Name"),
                "right_name": val("Right Name"),
            }
            self._send(payload)
        except ValueError as ex:
            self._log(f"Input error: {ex}", "err")

    def _send_remove(self):
        mac = self.rem_mac.get()
        self._send({
            "Command": "remove_beacon",
            "Mode":    self.mode_var.get(),  # add this
            "mac":     mac
        })

    def _send_list(self):
        self._send({
            "Command": "list_beacons",
            "Mode":    self.mode_var.get()  # add this
        })
 
    def _send_raw(self):
        raw = self.raw_entry.get().strip()
        try:
            payload = json.loads(raw)
            self._send(payload)
        except json.JSONDecodeError as ex:
            self._log(f"Invalid JSON: {ex}", "err")
 
    # ── connection ────────────────────────────────────────────────────────────
 
    def _toggle_connection(self):
        if self.ser and self.ser.is_open:
            self._disconnect()
        else:
            self._connect()
 
    def _connect(self):
        port = self.port_var.get()
        baud = int(self.baud_var.get())
        try:
            self.ser = serial.Serial(port, baud, timeout=1)
            self.running = True
            self.read_thread = threading.Thread(target=self._read_loop, daemon=True)
            self.read_thread.start()
            self.status_lbl.config(text=f"● {port} @ {baud}", fg=ACCENT2)
            self.conn_btn.config(text="DISCONNECT", fg=WARN)
            self._log(f"Connected to {port} @ {baud}", "info")
        except serial.SerialException as ex:
            self._log(f"Connection failed: {ex}", "err")
 
    def _disconnect(self):
        self.running = False
        if self.ser:
            self.ser.close()
            self.ser = None
        self.status_lbl.config(text="● disconnected", fg=WARN)
        self.conn_btn.config(text="CONNECT", fg=ACCENT2)
        self._log("Disconnected", "info")
 
    def _read_loop(self):
        while self.running and self.ser and self.ser.is_open:
            try:
                line = self.ser.readline().decode("utf-8", errors="replace").strip()
                if line:
                    self.after(0, self._log, line, "rx")
            except serial.SerialException:
                self.after(0, self._log, "Serial error — disconnected", "err")
                self.after(0, self._disconnect)
                break
 
    def destroy(self):
        self._disconnect()
        super().destroy()
 
 
if __name__ == "__main__":
    # style ttk widgets to match dark theme
    app = BeaconGUI()
    style = ttk.Style(app)
    style.theme_use("clam")
    style.configure("TCombobox",
                    fieldbackground=ENTRY_BG, background=PANEL,
                    foreground=TEXT, selectbackground=BORDER,
                    selectforeground=TEXT, bordercolor=BORDER,
                    arrowcolor=ACCENT)
    app.mainloop()