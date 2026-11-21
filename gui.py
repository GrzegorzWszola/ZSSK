#!/usr/bin/env python3
"""Desktop interface for the ZSSK command-line program."""

import csv
import html
import io
import math
import os
from pathlib import Path
import queue
import shlex
import subprocess
import tempfile
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


ROOT = Path(__file__).resolve().parent
METRICS = {
    "Time (ms)": "T_ms",
    "Compression ratio": "R",
    "Speedup (requires 1 thread)": "S",
}


def default_executable():
    name = "zssk.exe" if os.name == "nt" else "zssk"
    for path in (ROOT / "build" / name, ROOT / "cmake-build-debug" / name,
                 ROOT / "cmake-build-release" / name, ROOT / name):
        if path.is_file():
            return str(path)
    return str(ROOT / "build" / name)


def positive_int(value, label, maximum=1000000):
    try:
        number = int(value)
    except ValueError as exc:
        raise ValueError(f"{label} must be an integer.") from exc
    if not 1 <= number <= maximum:
        raise ValueError(f"{label} must be between 1 and {maximum}.")
    return number


def number(value, label, minimum=0, maximum=None):
    try:
        result = float(value)
    except ValueError as exc:
        raise ValueError(f"{label} must be a number.") from exc
    if not math.isfinite(result) or result < minimum or (maximum is not None and result > maximum):
        raise ValueError(f"{label} is outside the allowed range.")
    return result


def same_file(path, other):
    return path == other or (path.exists() and other.exists() and os.path.samefile(path, other))


def parse_benchmark(text):
    rows = []
    reader = csv.DictReader(io.StringIO(text, newline=""))
    required = {"algorytm", "k", "p", "R", "T_ms", "S", "poprawne"}
    if reader.fieldnames is None or not required.issubset(reader.fieldnames):
        raise ValueError("The benchmark CSV is missing required columns.")
    for row in reader:
        try:
            parsed = {
                "algorithm": row["algorytm"],
                "block": int(row["k"]),
                "threads": int(row["p"]),
                "ok": row["poprawne"] == "1",
            }
            for key in ("R", "T_ms", "S"):
                parsed[key] = float(row[key])
                if not math.isfinite(parsed[key]) or parsed[key] < 0:
                    raise ValueError(f"Invalid {key} value")
            rows.append(parsed)
        except (ValueError, TypeError, KeyError) as exc:
            raise ValueError(f"Invalid benchmark CSV row: {row}") from exc
    return rows


def chart_data(result, metric):
    if result["kind"] == "benchmark":
        key = METRICS[metric]
        bars = [
            (f'{row["algorithm"]} k={row["block"]} p={row["threads"]}',
             row[key], "#b34c43" if not row["ok"] else
             ("#3478b8" if row["algorithm"] == "huff" else "#36a16b"))
            for row in result["rows"]
        ]
        return metric, bars
    return "File size (bytes)", [
        ("Input", result["input_size"], "#3478b8"),
        ("Output", result["output_size"], "#36a16b"),
    ]


def draw_chart(painter, title, bars):
    """Render the same chart to either a Tk canvas or an SVG document."""
    width = max(800, 100 + len(bars) * 125)
    height = 430
    left, top, bottom = 85, 55, 315
    right = width - 25
    painter.rect(0, 0, width, height, "#ffffff")
    painter.text(width / 2, 24, title, 17)
    maximum = max((value for _, value, _ in bars), default=0) or 1
    for step in range(6):
        y = bottom - (bottom - top) * step / 5
        painter.line(left, y, right, y, "#dddddd")
        painter.text(left - 8, y, f"{maximum * step / 5:.4g}", 11, "end")
    painter.line(left, top, left, bottom, "#333333")
    painter.line(left, bottom, right, bottom, "#333333")
    if bars:
        spacing = (right - left) / len(bars)
        bar_width = min(55, spacing * 0.65)
        for index, (label, value, color) in enumerate(bars):
            x = left + spacing * (index + 0.5)
            y = bottom - (bottom - top) * value / maximum
            painter.rect(x - bar_width / 2, y, x + bar_width / 2, bottom, color)
            painter.text(x, max(top - 10, y - 10), f"{value:.4g}", 10)
            painter.text(x, bottom + 24, label, 10)
    else:
        painter.text(width / 2, 180, "No results to display", 14)
    return width, height


