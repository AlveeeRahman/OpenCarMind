import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("ocm_report", ROOT / "tools" / "ocm_report.py")
ocm_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ocm_report)

HEADER = ["seq", "uptime_ms", "epoch_ms", "rpm", "speed_kph", "coolant_c", "throttle_pct"]
ML_HEADER = ["window_seq", "start_ms", "end_ms", "model", "manoeuvre", "p_stopped", "p_braking", "p_accelerating",
             "p_cruising", "est_brake_pressure_bar", "est_flywheel_torque_nm", "est_fuel_rate_lph", "latency_us", "queue_ms"]


def write_session(folder, name, rows, ml_rows=None):
    with open(folder / f"{name}.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(HEADER)
        writer.writerows(rows)
    if ml_rows:
        with open(folder / f"{name}.ml.csv", "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(ML_HEADER)
            writer.writerows(ml_rows)


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.sessions = base / "obd" / "sessions"
        self.sessions.mkdir(parents=True)
        self.out = base / "site"
        rows = [[i, 1000 + i * 1000, 1_800_000_000_000 + i * 1000, 1500 + i, 36 if i > 10 else 0, 85, 12] for i in range(60)]
        ml = [[i, 0, 1000 + i * 2000, "abc", "cruising" if i % 2 else "stopped", .1, .1, .1, .7, 5, 20, 2, 9000, 1]
              for i in range(10, 30)]
        write_session(self.sessions, "s000001", rows, ml)
        write_session(self.sessions, "s000002", rows[:30])

    def tearDown(self):
        self.tmp.cleanup()

    def test_builds_every_page_and_the_summary(self):
        infos = ocm_report.build(Path(self.tmp.name) / "obd", self.out)
        self.assertEqual([i["id"] for i in infos], ["s000001", "s000002"])
        for name in ("index.html", "latest.html", "s000001.html", "s000002.html", "summary.json"):
            self.assertTrue((self.out / name).is_file(), name)
        summary = json.loads((self.out / "summary.json").read_text())
        self.assertEqual(summary["sessions"], 2)
        self.assertEqual(summary["latest"]["id"], "s000002")

    def test_session_numbers(self):
        info = ocm_report.build(self.sessions, self.out)[0]
        self.assertEqual(info["rows"], 60)
        self.assertEqual(info["minutes"], 1.0)
        self.assertEqual(info["max_speed_kph"], 36)
        self.assertEqual(info["cruising_pct"] + info["stopped_pct"], 100)

    def test_reports_make_no_external_requests(self):
        ocm_report.build(self.sessions, self.out)
        for page in self.out.glob("*.html"):
            text = page.read_text()
            self.assertNotIn("http://", text.replace("http://www.w3.org", ""), page.name)
            self.assertNotIn("https://", text, page.name)

    def test_session_without_model_file_says_so(self):
        ocm_report.build(self.sessions, self.out)
        self.assertIn("No model results file", (self.out / "s000002.html").read_text())


if __name__ == "__main__":
    unittest.main()
