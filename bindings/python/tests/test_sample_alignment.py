from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SAMPLES_DIR = ROOT / "samples"
CANONICAL_SAMPLES = [
    "request_reply_callback_sample.py",
    "pair_recv_sample.py",
    "dealer_router_recv_sample.py",
    "pubsub_recv_sample.py",
    "stream_recv_sample.py",
    "stream_packet_callback_sample.py",
    "monitor_recv_sample.py",
]
ALLOWED_NON_SAMPLE_FILES = {
    "run_samples.py",
    "run_samples.sh",
    "sample_support.py",
}


class SampleAlignmentTests(unittest.TestCase):
    def test_required_sample_files_exist(self):
        expected = CANONICAL_SAMPLES + sorted(ALLOWED_NON_SAMPLE_FILES)
        for name in expected:
            self.assertTrue((SAMPLES_DIR / name).exists(), msg=name)

    def test_canonical_sample_set_matches_sample_files(self):
        sample_files = {
            path.name
            for path in SAMPLES_DIR.glob("*.py")
            if path.name not in ALLOWED_NON_SAMPLE_FILES
        }
        self.assertEqual(sample_files, set(CANONICAL_SAMPLES))

    def test_samples_avoid_inproc_and_sleep_handshake(self):
        for name in CANONICAL_SAMPLES:
            path = SAMPLES_DIR / name
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("inproc://", text, msg=path.name)
            self.assertNotIn("sleep(", text, msg=path.name)

    def test_official_runner_uses_exact_canonical_sample_set(self):
        runner = (SAMPLES_DIR / "run_samples.py").read_text(encoding="utf-8")
        for name in CANONICAL_SAMPLES:
            self.assertIn(name, runner)
        self.assertIn("CANONICAL_SAMPLES", runner)
        self.assertNotIn("pair_callback_sample.py", runner)
        self.assertNotIn("pubsub_callback_sample.py", runner)
        self.assertIn(
            'print(f"Summary: {passed}/{len(CANONICAL_SAMPLES)} samples passed")',
            runner,
        )

    def test_readme_mentions_samples_runner(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("samples/run_samples.sh", readme)

    def test_run_samples_scripts_enumerate_and_summarize_all_samples(self):
        runner = (SAMPLES_DIR / "run_samples.py").read_text(encoding="utf-8")
        shell = (SAMPLES_DIR / "run_samples.sh").read_text(encoding="utf-8")

        for name in CANONICAL_SAMPLES:
            self.assertIn(name, runner)
        self.assertIn("Summary:", runner)
        self.assertIn("samples/run_samples.py", shell)
