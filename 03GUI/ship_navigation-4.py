#!/usr/bin/env python3
"""
Ship Navigation GUI
Parses Zephyr log lines containing embedded JSON:
  [00:00:22.950,225] <inf> nus_central: [IMU] {"SteeringAngle":401.79, "CumAngle":401.79, "GyroZ":0.46}

Requirements:
    pip install pygame pyserial

Usage:
    python ship_navigation.py
    python ship_navigation.py --port /dev/tty.usbmodem12401 --baud 115200
"""

import pygame
import serial
import serial.tools.list_ports
import threading
import json
import math
import argparse
import sys
import re
import collections
import random
import array
import math as _math

# ── Music ─────────────────────────────────────────────────────────────────────
_MUSIC_RATE  = 22050
_music_sound = None
_music_idx   = 0

def _build_tone(freq, dur, vol=0.25):
    n   = int(_MUSIC_RATE * dur)
    buf = array.array('h')
    for i in range(n):
        t   = i / _MUSIC_RATE
        env = min(1.0, min(i / (_MUSIC_RATE * 0.01), (n - i) / (_MUSIC_RATE * 0.08)))
        buf.append(int(vol * env * 32767 * _math.sin(2 * _math.pi * freq * t)))
    stereo = array.array('h')
    for s in buf:
        stereo.append(s)
        stereo.append(s)
    import numpy as np
    arr = np.frombuffer(stereo, dtype=np.int16).reshape(-1, 2)
    return pygame.sndarray.make_sound(arr)

def _note(name):
    notes = {'C':0,'C#':1,'D':2,'D#':3,'E':4,'F':5,'F#':6,'G':7,'G#':8,'A':9,'A#':10,'B':11}
    n = name[:-1] if name[-2] not in '#b' else name[:-1]
    oct = int(name[-1])
    st  = notes[n] + (oct - 4) * 12
    return 440.0 * (2 ** (st / 12.0))

_MELODY = [
    ('E4',0.25),('F#4',0.25),('G4',0.25),('A4',0.25),
    ('B4',0.375),('A4',0.125),('B4',0.5),
    ('G4',0.25),('A4',0.25),('B4',0.25),('C5',0.25),
    ('D5',0.375),('C5',0.125),('D5',0.5),
    ('E5',0.375),('D5',0.125),('C5',0.25),('B4',0.25),
    ('A4',0.375),('G4',0.125),('F#4',0.25),('E4',0.25),
    ('F#4',0.25),('G4',0.25),('A4',0.5),('B4',0.5),
]
_melody_sounds = []
_melody_built  = False

MUSIC_NOTE_EVENT = pygame.USEREVENT + 1

def init_music():
    """Call after pygame.init() — builds tones on main thread."""
    global _melody_sounds, _melody_built
    try:
        pygame.mixer.init(frequency=_MUSIC_RATE, size=-16, channels=2, buffer=256)
        _melody_sounds = [_build_tone(_note(n), d) for n, d in _MELODY]
        _melody_built  = True
        _schedule_next(0)
    except Exception as e:
        print(f"Music init failed: {e}")

def _schedule_next(idx):
    if not _melody_built:
        return
    _, dur = _MELODY[idx % len(_MELODY)]
    pygame.time.set_timer(MUSIC_NOTE_EVENT, int(dur * 1000), loops=1)

def handle_music_event(idx_ref):
    """Call from event loop when event.type == MUSIC_NOTE_EVENT."""
    if not _melody_built:
        return idx_ref[0]
    i = idx_ref[0] % len(_MELODY)
    _melody_sounds[i].play()
    idx_ref[0] = i + 1
    _schedule_next(idx_ref[0])
    return idx_ref[0]



# ── Config ───────────────────────────────────────────────────────────────────
OCEAN_W  = 1100
OCEAN_H  = 580
CTRL_H   = 50
RAW_H    = 180
WIDTH    = OCEAN_W
HEIGHT   = OCEAN_H + CTRL_H + RAW_H
FPS      = 60
MARGIN   = 20
SHIP_SPEED  = 0.8
TURN_RATE   = 0.015
TRAIL_LEN   = 120
RAW_LOG_MAX = 12

JSON_RE = re.compile(r'\{.*\}')

WATER_COLOR  = (10, 22, 40)
BORDER_COLOR = (60, 120, 180)
PANEL_COLOR  = (14, 18, 30)
PANEL_BORDER = (40, 60, 90)
CTRL_COLOR   = (18, 24, 38)
HUD_TEXT     = (100, 180, 255)
NORTH_COLOR  = (255, 100, 100)
COMPASS_TEXT = (180, 210, 240)
SHIP_BODY    = (200, 220, 240)
SHIP_DECK    = (140, 180, 210)
SAIL_COLOR   = (180, 210, 240)
RAW_JSON     = (100, 200, 130)
RAW_OTHER    = (140, 150, 160)
RAW_ERR      = (220, 100, 80)
RAW_LABEL    = (70, 110, 150)