class CanvasPainter:
    def __init__(self, canvas):
        self.canvas = canvas

    def rect(self, x1, y1, x2, y2, color):
        self.canvas.create_rectangle(x1, y1, x2, y2, fill=color, outline="")

    def line(self, x1, y1, x2, y2, color):
        self.canvas.create_line(x1, y1, x2, y2, fill=color)

    def text(self, x, y, text, size, anchor="middle"):
        anchors = {"middle": "center", "end": "e"}
        self.canvas.create_text(x, y, text=text, font=("TkDefaultFont", size),
                                anchor=anchors[anchor], fill="#222222")


class SvgPainter:
    def __init__(self):
        self.elements = []

    def rect(self, x1, y1, x2, y2, color):
        self.elements.append(
            f'<rect x="{x1:g}" y="{y1:g}" width="{x2-x1:g}" '
            f'height="{y2-y1:g}" fill="{color}"/>')

    def line(self, x1, y1, x2, y2, color):
        self.elements.append(
            f'<line x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}" stroke="{color}"/>')

    def text(self, x, y, text, size, anchor="middle"):
        self.elements.append(
            f'<text x="{x:g}" y="{y:g}" text-anchor="{anchor}" '
            f'dominant-baseline="middle" font-family="sans-serif" font-size="{size}" '
            f'fill="#222222">{html.escape(text)}</text>')

    def document(self, width, height):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
                f'viewBox="0 0 {width} {height}">\n' +
                "\n".join(self.elements) + "\n</svg>\n")


def render_svg(result, metric):
    painter = SvgPainter()
    title, bars = chart_data(result, metric)
    width, height = draw_chart(painter, title, bars)
    return painter.document(width, height)


