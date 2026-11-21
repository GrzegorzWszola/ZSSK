import csv
from pathlib import Path
import tempfile
import time
import tkinter as tk
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

import gui


class ChartTests(unittest.TestCase):
    def test_svg_escapes_text_and_contains_values(self):
        painter = gui.SvgPainter()
        width, height = gui.draw_chart(painter, "A & B", [("<input>", 50, "#123456")])
        xml = painter.document(width, height)
        document = ET.fromstring(xml)
        self.assertIn("A &amp; B", xml)
        self.assertIn("&lt;input&gt;", xml)
        self.assertEqual(document.attrib["width"], "800")
        self.assertIn("50", "".join(document.itertext()))

    def test_rejects_malformed_benchmark(self):
        with self.assertRaises(ValueError):
            gui.parse_benchmark("bad,header\n1,2\n")
        with self.assertRaises(ValueError):
            gui.parse_benchmark(
                "algorytm,k,p,R,T_ms,S,poprawne\nhuff,10,1,nan,1,1,1\n"
            )

    def test_benchmark_chart_metric_and_failed_color(self):
        rows = gui.parse_benchmark(
            "algorytm,k,p,R,T_ms,S,poprawne\nhuff,10,1,0.5,12,1,0\n"
        )
        title, bars = gui.chart_data({"kind": "benchmark", "rows": rows}, "Time (ms)")
        self.assertEqual(title, "Time (ms)")
        self.assertEqual(bars, [("huff k=10 p=1", 12.0, "#b34c43")])


class ApplicationTests(unittest.TestCase):
    def setUp(self):
        exe = Path(gui.default_executable())
        if not exe.is_file():
            self.skipTest("Build the zssk executable before running the GUI integration tests.")
        try:
            self.root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest(f"Tk display unavailable: {exc}")
        self.root.withdraw()
        self.app = gui.Application(self.root)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(self.root.destroy)
        self.directory = Path(self.temp.name)
        self.source = self.directory / 'input,"quoted"\nfile.bin'
        self.data = bytes(range(256)) * 12 + b"A" * 2048
        self.source.write_bytes(self.data)
        self.app.values["executable"].set(str(exe))
        self.app.values["input"].set(str(self.source))

    def run_app(self):
        self.app.start()
        deadline = time.monotonic() + 30
        while self.app.running and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.running, "Operation timed out")
        self.assertEqual(self.app.status.cget("text"), "Finished", self.app.output_text.get("1.0", "end"))

    def test_compress_decompress_and_export_chart(self):
        archive = self.directory / "result.zssk"
        restored = self.directory / "restored.bin"
        chart = self.directory / "chart.svg"
        self.app.values["output"].set(str(archive))
        self.app.values["block"].set("10")
        self.app.values["threads"].set("2")
        self.run_app()
        self.assertEqual(self.app.result["input_size"], len(self.data))
        self.assertEqual(self.app.result["output_size"], archive.stat().st_size)
        with mock.patch.object(gui.filedialog, "asksaveasfilename", return_value=str(chart)):
            self.app.save_chart()
        self.assertEqual(ET.parse(chart).getroot().tag, "{http://www.w3.org/2000/svg}svg")
        with mock.patch.object(gui.filedialog, "asksaveasfilename", return_value=str(archive)), \
                mock.patch.object(gui.messagebox, "showerror") as error:
            self.app.save_chart()
            error.assert_called_once()
        self.app.values["operation"].set("decompress")
        self.app.update_operation()
        self.app.values["input"].set(str(archive))
        self.app.values["output"].set(str(restored))
        self.run_app()
        self.assertEqual(restored.read_bytes(), self.data)
        self.assertEqual(self.app.result["output_size"], len(self.data))

    def test_benchmark_csv_and_chart_with_special_filename(self):
        csv_path = self.directory / "benchmark.csv"
        self.app.values["operation"].set("bench")
        self.app.update_operation()
        for key, value in {
            "algorithm": "both", "threads": "1,2", "bmin": "10", "bmax": "10",
            "reps": "1", "csv": str(csv_path),
        }.items():
            self.app.values[key].set(value)
        self.run_app()
        self.assertEqual(len(self.app.result["rows"]), 4)
        with csv_path.open(encoding="utf-8", newline="") as file:
            rows = list(csv.DictReader(file))
        self.assertEqual(len(rows), 4)
        self.assertTrue(all(row["plik"] == str(self.source) for row in rows))
        for metric in gui.METRICS:
            self.app.values["metric"].set(metric)
            self.app.redraw()
            svg = gui.render_svg(self.app.result, metric)
            self.assertIn(metric.replace("&", "&amp;"), svg)
            ET.fromstring(svg)

    def test_validation_prevents_output_overwriting_input(self):
        self.app.values["output"].set(str(self.source))
        with self.assertRaisesRegex(ValueError, "different"):
            self.app.build_request()
        self.app.values["output"].set(str(self.directory / "out.zssk"))
        self.app.values["threads"].set("0")
        with self.assertRaisesRegex(ValueError, "Threads"):
            self.app.build_request()
        self.app.values["operation"].set("bench")
        self.app.values["threads"].set("1,2")
        self.app.values["bmin"].set("27")
        with self.assertRaisesRegex(ValueError, "block exponent"):
            self.app.build_request()

    def test_failure_is_reported(self):
        self.app.values["operation"].set("decompress")
        self.app.update_operation()
        self.app.values["output"].set(str(self.directory / "out.bin"))
        with mock.patch.object(gui.messagebox, "showerror") as error:
            self.app.start()
            deadline = time.monotonic() + 30
            while self.app.running and time.monotonic() < deadline:
                self.root.update()
                time.sleep(0.01)
            self.assertFalse(self.app.running)
            self.assertEqual(self.app.status.cget("text"), "Failed")
            error.assert_called_once()
            self.assertIsNone(self.app.result)


if __name__ == "__main__":
    unittest.main()
