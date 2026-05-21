import json
import queue
import threading
from time import sleep, time
import requests

from xy_visualiser import LiveVisualiser
from serialInterface import basestation_serial_interface
from mobileposition import mobile_station_position
from constants import *

# -- Output mode flags --------------------------------------------------------
ENABLE_MATPLOTVIS = True
ENABLE_WEBAPPVIS  = True

# -- Position scaling / offset ------------------------------------------------
X_SCALER = 1.4
Y_SCALER = 1.0
X_OFFSET = 1.0
Y_OFFSET = 1.0

# -- Tuning -------------------------------------------------------------------
HTTP_POLL_INTERVAL   = 0.5   # seconds between TagoIO GET polls
HTTP_TX_BATCH_SIZE   = 20    # max packets popped per HTTP-TX iteration
UART_QUEUE_MAXSIZE   = 512   # parsed JSON dicts
HTTP_TX_QUEUE_MAXSIZE = 256  # outbound TagoIO packets


# =============================================================================
# SHARED QUEUES
# =============================================================================

uart_queue    = queue.Queue(maxsize=UART_QUEUE_MAXSIZE)   # UART → main
cmd_queue     = queue.Queue()                              # HTTP-RX → main
http_tx_queue = queue.Queue(maxsize=HTTP_TX_QUEUE_MAXSIZE) # main → HTTP-TX

# Shared state for HTTP-RX ↔ main (current_base_mode needed when building TX packets)
_state_lock      = threading.Lock()
_current_base_mode = None   # updated by main thread after processing a command


# =============================================================================
# TagoIO GET
# =============================================================================

def _tago_get(variable_name):
    """Fetch the latest value for *variable_name* from TagoIO. Returns value or None."""
    try:
        r = requests.get(URL, headers=HEADERS, params={"variable": variable_name, "qty": 1}, timeout=5)
        if r.status_code == 200:
            body = r.json()
            if body.get("status") and body.get("result"):
                return body["result"][0]["value"]
    except Exception as e:
        print(f"[HTTP-RX] GET '{variable_name}' failed: {e}", file=OUT_FILE)
    return None


# =============================================================================
# UART READER
# =============================================================================

def uart_reader_thread():
    port = basestation_serial_interface._serial_port

    while not _stop_event.is_set():
        try:
            raw = port.readline().decode('utf-8').strip()
        except Exception as e:
            print(f"[UART] Read error: {e}", file=OUT_FILE)
            sleep(0.05)
            continue

        if not raw:
            continue

        try:
            data = json.loads(raw)
            _ = data["MessageType"]          # validate required key
        except (json.JSONDecodeError, KeyError, TypeError):
            print(f"[UART] Bad packet: {raw}", file=OUT_FILE)
            port.flush()
            continue

        try:
            uart_queue.put_nowait(data)
        except queue.Full:
            print("[UART] uart_queue full — dropping oldest packet.", file=OUT_FILE)
            try:
                uart_queue.get_nowait()      # drop oldest
            except queue.Empty:
                pass
            uart_queue.put_nowait(data)


# =============================================================================
# HTTP RECEIVE 
# =============================================================================

def http_rx_thread():
    print("[HTTP-RX] Poll thread started.", file=OUT_FILE)

    last_base_mode             = None
    last_beacon_add_request    = None
    last_beacon_del_request    = None
    last_beacon_send_list_request = None

    while not _stop_event.is_set():
        poll_start = time()

        if ENABLE_WEBAPPVIS:
            # ---- base mode --------------------------------------------------
            val = _tago_get("basemode")
            if last_base_mode is None:
                last_base_mode = val
            elif val is not None and val != last_base_mode:
                cmd_queue.put({"cmd": "set_mode", "value": val})
                last_base_mode = val

            # ---- add beacon -------------------------------------------------
            val = _tago_get("new_beacon")
            if last_beacon_add_request is None:
                last_beacon_add_request = val
            elif val is not None and val != last_beacon_add_request:
                cmd_queue.put({"cmd": "add_beacon", "value": val})
                last_beacon_add_request = val

            # ---- delete beacon ----------------------------------------------
            val = _tago_get("del_beacon")
            if last_beacon_del_request is None:
                last_beacon_del_request = val
            elif val is not None and val != last_beacon_del_request:
                cmd_queue.put({"cmd": "del_beacon", "value": val})
                last_beacon_del_request = val

            # ---- list beacons -----------------------------------------------
            val = _tago_get("list_beacon_button")
            if last_beacon_send_list_request is None:
                last_beacon_send_list_request = val
            elif val is not None and val != last_beacon_send_list_request:
                cmd_queue.put({"cmd": "list_beacons", "value": val})
                last_beacon_send_list_request = val

        # Sleep for the remainder of the poll interval
        elapsed = time() - poll_start
        sleep(max(0.0, HTTP_POLL_INTERVAL - elapsed))