class Application:
    def __init__(self, root):
        self.root = root
        root.title("ZSSK")
        root.geometry("1000x850")
        root.minsize(720, 600)
        self.events = queue.Queue()
        self.running = False
        self.result = None
        self.protected_paths = ()

        defaults = {
            "executable": default_executable(), "operation": "compress", "input": "",
            "output": "", "algorithm": "huff", "threads": "1", "block": "16",
            "bmin": "10", "bmax": "26", "reps": "3", "memlimit": "0",
            "r0": "0", "t0": "0", "weight": "0.5", "csv": "",
            "metric": "Time (ms)",
        }
        self.values = {key: tk.StringVar(value=value) for key, value in defaults.items()}
        self.make_widgets()
        self.update_operation()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.poll)

    def make_widgets(self):
        frame = ttk.Frame(self.root, padding=10)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)

        def path_row(row, label, key, command):
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", pady=2)
            ttk.Entry(frame, textvariable=self.values[key]).grid(row=row, column=1, sticky="ew", padx=5)
            button = ttk.Button(frame, text="Browse...", command=command)
            button.grid(row=row, column=2, sticky="ew")
            return button

        path_row(0, "ZSSK executable", "executable", self.choose_executable)
        ttk.Label(frame, text="Operation").grid(row=1, column=0, sticky="w")
        operation = ttk.Combobox(frame, textvariable=self.values["operation"], state="readonly",
                                 values=("compress", "decompress", "bench", "selftest"))
        operation.grid(row=1, column=1, sticky="w", padx=5, pady=2)
        operation.bind("<<ComboboxSelected>>", self.update_operation)
        self.input_button = path_row(2, "Input file", "input", self.choose_input)
        self.output_button = path_row(3, "Output file", "output", self.choose_output)
        self.csv_button = path_row(4, "Benchmark CSV (optional)", "csv", self.choose_csv)

        options = ttk.LabelFrame(frame, text="Options", padding=6)
        options.grid(row=5, column=0, columnspan=3, sticky="ew", pady=5)
        for col in (1, 3, 5):
            options.columnconfigure(col, weight=1)
        self.option_widgets = {}

        def option(row, column, label, key, choices=None):
            ttk.Label(options, text=label).grid(row=row, column=column, sticky="w", padx=4, pady=2)
            if choices:
                widget = ttk.Combobox(options, textvariable=self.values[key], values=choices,
                                      state="readonly", width=12)
            else:
                widget = ttk.Entry(options, textvariable=self.values[key], width=12)
            widget.grid(row=row, column=column + 1, sticky="ew", padx=4, pady=2)
            self.option_widgets[key] = widget

        option(0, 0, "Algorithm", "algorithm", ("rle", "huff", "both"))
        option(0, 2, "Threads (bench: list)", "threads")
        option(0, 4, "Block exponent", "block")
        option(1, 0, "Min block exponent", "bmin")
        option(1, 2, "Max block exponent", "bmax")
        option(1, 4, "Repetitions", "reps")
        option(2, 0, "Memory limit (MB)", "memlimit")
        option(2, 2, "Ratio limit R0", "r0")
        option(2, 4, "Time limit T0 (ms)", "t0")
        option(3, 0, "Ratio weight (0-1)", "weight")

        actions = ttk.Frame(frame)
        actions.grid(row=6, column=0, columnspan=3, sticky="ew", pady=5)
        self.run_button = ttk.Button(actions, text="Run", command=self.start)
        self.run_button.pack(side="left")
        self.status = ttk.Label(actions, text="Ready")
        self.status.pack(side="left", padx=10)

        ttk.Label(frame, text="Program output").grid(row=7, column=0, sticky="w")
        self.output_text = tk.Text(frame, height=8, wrap="word", state="disabled")
        self.output_text.grid(row=8, column=0, columnspan=3, sticky="nsew")
        frame.rowconfigure(8, weight=1)

        chart_controls = ttk.Frame(frame)
        chart_controls.grid(row=9, column=0, columnspan=3, sticky="ew", pady=(8, 2))
        ttk.Label(chart_controls, text="Chart metric:").pack(side="left")
        metric = ttk.Combobox(chart_controls, textvariable=self.values["metric"],
                              values=list(METRICS), state="readonly", width=28)
        metric.pack(side="left", padx=5)
        metric.bind("<<ComboboxSelected>>", lambda event: self.redraw())
        self.save_button = ttk.Button(chart_controls, text="Save chart as SVG...", command=self.save_chart,
                                      state="disabled")
        self.save_button.pack(side="right")
        self.canvas = tk.Canvas(frame, height=300, background="white", highlightthickness=1)
        self.canvas.grid(row=10, column=0, columnspan=3, sticky="nsew")
        vertical_scrollbar = ttk.Scrollbar(frame, orient="vertical", command=self.canvas.yview)
        vertical_scrollbar.grid(row=10, column=3, sticky="ns")
        frame.rowconfigure(10, weight=2)
        scrollbar = ttk.Scrollbar(frame, orient="horizontal", command=self.canvas.xview)
        scrollbar.grid(row=11, column=0, columnspan=3, sticky="ew")
        self.canvas.configure(xscrollcommand=scrollbar.set, yscrollcommand=vertical_scrollbar.set)
        self.redraw()

    def update_operation(self, event=None):
        operation = self.values["operation"].get()
        enabled = {
            "algorithm": operation in ("compress", "bench"),
            "threads": operation != "selftest",
            "block": operation == "compress",
            **{key: operation == "bench" for key in ("bmin", "bmax", "reps", "memlimit", "r0", "t0", "weight")},
        }
        for key, widget in self.option_widgets.items():
            widget.configure(state=("readonly" if key == "algorithm" else "normal") if enabled[key] else "disabled")
        for key, button, active in (
            ("input", self.input_button, operation != "selftest"),
            ("output", self.output_button, operation in ("compress", "decompress")),
            ("csv", self.csv_button, operation == "bench"),
        ):
            button.configure(state="normal" if active else "disabled")
        if operation == "compress" and self.values["algorithm"].get() == "both":
            self.values["algorithm"].set("huff")

    def choose_executable(self):
        path = filedialog.askopenfilename(title="Select the ZSSK executable")
        if path:
            self.values["executable"].set(path)

    def choose_input(self):
        path = filedialog.askopenfilename(title="Select input file")
        if path:
            self.values["input"].set(path)

    def choose_output(self):
        path = filedialog.asksaveasfilename(title="Select output file")
        if path:
            self.values["output"].set(path)

    def choose_csv(self):
        path = filedialog.asksaveasfilename(title="Save benchmark CSV", defaultextension=".csv",
                                            filetypes=[("CSV", "*.csv"), ("All files", "*")])
        if path:
            self.values["csv"].set(path)

    def build_request(self):
        values = {key: value.get() for key, value in self.values.items()}
        for key in values.keys() - {"executable", "input", "output", "csv"}:
            values[key] = values[key].strip()
        exe = Path(values["executable"]).expanduser().resolve()
        if not exe.is_file():
            raise ValueError("Select an existing ZSSK executable.")
        operation = values["operation"]
        if operation not in ("compress", "decompress", "bench", "selftest"):
            raise ValueError("Select an operation.")
        args = [str(exe), operation]
        input_path = output_path = csv_path = None
        if operation != "selftest":
            if not values["input"]:
                raise ValueError("Select an input file.")
            input_path = Path(values["input"]).expanduser().resolve()
            if not input_path.is_file():
                raise ValueError("Input file does not exist.")
            args.append(str(input_path))
        if operation in ("compress", "decompress"):
            if not values["output"]:
                raise ValueError("Select an output file.")
            output_path = Path(values["output"]).expanduser().resolve()
            if any(same_file(output_path, path) for path in (input_path, exe)):
                raise ValueError("Output must be different from the input and executable.")
            args.append(str(output_path))
        if operation == "bench" and values["csv"]:
            csv_path = Path(values["csv"]).expanduser().resolve()
            if any(same_file(csv_path, path) for path in (input_path, exe)):
                raise ValueError("CSV output must be different from the input and executable.")
        if operation != "selftest":
            parts = values["threads"].split(",") if operation == "bench" else [values["threads"]]
            threads = [positive_int(part.strip(), "Threads", 1024) for part in parts]
            args += ["--threads", ",".join(map(str, threads))]
        if operation in ("compress", "bench"):
            algorithm = values["algorithm"]
            if algorithm not in (("rle", "huff") if operation == "compress" else ("rle", "huff", "both")):
                raise ValueError("Choose a valid algorithm for this operation.")
            args += ["--algo", algorithm]
        if operation == "compress":
            block = positive_int(values["block"], "Block exponent", 26)
            if block < 10:
                raise ValueError("Block exponent must be between 10 and 26.")
            args += ["--block", str(block)]
        if operation == "bench":
            bmin = positive_int(values["bmin"], "Minimum block exponent", 26)
            bmax = positive_int(values["bmax"], "Maximum block exponent", 26)
            if bmin < 10 or bmax < bmin:
                raise ValueError("Block exponents must satisfy 10 <= minimum <= maximum <= 26.")
            reps = positive_int(values["reps"], "Repetitions")
            memlimit = number(values["memlimit"], "Memory limit")
            r0 = number(values["r0"], "Ratio limit")
            t0 = number(values["t0"], "Time limit")
            weight = number(values["weight"], "Weight", maximum=1)
            args += ["--bmin", str(bmin), "--bmax", str(bmax), "--reps", str(reps),
                     "--memlimit", str(memlimit), "--R0", str(r0), "--T0", str(t0), "--w", str(weight)]
        return args, operation, input_path, output_path, csv_path

    def start(self):
        try:
            request = self.build_request()
            for path in (request[3], request[4]):
                if path and path.exists() and not messagebox.askyesno("Overwrite?", f"Overwrite {path}?"):
                    return
        except (ValueError, OSError) as exc:
            messagebox.showerror("Invalid input", str(exc))
            return
        self.result = None
        self.protected_paths = tuple(path for path in (Path(request[0][0]), *request[2:]) if path)
        self.save_button.configure(state="disabled")
        self.redraw()
        self.running = True
        self.run_button.configure(state="disabled")
        self.status.configure(text="Running...")
        self.set_output("$ " + shlex.join(request[0]) + "\n")
        threading.Thread(target=self.run_command, args=request, daemon=True).start()

    def run_command(self, args, operation, input_path, output_path, csv_path):
        output = ""
        try:
            with tempfile.TemporaryDirectory(prefix="zssk-gui-") as directory:
                temp_csv = Path(directory) / "benchmark.csv"
                if operation == "bench":
                    args = args + ["--csv", str(temp_csv)]
                input_size = input_path.stat().st_size if input_path else 0
                completed = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True, errors="replace", check=False)
                output = completed.stdout
                if completed.stderr:
                    output += "\n" + completed.stderr
                if completed.returncode:
                    self.events.put(("error", f"Program exited with code {completed.returncode}.\n{output}"))
                    return
                result = None
                if operation == "bench":
                    csv_text = temp_csv.read_text(encoding="utf-8")
                    rows = parse_benchmark(csv_text)
                    if csv_path:
                        with csv_path.open("w", encoding="utf-8", newline="") as destination:
                            destination.write(csv_text)
                    if rows:
                        result = {"kind": "benchmark", "rows": rows}
                        if any(not row["ok"] for row in rows):
                            output += "\nWarning: red bars indicate failed verification.\n"
                    else:
                        output += "\nNo benchmark results to chart.\n"
                elif operation in ("compress", "decompress"):
                    result = {"kind": "sizes", "input_size": input_size,
                              "output_size": output_path.stat().st_size}
                self.events.put(("success", output, result))
        except (OSError, ValueError, csv.Error) as exc:
            self.events.put(("error", f"{output}\n{exc}" if output else str(exc)))

    def poll(self):
        try:
            while True:
                event = self.events.get_nowait()
                self.running = False
                self.run_button.configure(state="normal")
                if event[0] == "error":
                    self.status.configure(text="Failed")
                    self.append_output("\n" + event[1])
                    messagebox.showerror("Execution failed", event[1])
                else:
                    self.status.configure(text="Finished")
                    self.append_output("\n" + event[1])
                    self.result = event[2]
                    self.save_button.configure(state="normal" if self.result else "disabled")
                    self.redraw()
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def set_output(self, text):
        self.output_text.configure(state="normal")
        self.output_text.delete("1.0", "end")
        self.output_text.insert("end", text)
        self.output_text.configure(state="disabled")

    def append_output(self, text):
        self.output_text.configure(state="normal")
        self.output_text.insert("end", text)
        self.output_text.see("end")
        self.output_text.configure(state="disabled")

    def redraw(self):
        self.canvas.delete("all")
        if self.result:
            title, bars = chart_data(self.result, self.values["metric"].get())
        else:
            title, bars = "Results", []
        width, height = draw_chart(CanvasPainter(self.canvas), title, bars)
        self.canvas.configure(scrollregion=(0, 0, width, height))

    def save_chart(self):
        if not self.result:
            return
        path = filedialog.asksaveasfilename(title="Save chart", defaultextension=".svg",
                                            filetypes=[("SVG image", "*.svg")])
        if path:
            try:
                target = Path(path).expanduser().resolve()
                if any(same_file(target, protected) for protected in self.protected_paths):
                    raise ValueError("Chart must be saved separately from the executable and data files.")
                target.write_text(render_svg(self.result, self.values["metric"].get()), encoding="utf-8")
            except (OSError, ValueError) as exc:
                messagebox.showerror("Could not save chart", str(exc))

    def close(self):
        if self.running:
            messagebox.showinfo("Program running", "Wait for the current operation to finish before closing.")
        else:
            self.root.destroy()


if __name__ == "__main__":
    Application(tk.Tk())
    tk.mainloop()
