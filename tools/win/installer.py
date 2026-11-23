"""FightingNX desktop installer. The build runs in a separate process."""

import os
from pathlib import Path
from queue import Empty, Queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from installer_build import ROOT, OUT, SD_ROOT, SHADERS, validate_game


class Installer(ttk.Frame):
    def __init__(self, window):
        super().__init__(window, padding=26)
        self.window = window
        self.events = Queue()
        self.busy = False
        self.fields = []
        self.status = tk.StringVar(value="Select your extracted game folder.")
        self.game = tk.StringVar()
        devkit = os.environ.get("DEVKITPRO", "")
        self.devkit = tk.StringVar(value=devkit if Path(devkit).is_absolute() and Path(devkit).exists()
                                   else r"C:\devkitPro")

        self.pack(fill="both", expand=True)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(7, weight=1)
        ttk.Label(self, text="FightingNX", style="Title.TLabel").grid(row=0, sticky="w")
        ttk.Label(self, text="Build for Nintendo Switch from your own Xbox 360 disc dump.",
                  style="Subtitle.TLabel").grid(row=1, sticky="w", pady=(2, 22))

        self.add_folder(2, "Extracted game folder", self.game)
        self.add_folder(3, "devkitPro installation", self.devkit)
        self.game.trace_add("write", lambda *_: self.refresh_buttons())
        self.devkit.trace_add("write", lambda *_: self.refresh_buttons())

        actions = ttk.Frame(self)
        actions.grid(row=4, sticky="ew", pady=(11, 0))
        self.capture_button = ttk.Button(actions, text="Capture shaders",
                                         command=lambda: self.start("capture"))
        self.capture_button.pack(side="left")
        self.build_button = ttk.Button(actions, text="Build Switch folder", style="Primary.TButton",
                                       command=lambda: self.start("build"))
        self.build_button.pack(side="left", padx=8)
        self.open_button = ttk.Button(actions, text="Open SD card folder",
                                      command=lambda: os.startfile(SD_ROOT))
        self.open_button.pack(side="right")

        ttk.Label(self, text="Capture on Windows first. Open the menus and modes you plan to play.",
                  style="Subtitle.TLabel", wraplength=610).grid(row=5, sticky="w", pady=(9, 16))
        status = ttk.Frame(self)
        status.grid(row=6, sticky="ew", pady=(0, 7))
        ttk.Label(status, textvariable=self.status, wraplength=580).pack(side="left", fill="x")
        self.progress = ttk.Progressbar(status, mode="indeterminate", length=85)
        self.progress.pack(side="right")

        log_frame = ttk.Frame(self)
        log_frame.grid(row=7, sticky="nsew")
        self.log = tk.Text(log_frame, height=12, wrap="word", font=("Consolas", 10),
                           bg="#172131", fg="#e4ebf6", borderwidth=0, padx=12, pady=10,
                           state="disabled", highlightthickness=0)
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.log.pack(side="left", fill="both", expand=True)
        self.refresh_buttons()
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        self.after(80, self.read_events)

    def add_folder(self, row, label, value):
        field = ttk.Frame(self)
        field.grid(row=row, sticky="ew", pady=(0, 12))
        field.columnconfigure(0, weight=1)
        ttk.Label(field, text=label).grid(row=0, column=0, sticky="w", pady=(0, 4))
        entry = ttk.Entry(field, textvariable=value)
        entry.grid(row=1, column=0, sticky="ew")

        def browse():
            path = filedialog.askdirectory(parent=self.window, title=label)
            if path:
                value.set(path)

        button = ttk.Button(field, text="Browse…", command=browse)
        button.grid(row=1, column=1, padx=(8, 0))
        self.fields.extend((entry, button))

    def refresh_buttons(self):
        ready = not self.busy and bool(self.game.get().strip()) and bool(self.devkit.get().strip())
        for field in self.fields:
            field.configure(state="disabled" if self.busy else "normal")
        shaders = any(SHADERS.glob("vs_*.bin")) and any(SHADERS.glob("ps_*.bin"))
        self.capture_button.configure(state="normal" if ready else "disabled")
        self.build_button.configure(state="normal" if ready and shaders else "disabled")
        staged = (SD_ROOT / "switch/fnc-nx/fnc-nx.nro").is_file()
        self.open_button.configure(state="normal" if staged and not self.busy else "disabled")

    def append(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line)
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self, action):
        game, devkit = Path(self.game.get().strip()), Path(self.devkit.get().strip())
        try:
            validate_game(game)
        except (OSError, ValueError) as error:
            messagebox.showerror("Check the game folder", str(error), parent=self.window)
            return
        self.busy = True
        self.status.set("Starting the local build…")
        self.refresh_buttons()
        self.progress.start(12)
        threading.Thread(target=self.run, args=(action, game, devkit), daemon=True).start()

    def run(self, action, game, devkit):
        result = 1
        try:
            OUT.mkdir(parents=True, exist_ok=True)
            python = Path(sys.executable)
            if python.name.lower() == "pythonw.exe":
                python = python.with_name("python.exe")
            command = [str(python), "-u", str(Path(__file__).with_name("installer_build.py")), action,
                       "--game", str(game.resolve()), "--devkitpro", str(devkit.resolve())]
            with (OUT / "installer.log").open("w", encoding="utf-8") as log:
                with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, errors="replace", creationflags=subprocess.CREATE_NO_WINDOW) as process:
                    for line in process.stdout:
                        log.write(line)
                        self.events.put(("stage", line.removeprefix("::stage::").strip())
                                        if line.startswith("::stage::") else ("log", line))
                    result = process.wait()
        except OSError as error:
            self.events.put(("log", f"{error}\n"))
        self.events.put(("done", result))

    def read_events(self):
        try:
            for _ in range(200):
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.append(value)
                elif kind == "stage":
                    self.status.set(value)
                else:
                    self.busy = False
                    self.progress.stop()
                    if value:
                        self.status.set("Build stopped. See the log for details.")
                    self.refresh_buttons()
        except Empty:
            pass
        self.after(80, self.read_events)

    def close(self):
        if self.busy:
            messagebox.showinfo("Build in progress", "Close the game window after capture and let the build finish.",
                                parent=self.window)
        else:
            self.window.destroy()


def main():
    window = tk.Tk()
    window.title("FightingNX Installer")
    window.minsize(660, 535)
    window.geometry("740x635")
    style = ttk.Style(window)
    style.theme_use("clam")
    style.configure("TFrame", background="#f3f6fb")
    style.configure("TLabel", background="#f3f6fb", foreground="#172131", font=("Segoe UI", 10))
    style.configure("Subtitle.TLabel", foreground="#48576c")
    style.configure("Title.TLabel", font=("Segoe UI Semibold", 27))
    style.configure("TButton", font=("Segoe UI", 10), padding=(12, 7))
    style.configure("Primary.TButton", background="#234986", foreground="white")
    style.map("Primary.TButton", background=[("disabled", "#dce4f0"), ("active", "#3268b4")],
              foreground=[("disabled", "#65748a")])
    Installer(window)
    window.mainloop()


if __name__ == "__main__":
    main()