# =============================================================================
# HTTP TRANSMIT
# =============================================================================

def http_tx_thread():
    print("[HTTP-TX] Transmit thread started.", file=OUT_FILE)

    while not _stop_event.is_set():
        batch = []
        try:
            # Block until at least one group of variables is available, then flatten it in
            batch.extend(http_tx_queue.get(timeout=1.0))
        except queue.Empty:
            continue

        # Drain any additional groups that arrived in the meantime, keeping the list flat
        for _ in range(HTTP_TX_BATCH_SIZE - 1):
            try:
                batch.extend(http_tx_queue.get_nowait())
            except queue.Empty:
                break

        try:
            r = requests.post(URL, data=json.dumps(batch), headers=HEADERS, timeout=5)
            # print(f"[HTTP-TX] POST {len(batch)} variable(s): {r.text}", file=OUT_FILE)
        except Exception as e:
            print(f"[HTTP-TX] POST failed: {e}", file=OUT_FILE)


# =============================================================================
# Build and enqueue a serial TX packet
# =============================================================================

def _serial_send(obj: dict):
    msg = json.dumps(obj) + '\n'
    print(f"[SERIAL-TX] {msg.strip()}", file=OUT_FILE)
    basestation_serial_interface._serial_send(msg.encode('utf-8'))


def _base_mode_str():
    with _state_lock:
        return "base" if _current_base_mode else "sniffer"


def _enqueue_delayed(packet: list, delay: float):
    def _worker():
        sleep(delay)
        if not _stop_event.is_set():
            http_tx_queue.put_nowait(packet)
    threading.Thread(target=_worker, daemon=True).start()


# =============================================================================
# COMMAND HANDLERS
# =============================================================================

def handle_set_mode(value):
    global _current_base_mode
    print(f"[CMD] set_mode {value}", file=OUT_FILE)
    with _state_lock:
        _current_base_mode = value
    _serial_send({"Command": "set_mode", "Mode": "base" if value else "sniffer"})


def handle_add_beacon(value):
    print(f"[CMD] add_beacon {value}", file=OUT_FILE)
    req = json.loads(value)
    _serial_send({
        "Command":    "add_beacon",
        "Mode":       _base_mode_str(),
        "name":       req["name"],
        "mac":        req["mac"],
        "major":      int(req["major"]),
        "minor":      int(req["minor"]),
        "left_name":  req["leftname"],
        "right_name": req["rightname"],
        "rssi_ref":   int(req["rssi_ref"]),
        "X":          int(float(req["x_fix"]) * 100),
        "Y":          int(float(req["y_fix"]) * 100),
    })


def handle_del_beacon(value):
    print(f"[CMD] del_beacon → {value}", file=OUT_FILE)
    req = json.loads(value)
    _serial_send({"Command": "remove_beacon", "Mode": _base_mode_str(), "mac": req["mac"]})


def handle_list_beacons(_value):
    print("[CMD] list_beacons", file=OUT_FILE)
    _serial_send({"Command": "list_beacons", "Mode": _base_mode_str()})


COMMAND_HANDLERS = {
    "set_mode":    handle_set_mode,
    "add_beacon":  handle_add_beacon,
    "del_beacon":  handle_del_beacon,
    "list_beacons": handle_list_beacons,
}


# =============================================================================
# SET UP
# =============================================================================

beacons    = DEFAULT_BEACONS.copy()
_stop_event = threading.Event()

