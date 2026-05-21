import sys
import os

# === DEBUG ===
DEBUG_MODE = True

DEBUG_OUT = sys.stdout
PROD_OUT = open(os.devnull, 'w')

OUT_FILE = DEBUG_OUT if DEBUG_MODE else PROD_OUT

# === LIMITS ===
MAX_X_POS = 3400  # mm
MAX_Y_POS = 7000  # mm
MAX_Z_POS = 2500  # mm

# === SERIAL COMMS ===
# BASESTATION_SERIAL_PORT = "/dev/tty.usbmodem12301"
BASESTATION_SERIAL_PORT = "/dev/ttyACM0"

# === GUI COMMS ===
TOKEN = "97770776-fec5-408e-ac0a-d135cb20da22"
URL = "https://api.tago.io/data"
HEADERS = {
    "Content-Type": "application/json",
    "Device-Token": TOKEN
}

# === BEACONS ===
BEACON_4011_A = { "F5:75:FE:85:34:67" : { "Name": "4011-A", "Major": 2753, "Minor": 32998, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_B = { "E5:73:87:06:1E:86" : { "Name": "4011-B", "Major": 32975, "Minor": 20959, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_C = { "CA:99:9E:FD:98:B1" : { "Name": "4011-C", "Major": 26679, "Minor": 40363, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_D = { "CB:1B:89:82:FF:FE" : { "Name": "4011-D", "Major": 41747, "Minor": 38800 , "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_E = { "D4:D2:A0:A4:5C:AC" : { "Name": "4011-E", "Major": 30679, "Minor": 51963, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_F = { "C1:13:27:E9:B7:7C" : { "Name": "4011-F", "Major": 6195, "Minor": 18394, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_G = { "F1:04:48:06:39:A0" : { "Name": "4011-G", "Major": 30525, "Minor": 30544, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_H = { "CA:0C:E0:DB:CE:60" : { "Name": "4011-H", "Major": 57395, "Minor": 28931, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_I = { "D4:7F:D4:7C:20:13" : { "Name": "4011-I", "Major": 60345, "Minor": 49995, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_J = { "F7:0B:21:F1:C8:E1" : { "Name": "4011-J", "Major": 12249, "Minor": 30916, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_K = { "FD:E0:8D:FA:3E:4A" : { "Name": "4011-K", "Major": 36748, "Minor": 11457, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_L = { "EE:32:F7:28:FA:AC" : { "Name": "4011-L", "Major": 27564, "Minor": 27589, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }
BEACON_4011_M = { "F7:3B:46:A8:D7:2C" : { "Name": "4011-M", "Major": 49247, "Minor": 52925, "Position": (0,0,0), "LeftNeighbour": "4011-B", "RightNeighbour": "4011-H" } }

DEFAULT_BEACONS = BEACON_4011_A | \
                  BEACON_4011_B | \
                  BEACON_4011_C | \
                  BEACON_4011_D | \
                  BEACON_4011_E | \
                  BEACON_4011_F | \
                  BEACON_4011_G | \
                  BEACON_4011_H | \
                  BEACON_4011_I | \
                  BEACON_4011_J | \
                  BEACON_4011_K | \
                  BEACON_4011_L | \
                  BEACON_4011_M
