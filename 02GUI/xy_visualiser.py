import collections
import math
import threading
import time

import matplotlib
matplotlib.use("TkAgg")          # use a thread-safe backend
import matplotlib.pyplot as plt
import matplotlib.animation as animation


class LiveVisualiser:
    def __init__(
        self,
        x_range: tuple[float, float] = (0, 100),
        y_range: tuple[float, float] = (0, 100),
        trail_length: int = 200,
        update_ms: int = 50,
        grid_divisions: int = 10,
        title: str = "Live XY Visualiser",
    ):
        self.x_min, self.x_max = x_range
        self.y_min, self.y_max = y_range
        self.trail_length   = trail_length
        self.update_ms      = update_ms
        self.grid_divisions = grid_divisions
        self.window_title   = title

        self._x: float = (self.x_min + self.x_max) / 2
        self._y: float = (self.y_min + self.y_max) / 2
        self._lock   = threading.Lock()
        self._thread: threading.Thread | None = None
        self._ani:    animation.FuncAnimation | None = None
        self._running = False

    # -- Public API ------------------------------------------------------------

    def update(self, x: float, y: float) -> None:
        with self._lock:
            self._x, self._y = x, y

    def start(self) -> "LiveVisualiser":
        if self._running:
            return self
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._running = False
        try:
            plt.close("all")
        except Exception:
            pass

    def wait(self) -> None:
        if self._thread:
            self._thread.join()

    # -- Internal --------------------------------------------------------------
    def _get_xy(self) -> tuple[float, float]:
        with self._lock:
            return self._x, self._y

    def _run(self) -> None:
        fig, artists, trail_x, trail_y = self._build_figure()

        self._ani = animation.FuncAnimation(
            fig,
            self._animate,
            fargs=(artists, trail_x, trail_y),
            interval=self.update_ms,
            blit=True,
            cache_frame_data=False,
        )

        plt.tight_layout()
        plt.show()       # blocks until window is closed
        self._running = False

    def _build_figure(self):
        fig, ax = plt.subplots(figsize=(7, 7), facecolor="#0d0d0d")
        fig.canvas.manager.set_window_title(self.window_title)

        ax.set_facecolor("#111111")
        ax.set_xlim(self.x_min, self.x_max)
        ax.set_ylim(self.y_min, self.y_max)
        ax.set_aspect("equal")

        xs = [self.x_min + i * (self.x_max - self.x_min) / self.grid_divisions
              for i in range(self.grid_divisions + 1)]
        ys = [self.y_min + i * (self.y_max - self.y_min) / self.grid_divisions
              for i in range(self.grid_divisions + 1)]
        ax.set_xticks(xs)
        ax.set_yticks(ys)
        ax.grid(color="#2a2a2a", linewidth=0.8, linestyle="--")
        ax.tick_params(colors="#555555", labelsize=8)
        for spine in ax.spines.values():
            spine.set_edgecolor("#333333")

        ax.set_xlabel("X", color="#666666", fontsize=10)
        ax.set_ylabel("Y", color="#666666", fontsize=10)
        ax.set_title("LIVE POSITION", color="#00ffcc",
                     fontsize=11, fontfamily="monospace", pad=10)

        vline = ax.axvline(x=0, color="#00ffcc22", linewidth=1, linestyle=":")
        hline = ax.axhline(y=0, color="#00ffcc22", linewidth=1, linestyle=":")

        maxlen = max(self.trail_length, 1)
        trail_x: collections.deque = collections.deque(maxlen=maxlen)
        trail_y: collections.deque = collections.deque(maxlen=maxlen)
        trail_line, = ax.plot([], [], color="#00ffcc", linewidth=1.2,
                              alpha=0.35, zorder=2)

        dot_glow, = ax.plot([], [], "o", markersize=22, color="#00ffcc",
                            alpha=0.12, zorder=3)
        dot_mid,  = ax.plot([], [], "o", markersize=10, color="#00ffcc",
                            alpha=0.4,  zorder=4)
        dot_core, = ax.plot([], [], "o", markersize=5,  color="#ffffff",
                            alpha=1.0,  zorder=5)

        coord_text = ax.text(
            0.02, 0.97, "", transform=ax.transAxes,
            color="#00ffcc", fontsize=10, fontfamily="monospace",
            va="top", ha="left",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="#000000aa",
                      edgecolor="#00ffcc44", linewidth=0.8),
        )

        artists = (vline, hline, trail_line, dot_glow, dot_mid, dot_core, coord_text)
        return fig, artists, trail_x, trail_y

    def _animate(self, _, artists, trail_x, trail_y):
        vline, hline, trail_line, dot_glow, dot_mid, dot_core, coord_text = artists

        try:
            x, y = self._get_xy()
        except Exception as exc:
            coord_text.set_text(f"ERROR: {exc}")
            return artists

        xd = max(self.x_min, min(self.x_max, x))
        yd = max(self.y_min, min(self.y_max, y))

        trail_x.append(xd)
        trail_y.append(yd)

        vline.set_xdata([xd, xd])
        hline.set_ydata([yd, yd])

        if self.trail_length > 0:
            trail_line.set_data(list(trail_x), list(trail_y))

        for dot in (dot_glow, dot_mid, dot_core):
            dot.set_data([xd], [yd])

        oob = " ⚠ OOB" if (x != xd or y != yd) else ""
        coord_text.set_text(f"X: {x:8.2f}\nY: {y:8.2f}{oob}")

        return artists


# -- Standalone demo (python xy_visualiser.py) ---------------------------------
if __name__ == "__main__":
    vis = LiveVisualiser(x_range=(0, 100), y_range=(0, 100), trail_length=200)
    vis.start()

    t0 = time.time()
    while vis._running:
        t = time.time() - t0
        x = 50 + 40 * math.sin(2.0 * t)
        y = 50 + 40 * math.sin(3.0 * t + math.pi / 4)
        vis.update(x, y)
        time.sleep(0.02)
        