#!/usr/bin/env python3
"""TreeMiner matrix view — standalone terminal dashboard.

Renders a digital-rain backdrop with live mining gauges fed from the running
miner's read-only /stats API (default http://127.0.0.1:42069). The miner itself
keeps running under systemd untouched — this is a viewer, not a controller.

Usage:  python3 tmatrix.py [host:port]     (q to quit)
"""
import curses
import json
import random
import subprocess
import sys
import time
import urllib.request

API = "http://%s/stats" % (sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:42069")
RAIN_CHARS = "01アイウエオカキクケコサシスセソXENBLOCKS$#@*+=<>"
FETCH_EVERY = 2.0
FRAME = 0.08


def fetch_stats():
    try:
        with urllib.request.urlopen(API, timeout=3) as r:
            return json.load(r), None
    except Exception as e:  # noqa: BLE001 - any failure renders as "unreachable"
        return None, str(e)


def gpu_temps():
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=temperature.gpu,power.draw",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=2).stdout
        return [tuple(x.strip() for x in line.split(","))
                for line in out.strip().splitlines()]
    except Exception:  # noqa: BLE001
        return []


def fmt_uptime(seconds):
    d, rem = divmod(int(seconds), 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    return (f"{d}d {h:02}:{m:02}:{s:02}" if d else f"{h:02}:{m:02}:{s:02}")


class Rain:
    def __init__(self, h, w):
        self.resize(h, w)

    def resize(self, h, w):
        self.h, self.w = h, w
        self.drops = [random.randint(-h, 0) for _ in range(max(1, w))]

    def step(self, scr):
        for x in range(0, self.w - 1, 2):
            y = self.drops[x]
            if 0 <= y < self.h - 1:
                try:
                    scr.addstr(y, x, random.choice(RAIN_CHARS),
                               curses.color_pair(1) | curses.A_DIM)
                except curses.error:
                    pass
            self.drops[x] = y + 1 if y < self.h else random.randint(-self.h, 0)


def box(scr, y, x, h, w, title, color):
    try:
        for i in range(1, w - 1):
            scr.addstr(y, x + i, "─", color)
            scr.addstr(y + h - 1, x + i, "─", color)
        for i in range(1, h - 1):
            scr.addstr(y + i, x, "│", color)
            scr.addstr(y + i, x + w - 1, "│", color)
            scr.addstr(y + i, x + 1, " " * (w - 2))
        scr.addstr(y, x, "┌", color)
        scr.addstr(y, x + w - 1, "┐", color)
        scr.addstr(y + h - 1, x, "└", color)
        try:
            scr.addstr(y + h - 1, x + w - 1, "┘", color)
        except curses.error:
            pass  # bottom-right corner write always errors on last cell
        scr.addstr(y, x + 2, f" {title} ", color | curses.A_BOLD)
    except curses.error:
        pass


def bar(scr, y, x, width, frac, color):
    fill = max(0, min(width, int(width * frac)))
    try:
        scr.addstr(y, x, "█" * fill, color)
        scr.addstr(y, x + fill, "░" * (width - fill), color | curses.A_DIM)
    except curses.error:
        pass


def main(scr):
    curses.curs_set(0)
    curses.use_default_colors()
    curses.start_color()
    curses.init_pair(1, curses.COLOR_GREEN, -1)   # rain / ok
    curses.init_pair(2, curses.COLOR_CYAN, -1)    # labels
    curses.init_pair(3, curses.COLOR_YELLOW, -1)  # xuni / warn
    curses.init_pair(4, curses.COLOR_RED, -1)     # down / super
    curses.init_pair(5, curses.COLOR_WHITE, -1)   # values
    scr.nodelay(True)
    h, w = scr.getmaxyx()
    rain = Rain(h, w)
    stats, err, last_fetch, temps = None, None, 0.0, []

    G, C, Y, R, W = (curses.color_pair(i) for i in range(1, 6))

    while True:
        if scr.getch() in (ord("q"), ord("Q")):
            return
        nh, nw = scr.getmaxyx()
        if (nh, nw) != (h, w):
            h, w = nh, nw
            rain.resize(h, w)
            scr.erase()
        now = time.time()
        if now - last_fetch >= FETCH_EVERY:
            stats, err = fetch_stats()
            temps = gpu_temps()
            last_fetch = now

        scr.erase()
        rain.step(scr)

        if w < 60 or h < 18:
            try:
                scr.addstr(0, 0, "terminal too small", R)
            except curses.error:
                pass
            scr.refresh()
            time.sleep(FRAME)
            continue

        cx = max(2, (w - 76) // 2)
        title = "T R E E M I N E R"
        try:
            scr.addstr(1, max(0, (w - len(title)) // 2), title, G | curses.A_BOLD)
        except curses.error:
            pass

        if stats is None:
            box(scr, 3, cx, 5, 72, "LINK", R)
            scr.addstr(5, cx + 3, f"miner API unreachable: {err or '?'}"[: w - cx - 4], R)
            scr.refresh()
            time.sleep(FRAME)
            continue

        khs = stats.get("totalHashrate", 0) / 1000.0
        diff = stats.get("difficulty", 0)
        eff = stats.get("difficultyStats", {}).get("effective_mining_difficulty", diff)
        j = stats.get("journal", {})
        sub = stats.get("submissions", {})
        pool_up = stats.get("serverState", "?") == "up"
        outage_s = stats.get("pool", {}).get("outage_duration_ms", 0) // 1000

        # ── HASHPOWER ────────────────────────────────────────────────
        box(scr, 3, cx, 7, 72, "HASHPOWER", G)
        scr.addstr(4, cx + 3, f"{khs:8.1f} kH/s", W | curses.A_BOLD)
        scr.addstr(4, cx + 20, f"difficulty {diff}", C)
        if eff != diff:
            scr.addstr(4, cx + 38, f"(mining at {eff})", Y)
        scr.addstr(4, cx + 56, fmt_uptime(stats.get("uptime", 0)), C)
        for i, g in enumerate(stats.get("gpus", [])[:2]):
            gk = g.get("hashrate", 0) / 1000.0
            t = temps[i] if i < len(temps) else ("?", "?")
            scr.addstr(6 + i, cx + 3, f"GPU{g.get('index', i)}", C)
            bar(scr, 6 + i, cx + 9, 34, gk / max(khs, 0.001), G)
            scr.addstr(6 + i, cx + 45, f"{gk:7.1f} kH/s  {t[0]}°C {t[1]}W", W)

        # ── LEDGER ───────────────────────────────────────────────────
        box(scr, 10, cx, 6, 35, "LEDGER (journal)", G)
        scr.addstr(11, cx + 3, "confirmed", C)
        scr.addstr(11, cx + 20, f"{j.get('acked_total', 0):>8}", G | curses.A_BOLD)
        scr.addstr(12, cx + 3, "pending", C)
        scr.addstr(12, cx + 20, f"{j.get('pending', 0):>8}", Y)
        scr.addstr(13, cx + 3, "parked", C)
        scr.addstr(13, cx + 20, f"{j.get('parked_total', 0):>8}", Y)
        scr.addstr(14, cx + 3, "lost", C)
        scr.addstr(14, cx + 20, f"{j.get('dead_total', 0):>8}",
                   R if j.get("dead_total") else G)

        # ── UPLINK ───────────────────────────────────────────────────
        box(scr, 10, cx + 37, 6, 35, "UPLINK (pool)", G if pool_up else R)
        if pool_up:
            scr.addstr(11, cx + 40, "● ONLINE", G | curses.A_BOLD)
        else:
            scr.addstr(11, cx + 40, f"● DOWN {outage_s // 60}m{outage_s % 60:02}s",
                       R | curses.A_BOLD | curses.A_BLINK)
        scr.addstr(12, cx + 40, "session blocks", C)
        scr.addstr(12, cx + 58, f"{stats.get('acceptedBlocks', 0):>8}", G | curses.A_BOLD)
        scr.addstr(13, cx + 40, "confirmed", C)
        scr.addstr(13, cx + 58, f"{sub.get('acked_total', 0):>8}", W)
        scr.addstr(14, cx + 40, "rejected-bad", C)
        scr.addstr(14, cx + 58, f"{sub.get('permanently_invalid_total', 0):>8}",
                   R if sub.get("permanently_invalid_total") else G)

        footer = " q quit   dashboard http://%s:42069 " % (
            (stats.get("console", {}).get("urls") or ["127.0.0.1"])[0])
        try:
            scr.addstr(h - 1, max(0, (w - len(footer)) // 2), footer, C | curses.A_DIM)
        except curses.error:
            pass
        if stats.get("fatalDurabilityFailure"):
            scr.addstr(2, cx, "!! DURABILITY FAILURE — MINER HALTING !!", R | curses.A_BOLD)

        scr.refresh()
        time.sleep(FRAME)


if __name__ == "__main__":
    curses.wrapper(main)