# ── Music synthesis ──────────────────────────────────────────────────────────


# ── Obstacles ─────────────────────────────────────────────────────────────────
class Obstacle:
    def __init__(self, x, y, radius, kind):
        self.x      = x
        self.y      = y
        self.radius = radius
        self.kind   = kind  # 'rock' or 'lighthouse'

OBSTACLES = [
    Obstacle(300,  200, 18, 'rock'),
    Obstacle(700,  150, 14, 'rock'),
    Obstacle(1100, 300, 22, 'rock'),
    Obstacle(500,  550, 16, 'rock'),
    Obstacle(900,  480, 12, 'rock'),
    Obstacle(200,  600, 20, 'rock'),
    Obstacle(1200, 600, 15, 'rock'),
    Obstacle(600,  350, 24, 'lighthouse'),
    Obstacle(1050, 180, 16, 'lighthouse'),
]


def draw_rock(surf, x, y, r):
    # bumpy rock shape using polygon
    pts = []
    n = 8
    for i in range(n):
        angle = 2 * math.pi * i / n
        jitter = r * (0.7 + 0.3 * ((i * 7 + 3) % 5) / 4)
        pts.append((int(x + math.cos(angle) * jitter),
                    int(y + math.sin(angle) * jitter)))
    pygame.draw.polygon(surf, (80, 75, 70), pts)
    pygame.draw.polygon(surf, (110, 105, 95), pts, 2)
    # highlight
    pygame.draw.circle(surf, (120, 115, 108), (int(x - r*0.25), int(y - r*0.25)), max(2, r//4))


def draw_lighthouse(surf, x, y, r, tick, font_sm):
    # base
    base = [(x-r, y+r), (x+r, y+r), (x+r*0.6, y-r), (x-r*0.6, y-r)]
    pygame.draw.polygon(surf, (200, 80, 60), base)
    pygame.draw.polygon(surf, (240, 120, 90), base, 2)
    # stripes
    stripe_y = y + r * 0.3
    pygame.draw.rect(surf, (240, 240, 230),
                     (int(x - r*0.55), int(stripe_y), int(r*1.1), int(r*0.25)))
    # lantern room
    pygame.draw.rect(surf, (60, 60, 70),
                     (int(x - r*0.55), int(y - r*1.1), int(r*1.1), int(r*0.4)),
                     border_radius=2)
    # rotating light beam
    beam_angle = (tick * 0.04) % (2 * math.pi)
    beam_len   = 80
    bx = int(x + math.cos(beam_angle) * beam_len)
    by = int(y - r + math.sin(beam_angle) * beam_len)
    beam_surf = pygame.Surface((OCEAN_W, OCEAN_H), pygame.SRCALPHA)
    pygame.draw.line(beam_surf, (255, 255, 150, 40), (int(x), int(y - r)), (bx, by), 6)
    surf.blit(beam_surf, (0, 0))
    # glowing light
    glow = pygame.Surface((20, 20), pygame.SRCALPHA)
    alpha = 180 + int(60 * math.sin(tick * 0.1))
    pygame.draw.circle(glow, (255, 255, 120, alpha), (10, 10), 5)
    surf.blit(glow, (int(x) - 10, int(y - r) - 10))
    # label
    lbl = font_sm.render("lighthouse", True, (180, 160, 120))
    surf.blit(lbl, (int(x) - lbl.get_width()//2, int(y + r + 4)))


def draw_obstacles(surf, obstacles, tick, font_sm):
    for obs in obstacles:
        if obs.kind == 'rock':
            draw_rock(surf, obs.x, obs.y, obs.radius)
        elif obs.kind == 'lighthouse':
            draw_lighthouse(surf, obs.x, obs.y, obs.radius, tick, font_sm)


def check_obstacle_collision(ship, obstacles):
    for obs in obstacles:
        dx = ship.x - obs.x
        dy = ship.y - obs.y
        dist = math.hypot(dx, dy)
        if dist < obs.radius + 12:
            # push ship away
            if dist > 0:
                nx, ny = dx / dist, dy / dist
            else:
                nx, ny = 1.0, 0.0
            ship.x = obs.x + nx * (obs.radius + 13)
            ship.y = obs.y + ny * (obs.radius + 13)
            # reflect heading
            dot = math.cos(ship.heading) * nx + math.sin(ship.heading) * ny
            ship.heading = math.atan2(
                math.sin(ship.heading) - 2 * dot * ny,
                math.cos(ship.heading) - 2 * dot * nx
            )


# ── Serial reader ─────────────────────────────────────────────────────────────
class SerialReader:
    def __init__(self, port, baud):
        self.port           = port
        self.baud           = baud
        self.steering_angle = 0.0
        self.prev_steering  = None
        self.delta_angle    = 0.0
        self.gyro_z         = 0.0
        self.connected      = False
        self.error          = None
        self.raw_lines       = collections.deque(maxlen=RAW_LOG_MAX)
        self.scanned_colours = set()
        self._lock           = threading.Lock()
        self._thread         = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def get_raw_lines(self):
        with self._lock:
            return list(self.raw_lines)

    def _add_raw(self, line, kind="other"):
        with self._lock:
            self.raw_lines.append((line, kind))

    def get_scanned(self):
        with self._lock:
            return set(self.scanned_colours)

    def send_raw(self, data: bytes):
        """Send raw bytes over the serial port."""
        try:
            if hasattr(self, '_ser') and self._ser and self._ser.is_open:
                self._ser.write(data)
        except Exception as e:
            print(f"Send error: {e}")

    def _run(self):
        try:
            ser = serial.Serial(self.port, self.baud, timeout=1)
            self._ser = ser
            self.connected = True
            print(f"Connected to {self.port} at {self.baud} baud")
            while True:
                raw = ser.readline().decode("utf-8", errors="ignore").strip()
                if not raw:
                    continue
                self._add_raw(raw, kind="other")
                m = JSON_RE.search(raw)
                if m:
                    try:
                        d = json.loads(m.group())
                        if "SteeringAngle" in d:
                            cur = float(d["SteeringAngle"])
                            if self.prev_steering is not None:
                                self.delta_angle = cur - self.prev_steering
                            else:
                                self.delta_angle = 0.0
                            self.prev_steering  = cur
                            self.steering_angle = cur
                            self.gyro_z         = float(d.get("GyroZ", 0))
                            with self._lock:
                                if self.raw_lines:
                                    self.raw_lines[-1] = (raw, "json")
                        if d.get("TYPE") == "rfid":
                            colour = d.get("SCANNED", "").lower()
                            if colour in ("yellow", "green", "purple"):
                                with self._lock:
                                    self.scanned_colours.add(colour)
                                    if self.raw_lines:
                                        self.raw_lines[-1] = (raw, "json")
                    except json.JSONDecodeError:
                        self._add_raw("  [JSON parse error]", kind="err")
        except serial.SerialException as e:
            self.error     = str(e)
            self.connected = False
            self._add_raw(f"[serial error: {e}]", kind="err")


# ── Ship ──────────────────────────────────────────────────────────────────────
class Ship:
    def __init__(self, x, y):
        self.x       = float(x)
        self.y       = float(y)
        self.heading = -math.pi / 2
        self.trail   = []

    def update(self, delta_deg):
        self.heading += math.radians(delta_deg) * TURN_RATE
        self.x += math.cos(self.heading) * SHIP_SPEED
        self.y += math.sin(self.heading) * SHIP_SPEED
        self._clamp()
        self.trail.append((self.x, self.y))
        if len(self.trail) > TRAIL_LEN:
            self.trail.pop(0)

    def _clamp(self):
        vx, vy   = math.cos(self.heading), math.sin(self.heading)
        nvx, nvy = vx, vy
        if self.x < MARGIN:            self.x = MARGIN;            nvx = max(nvx, 0)
        if self.x > OCEAN_W - MARGIN:  self.x = OCEAN_W - MARGIN;  nvx = min(nvx, 0)
        if self.y < MARGIN:            self.y = MARGIN;             nvy = max(nvy, 0)
        if self.y > OCEAN_H - MARGIN:  self.y = OCEAN_H - MARGIN;  nvy = min(nvy, 0)
        if nvx != vx or nvy != vy:
            self.heading = (self.heading + math.pi) if (nvx == 0 and nvy == 0) \
                           else math.atan2(nvy, nvx)

    def reset(self):
        self.x       = OCEAN_W / 2
        self.y       = OCEAN_H / 2
        self.heading = -math.pi / 2
        self.trail   = []


# ── Drawing ───────────────────────────────────────────────────────────────────
def draw_water(surf):
    surf.fill(WATER_COLOR, (0, 0, OCEAN_W, OCEAN_H))
    for y in range(0, OCEAN_H, 30):
        for x in range(0, OCEAN_W, 60):
            pygame.draw.arc(surf, (30, 50, 80), (x, y - 4, 30, 8), 0, math.pi, 1)
    pygame.draw.rect(surf, BORDER_COLOR,
                     (MARGIN, MARGIN, OCEAN_W - MARGIN*2, OCEAN_H - MARGIN*2), 2, border_radius=4)


def draw_trail(surf, trail):
    if len(trail) < 2:
        return
    ts = pygame.Surface((OCEAN_W, OCEAN_H), pygame.SRCALPHA)
    for i in range(1, len(trail)):
        alpha = int(120 * i / len(trail))
        pygame.draw.line(ts, (80, 160, 240, alpha),
                         (int(trail[i-1][0]), int(trail[i-1][1])),
                         (int(trail[i][0]),   int(trail[i][1])), 2)
    surf.blit(ts, (0, 0))


def draw_ship(surf, x, y, heading):
    ss = pygame.Surface((60, 60), pygame.SRCALPHA)
    cx, cy = 30, 30
    hull = [(cx, cy-22), (cx+12, cy-6), (cx+10, cy+14), (cx-10, cy+14), (cx-12, cy-6)]
    pygame.draw.polygon(ss, (255, 182, 193), hull)
    pygame.draw.polygon(ss, (220, 100, 140), hull, 2)
    pygame.draw.polygon(ss, (255, 150, 170),
                        [(cx-8, cy+2), (cx+8, cy+2), (cx+8, cy+10), (cx-8, cy+10)])
    pygame.draw.line(ss, (255, 200, 210), (cx, cy-2), (cx, cy-18), 2)
    sl = pygame.Surface((60, 60), pygame.SRCALPHA)
    pygame.draw.polygon(sl, (255, 220, 230, 180), [(cx, cy-18), (cx+10, cy-10), (cx, cy-5)])
    ss.blit(sl, (0, 0))
    pygame.draw.circle(ss, (255, 240, 245), (cx, cy+5), 3)
    rotated = pygame.transform.rotate(ss, -(math.degrees(heading) + 90))
    surf.blit(rotated, rotated.get_rect(center=(int(x), int(y))))



def draw_compass(surf, x, y, heading, font_sm):
    r  = 36
    cs = pygame.Surface((r*2+4, r*2+4), pygame.SRCALPHA)
    cx, cy = r+2, r+2
    pygame.draw.circle(cs, (*WATER_COLOR, 200), (cx, cy), r)
    pygame.draw.circle(cs, (80, 130, 190, 120), (cx, cy), r, 1)
    for label, angle in [("N", 0), ("E", math.pi/2), ("S", math.pi), ("W", -math.pi/2)]:
        lx = cx + int(math.sin(angle) * (r - 10))
        ly = cy - int(math.cos(angle) * (r - 10))
        color = NORTH_COLOR if label == "N" else COMPASS_TEXT
        t = font_sm.render(label, True, color)
        cs.blit(t, t.get_rect(center=(lx, ly)))
    na = heading + math.pi/2
    pygame.draw.line(cs, NORTH_COLOR, (cx, cy),
                     (cx + int(math.sin(na)*(r-6)), cy - int(math.cos(na)*(r-6))), 3)
    pygame.draw.line(cs, (180, 210, 240), (cx, cy),
                     (cx - int(math.sin(na)*(r-6)), cy + int(math.cos(na)*(r-6))), 2)
    surf.blit(cs, (x - r - 2, y - r - 2))


def draw_hud(surf, heading, cum_angle, delta, gyro, connected, font_sm):
    hud = pygame.Surface((230, 110), pygame.SRCALPHA)
    pygame.draw.rect(hud, (*WATER_COLOR, 190), (0, 0, 230, 110), border_radius=8)
    deg = math.degrees(heading) % 360
    rows = [
        (f"Heading:   {deg:7.1f}°",       HUD_TEXT),
        (f"Cum angle: {cum_angle:7.1f}°",  HUD_TEXT),
        (f"Delta:     {delta:+7.2f}°",     HUD_TEXT),
        (f"GyroZ:     {gyro:7.3f}",        HUD_TEXT),
        (f"UART: {'connected' if connected else 'disconnected'}",
         (80, 200, 120) if connected else (255, 120, 80)),
    ]
    for i, (line, color) in enumerate(rows):
        hud.blit(font_sm.render(line, True, color), (10, 8 + i * 20))
    surf.blit(hud, (10, 10))


def draw_ctrl_bar(surf, oy, ports, selected_idx, font_sm, font_lbl, connected, buttons):
    pygame.draw.rect(surf, CTRL_COLOR, (0, oy, WIDTH, CTRL_H))
    pygame.draw.line(surf, PANEL_BORDER, (0, oy), (WIDTH, oy), 1)
    surf.blit(font_lbl.render("PORT:", True, RAW_LABEL), (12, oy + 16))

    box_x, box_y = 60, oy + 8
    box_w, box_h = 340, 30
    pygame.draw.rect(surf, (20, 30, 50), (box_x, box_y, box_w, box_h), border_radius=4)
    pygame.draw.rect(surf, PANEL_BORDER, (box_x, box_y, box_w, box_h), 1, border_radius=4)
    text = ports[selected_idx] if ports and selected_idx < len(ports) else "no ports found"
    surf.blit(font_sm.render(text, True, (180, 200, 220)), (box_x + 8, box_y + 8))
    surf.blit(font_sm.render("▼", True, RAW_LABEL), (box_x + box_w - 20, box_y + 8))
    buttons["dropdown"] = pygame.Rect(box_x, box_y, box_w, box_h)

    btn_x = box_x + box_w + 12
    btn_color  = (30, 80, 40)  if connected else (30, 50, 80)
    btn_border = (60, 160, 80) if connected else (60, 120, 180)
    pygame.draw.rect(surf, btn_color,  (btn_x, box_y, 100, box_h), border_radius=4)
    pygame.draw.rect(surf, btn_border, (btn_x, box_y, 100, box_h), 1, border_radius=4)
    surf.blit(font_sm.render("Disconnect" if connected else "Connect",
                              True, (200, 230, 200) if connected else (160, 200, 240)),
              (btn_x + 8, box_y + 8))
    buttons["connect"] = pygame.Rect(btn_x, box_y, 100, box_h)

    ref_x = btn_x + 110
    pygame.draw.rect(surf, (25, 35, 55), (ref_x, box_y, 80, box_h), border_radius=4)
    pygame.draw.rect(surf, PANEL_BORDER, (ref_x, box_y, 80, box_h), 1, border_radius=4)
    surf.blit(font_sm.render("Refresh", True, RAW_LABEL), (ref_x + 8, box_y + 8))
    buttons["refresh"] = pygame.Rect(ref_x, box_y, 80, box_h)


    zero_x = ref_x + 90
    pygame.draw.rect(surf, (50, 30, 10), (zero_x, box_y, 100, box_h), border_radius=4)
    pygame.draw.rect(surf, (200, 140, 60), (zero_x, box_y, 100, box_h), 1, border_radius=4)
    surf.blit(font_sm.render("Zero IMU", True, (220, 170, 80)), (zero_x + 8, box_y + 8))
    buttons["zero"] = pygame.Rect(zero_x, box_y, 100, box_h)

    home_x = zero_x + 110
    pygame.draw.rect(surf, (20, 40, 60), (home_x, box_y, 90, box_h), border_radius=4)
    pygame.draw.rect(surf, (60, 120, 180), (home_x, box_y, 90, box_h), 1, border_radius=4)
    surf.blit(font_sm.render("Home", True, (120, 180, 240)), (home_x + 18, box_y + 8))
    buttons["home"] = pygame.Rect(home_x, box_y, 90, box_h)


def draw_dropdown(surf, ports, selected_idx, ctrl_oy, font_sm):
    box_x = 60
    for i, port in enumerate(ports):
        item_rect = pygame.Rect(box_x, ctrl_oy + 8 + (i+1)*30, 340, 26)
        bg = (30, 50, 80) if i == selected_idx else (20, 30, 50)
        pygame.draw.rect(surf, bg, item_rect, border_radius=3)
        pygame.draw.rect(surf, PANEL_BORDER, item_rect, 1, border_radius=3)
        surf.blit(font_sm.render(port, True, (180, 200, 220)),
                  (item_rect.x + 8, item_rect.y + 5))
    return [pygame.Rect(box_x, ctrl_oy + 8 + (i+1)*30, 340, 26) for i in range(len(ports))]


def draw_raw_panel(surf, raw_lines, font_sm, font_lbl, oy):
    pygame.draw.rect(surf, PANEL_COLOR, (0, oy, WIDTH, RAW_H))
    pygame.draw.line(surf, PANEL_BORDER, (0, oy), (WIDTH, oy), 1)
    surf.blit(font_lbl.render("RAW UART DATA", True, RAW_LABEL), (12, oy + 6))
    y = oy + 24
    if not raw_lines:
        surf.blit(font_sm.render("waiting for data...", True, RAW_LABEL), (12, y))
        return
    for line, kind in raw_lines:
        color = RAW_JSON if kind == "json" else RAW_ERR if kind == "err" else RAW_OTHER
        max_chars = (WIDTH - 20) // 7
        display = line if len(line) <= max_chars else line[:max_chars-3] + "..."
        surf.blit(font_sm.render(display, True, color), (12, y))
        y += 16
        if y > oy + RAW_H - 6:
            break


def draw_startup_screen(surf, scanned, tick, font_title, font_med, font_sm):
    surf.fill((8, 16, 32))
    for y in range(0, HEIGHT, 40):
        offset = int(10 * math.sin(tick * 0.03 + y * 0.05))
        pygame.draw.line(surf, (15, 35, 60), (0, y + offset), (WIDTH, y + offset), 1)

    title = font_title.render("MAKING WAVES", True, (180, 220, 255))
    surf.blit(title, title.get_rect(center=(WIDTH//2, HEIGHT//4)))

    n_req = 3
    sub = font_med.render("scan all cards to begin", True, (80, 120, 160))
    surf.blit(sub, sub.get_rect(center=(WIDTH//2, HEIGHT//4 + 50)))

    all_cards = [
        ("yellow", (220, 200,  50), (80,  70, 10)),
        ("green",  ( 60, 200,  80), (15,  60, 20)),
        ("purple", (160,  80, 220), (50,  20, 70)),
    ]
    cards = all_cards

    slot_w, slot_h = 200, 120
    spacing = 60
    total_w = len(cards) * slot_w + (len(cards)-1) * spacing
    start_x = WIDTH//2 - total_w//2
    cy      = HEIGHT//2 + 20

    for i, (colour, rgb, dark) in enumerate(cards):
        x    = start_x + i * (slot_w + spacing)
        done = colour in scanned
        bg   = rgb if done else dark
        s    = pygame.Surface((slot_w, slot_h), pygame.SRCALPHA)
        pygame.draw.rect(s, (*bg, 180 if done else 60),   (0, 0, slot_w, slot_h), border_radius=12)
        pygame.draw.rect(s, (*rgb, 255 if done else 180), (0, 0, slot_w, slot_h), 2, border_radius=12)
        surf.blit(s, (x, cy - slot_h//2))
        mark = font_title.render("checkmark" if done else "?", True, (255,255,255) if done else rgb)
        if done:
            mark = font_title.render("v", True, (255, 255, 255))
        else:
            mark = font_title.render("?", True, rgb)
        surf.blit(mark, mark.get_rect(center=(x + slot_w//2, cy - 10)))
        lbl = font_med.render(colour, True, (220, 220, 220) if done else (120, 120, 120))
        surf.blit(lbl, lbl.get_rect(center=(x + slot_w//2, cy + slot_h//2 - 18)))

    n_scanned = len(scanned)
    prog = font_sm.render(f"{n_scanned}/{n_req} cards scanned", True, (100, 160, 200))
    surf.blit(prog, prog.get_rect(center=(WIDTH//2, cy + slot_h//2 + 30)))

    if len(scanned) >= 3:
        go = font_med.render("ALL CARDS SCANNED - starting...", True, (100, 255, 140))
        surf.blit(go, go.get_rect(center=(WIDTH//2, cy + slot_h//2 + 60)))

    hint = font_sm.render("press SPACE to skip (demo mode)", True, (40, 60, 80))
    surf.blit(hint, hint.get_rect(center=(WIDTH//2, HEIGHT - 30)))


def get_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


def list_ports():
    ports = get_ports()
    if not ports:
        print("No serial ports found.")
    else:
        print("Available ports:")
        for p in ports:
            print(f"  {p}")


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Ship Navigation GUI")
    parser.add_argument("--port",       type=str, default=None)
    parser.add_argument("--baud",       type=int, default=115200)
    parser.add_argument("--list-ports", action="store_true")
    args = parser.parse_args()

    if args.list_ports:
        list_ports()
        sys.exit(0)

    pygame.init()
    print("DEBUG: pygame init done")
    screen   = pygame.display.set_mode((WIDTH, HEIGHT))
    print("DEBUG: window created")
    pygame.display.set_caption("Ship Navigation")
    clock     = pygame.time.Clock()
    print("DEBUG: fonts loading")
    font_sm   = pygame.font.SysFont("monospace", 12)
    font_lbl  = pygame.font.SysFont("monospace", 11, bold=True)
    font_med  = pygame.font.SysFont("monospace", 20, bold=True)
    font_title= pygame.font.SysFont("monospace", 40, bold=True)
    print("DEBUG: fonts done")
    music_idx = [0]
    init_music()

    ship          = Ship(OCEAN_W / 2, OCEAN_H / 2)
    reader        = None
    ports         = get_ports()
    selected_idx  = 0
    dropdown_open = False
    baud          = args.baud
    buttons       = {}
    ctrl_oy       = OCEAN_H
    raw_oy        = OCEAN_H + CTRL_H
    tick          = 0

    if args.port:
        reader = SerialReader(args.port, baud)
        try:
            selected_idx = ports.index(args.port)
        except ValueError:
            pass

    # ── Connection screen — select port and connect ──────────────────────────
    while True:  # restart loop
        conn_btns          = {}
        conn_dropdown_open = False
        connecting         = True
        tick               = 0

        while connecting:
            tick += 1

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    pygame.quit(); sys.exit()
                if event.type == MUSIC_NOTE_EVENT:
                    handle_music_event(music_idx)
                if event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        pygame.quit(); sys.exit()
                    if event.key == pygame.K_SPACE:
                        connecting = False

                if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    mx, my = event.pos

                    if conn_dropdown_open:
                        item_rects = [pygame.Rect(WIDTH//2 - 200, HEIGHT//2 - 20 + (i+1)*34, 400, 28)
                                      for i in range(len(ports))]
                        clicked = False
                        for i, rect in enumerate(item_rects):
                            if rect.collidepoint(mx, my):
                                selected_idx = i
                                conn_dropdown_open = False
                                clicked = True
                                break
                        if not clicked:
                            conn_dropdown_open = False

                    elif conn_btns.get("dropdown") and conn_btns["dropdown"].collidepoint(mx, my):
                        conn_dropdown_open = not conn_dropdown_open

                    elif conn_btns.get("refresh") and conn_btns["refresh"].collidepoint(mx, my):
                        ports = get_ports()
                        selected_idx = min(selected_idx, max(0, len(ports)-1))

                    elif conn_btns.get("connect") and conn_btns["connect"].collidepoint(mx, my):
                        if ports:
                            reader = SerialReader(ports[selected_idx], baud)
                            pygame.time.wait(600)
                            if reader.connected:
                                connecting = False

            # draw
            screen.fill((8, 16, 32))
            for y in range(0, HEIGHT, 40):
                offset = int(10 * math.sin(tick * 0.03 + y * 0.05))
                pygame.draw.line(screen, (15, 35, 60), (0, y + offset), (WIDTH, y + offset), 1)

            title = font_title.render("MAKING WAVES", True, (180, 220, 255))
            screen.blit(title, title.get_rect(center=(WIDTH//2, HEIGHT//5)))

            sub = font_med.render("select serial port to connect", True, (80, 120, 160))
            screen.blit(sub, sub.get_rect(center=(WIDTH//2, HEIGHT//5 + 50)))

            # port dropdown
            box_x, box_y = WIDTH//2 - 200, HEIGHT//2 - 20
            box_w, box_h = 400, 34
            pygame.draw.rect(screen, (20, 30, 50),  (box_x, box_y, box_w, box_h), border_radius=6)
            pygame.draw.rect(screen, (60, 100, 160), (box_x, box_y, box_w, box_h), 1, border_radius=6)
            port_text = ports[selected_idx] if ports else "no ports found"
            screen.blit(font_med.render(port_text, True, (180, 210, 240)), (box_x + 10, box_y + 6))
            screen.blit(font_med.render("v", True, (80, 120, 160)), (box_x + box_w - 28, box_y + 6))
            conn_btns["dropdown"] = pygame.Rect(box_x, box_y, box_w, box_h)

            ref_x = box_x + box_w + 12
            pygame.draw.rect(screen, (20, 35, 55), (ref_x, box_y, 90, box_h), border_radius=6)
            pygame.draw.rect(screen, (40, 70, 100), (ref_x, box_y, 90, box_h), 1, border_radius=6)
            screen.blit(font_med.render("Refresh", True, (100, 150, 190)), (ref_x + 6, box_y + 6))
            conn_btns["refresh"] = pygame.Rect(ref_x, box_y, 90, box_h)

            btn_y = box_y + box_h + 20
            pygame.draw.rect(screen, (20, 60, 100), (WIDTH//2 - 100, btn_y, 200, 40), border_radius=8)
            pygame.draw.rect(screen, (60, 140, 220), (WIDTH//2 - 100, btn_y, 200, 40), 2, border_radius=8)
            screen.blit(font_med.render("Connect", True, (140, 200, 255)),
                        font_med.render("Connect", True, (0,0,0)).get_rect(center=(WIDTH//2, btn_y + 20)))
            conn_btns["connect"] = pygame.Rect(WIDTH//2 - 100, btn_y, 200, 40)

            if reader:
                status_color = (80, 200, 120) if reader.connected else (255, 120, 80)
                status_text  = "connected!" if reader.connected else (reader.error or "connecting...")
                screen.blit(font_med.render(status_text, True, status_color),
                            font_med.render(status_text, True, (0,0,0)).get_rect(center=(WIDTH//2, btn_y + 60)))

            if conn_dropdown_open and ports:
                for i, port in enumerate(ports):
                    ir = pygame.Rect(box_x, box_y + (i+1)*34, box_w, 28)
                    bg = (30, 60, 100) if i == selected_idx else (18, 28, 48)
                    pygame.draw.rect(screen, bg, ir, border_radius=4)
                    pygame.draw.rect(screen, (50, 90, 140), ir, 1, border_radius=4)
                    screen.blit(font_med.render(port, True, (180, 210, 240)), (ir.x + 10, ir.y + 4))

            hint = font_sm.render("SPACE to skip (demo)   ESC to quit", True, (40, 60, 80))
            screen.blit(hint, hint.get_rect(center=(WIDTH//2, HEIGHT - 30)))

            pygame.display.flip()
            clock.tick(FPS)

    # ── Startup screen — wait for 3 RFID cards ──────────────────────────────
        REQUIRED = {"yellow", "green", "purple"}
        startup  = True

        while startup:
            tick += 1
            scanned = reader.get_scanned() if reader else set()

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    pygame.quit(); sys.exit()
                if event.type == MUSIC_NOTE_EVENT:
                    handle_music_event(music_idx)
                if event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        pygame.quit(); sys.exit()
                    if event.key == pygame.K_SPACE:
                        startup = False

            if scanned >= REQUIRED:
                draw_startup_screen(screen, scanned, tick, font_title, font_med, font_sm)
                pygame.display.flip()
                pygame.time.wait(1200)
                startup = False

            draw_startup_screen(screen, scanned, tick, font_title, font_med, font_sm)
            pygame.display.flip()
            clock.tick(FPS)

        # ── Main game loop ───────────────────────────────────────────────────────
        running  = True
        go_home  = False

        while running:
            tick += 1

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False

                if event.type == pygame.KEYDOWN:
                    if event.key == pygame.K_ESCAPE:
                        running = False

                if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    mx, my = event.pos

                    if dropdown_open:
                        # check item clicks first
                        item_rects = [pygame.Rect(60, ctrl_oy + 8 + (i+1)*30, 340, 26)
                                      for i in range(len(ports))]
                        clicked_item = False
                        for i, rect in enumerate(item_rects):
                            if rect.collidepoint(mx, my):
                                selected_idx  = i
                                dropdown_open = False
                                clicked_item  = True
                                break
                        if not clicked_item:
                            dropdown_open = False

                    else:
                        if buttons.get("dropdown") and buttons["dropdown"].collidepoint(mx, my):
                            dropdown_open = True

                        elif buttons.get("connect") and buttons["connect"].collidepoint(mx, my):
                            if reader and reader.connected:
                                reader = None
                            elif ports:
                                reader = SerialReader(ports[selected_idx], baud)

                        elif buttons.get("refresh") and buttons["refresh"].collidepoint(mx, my):
                            ports        = get_ports()
                            selected_idx = min(selected_idx, max(0, len(ports)-1))

                        elif buttons.get("zero") and buttons["zero"].collidepoint(mx, my):
                            if reader and reader.connected:
                                reader.send_raw(b'{"Command":"zero"}\r\n')
                                print("Sent zero command to IMU")

                        elif buttons.get("home") and buttons["home"].collidepoint(mx, my):
                            running  = False
                            go_home  = True

            # steering
            connected = reader is not None and reader.connected
            if not connected:
                keys = pygame.key.get_pressed()
                demo_angle = -30.0 if keys[pygame.K_LEFT] else 30.0 if keys[pygame.K_RIGHT] else 0.0
                delta_deg  = demo_angle * 0.1
                cum_angle  = 0.0
                gyro       = 0.0
                raw_lines  = [("demo mode — connect UART to receive data", "other")]
            else:
                delta_deg = reader.delta_angle
                cum_angle = reader.steering_angle
                gyro      = reader.gyro_z
                raw_lines = reader.get_raw_lines()

            ship.update(delta_deg)
            check_obstacle_collision(ship, OBSTACLES)

            # ── draw ──
            draw_water(screen)
            draw_trail(screen, ship.trail)
            draw_obstacles(screen, OBSTACLES, tick, font_sm)
            draw_ship(screen, ship.x, ship.y, ship.heading)
            draw_compass(screen, OCEAN_W - 50, 50, ship.heading, font_sm)
            draw_hud(screen, ship.heading, cum_angle, delta_deg, gyro, connected, font_sm)

            hint = font_sm.render("ESC = quit" + ("   ← → steer" if not connected else ""),
                                   True, (50, 70, 90))
            screen.blit(hint, (OCEAN_W//2 - hint.get_width()//2, OCEAN_H - 20))

            draw_ctrl_bar(screen, ctrl_oy, ports, selected_idx,
                          font_sm, font_lbl, connected, buttons)

            if dropdown_open and ports:
                draw_dropdown(screen, ports, selected_idx, ctrl_oy, font_sm)

            draw_raw_panel(screen, raw_lines, font_sm, font_lbl, raw_oy)

            pygame.display.flip()
            clock.tick(FPS)

        if not go_home:
            break  # ESC or window close — exit entirely
        reader = None
        ship   = Ship(OCEAN_W / 2, OCEAN_H / 2)

    pygame.quit()


if __name__ == "__main__":
    main()