# -- Matplotlib visualiser (background daemon) --------------------------------
if ENABLE_MATPLOTVIS:
    vis = LiveVisualiser(x_range=(-5, 15), y_range=(-5, 15), trail_length=100)
    vis.start()
    print("[MAIN] LiveVisualiser started.", file=OUT_FILE)

# -- Start worker threads -----------------------------------------------------
threads = [
    threading.Thread(target=uart_reader_thread, name="UART-RX",  daemon=True),
    threading.Thread(target=http_rx_thread,     name="HTTP-RX",  daemon=True),
    threading.Thread(target=http_tx_thread,     name="HTTP-TX",  daemon=True),
]
for t in threads:
    t.start()


# =============================================================================
# MAIN LOOP
# =============================================================================

try:
    while True:

        # -- Process dashboard commands -------------------------
        while not cmd_queue.empty():
            try:
                cmd = cmd_queue.get_nowait()
                handler = COMMAND_HANDLERS.get(cmd["cmd"])
                if handler:
                    handler(cmd["value"])
                else:
                    print(f"[MAIN] Unknown command: {cmd}", file=OUT_FILE)
            except queue.Empty:
                break

        # -- Process incoming UART packets ----------------------
        while not uart_queue.empty():
            try:
                data_in = uart_queue.get_nowait()
            except queue.Empty:
                break

            msg_type = data_in.get("MessageType")

            # ---- Position Data ----------------------------------------------
            if msg_type == "PositionData":
                # print("[MAIN] PositionData received.", file=OUT_FILE)
                newpos = data_in["Data"]

                x_out = (newpos["y"] * X_SCALER) - X_OFFSET
                y_out = (newpos["x"] * Y_SCALER) - Y_OFFSET
                vx_out = newpos["vx"]
                vy_out = newpos["vy"]

                mobile_station_position.update_position(x_out, y_out, 0, data_in["Timestamp"])
                # print(f"  x={mobile_station_position._x}  "
                #       f"y={mobile_station_position._y}  "
                #       f"z={mobile_station_position._z}", file=OUT_FILE)

                if ENABLE_MATPLOTVIS:
                    vis.update(mobile_station_position._x, mobile_station_position._y)

                if ENABLE_WEBAPPVIS:
                    http_tx_queue.put_nowait([
                        {
                            "variable": "mobileposition",
                            "value":    1,
                            "metadata": {
                                "x":     mobile_station_position._x,
                                "y":     mobile_station_position._y,
                                "color": "cyan",
                            },
                        },
                        {"variable": "averagevelocity",  "value": (vx_out**2 + vy_out**2)**0.5,                     "unit": "m/s"},
                        {"variable": "totaldistance",    "value": mobile_station_position._total_distance_traveled, "unit": "m"},
                    ])

            # ---- BLE Sniffer Data ------------------------------------------
            elif msg_type == "Sniffer":
                # print("[MAIN] Sniffer packet received.", file=OUT_FILE)
                ble = data_in["Data"]

                if ENABLE_WEBAPPVIS:
                    http_tx_queue.put_nowait([
                        {"variable": "MACAddress", "value": ble["BLEMAC"]},
                        {"variable": "Major",      "value": ble["BLEMajor"]},
                        {"variable": "Minor",      "value": ble["BLEMinor"]},
                        {"variable": "RSSI",       "value": ble["RSSI"]},
                    ])

            # ---- Saved Beacon Data -----------------------------------------
            elif msg_type == "SavedBeacon":
                print("[MAIN] SavedBeacon received.", file=OUT_FILE)
                beacon = data_in["Data"]
                print(beacon, file=OUT_FILE)

                if ENABLE_WEBAPPVIS:
                    _enqueue_delayed([
                        {"variable": "saved_blename",    "value": beacon["BLEName"]},
                        {"variable": "saved_macaddress", "value": beacon["BLEMAC"]},
                        {"variable": "saved_major",      "value": beacon["BLEMajor"]},
                        {"variable": "saved_minor",      "value": beacon["BLEMinor"]},
                    ], delay=0.5)

        sleep(0.005)

except KeyboardInterrupt:
    print("\n[MAIN] Shutting down…", file=OUT_FILE)

finally:
    _stop_event.set()
    if ENABLE_MATPLOTVIS:
        vis.stop()