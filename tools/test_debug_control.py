import configparser
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import debug_control


class DebugControlTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "DaysGoneCoop.asi").touch()

    def test_allowlist_and_directory_validation(self):
        for command in ("God", "god-on extra", "spawn-pack;end", ""):
            with self.assertRaises(ValueError):
                debug_control.send(self.directory, command)
        with self.assertRaises(ValueError):
            debug_control.send(self.directory / "missing", "begin")
        for timeout in (0, -1, 121, float("nan")):
            with self.assertRaises(ValueError):
                debug_control.send(self.directory, "begin", timeout)

    def test_acknowledgement_and_error(self):
        result = self.directory / "DaysGoneCoop-debug-result.ini"
        for state in ("ok", "error"):
            def respond(_):
                request = debug_control.read_ini(self.directory / "DaysGoneCoop-debug.ini")
                number = request.getint("Debug", "Request")
                result.write_text(f"[Result]\nState={state}\nMessage=measured\nRequest={number}\n")
            with patch.object(debug_control.time, "sleep", side_effect=respond):
                self.assertEqual(debug_control.send(self.directory, "status"), (state == "ok", "measured"))
        self.assertFalse((self.directory / "DaysGoneCoop-debug.lock").exists())

    def test_stale_ack_does_not_succeed_and_lock_is_cleaned(self):
        (self.directory / "DaysGoneCoop-debug-result.ini").write_text("[Result]\nRequest=1\nState=ok\n")
        with patch.object(debug_control.time, "monotonic", side_effect=(0, 0, 21)):
            with self.assertRaises(TimeoutError):
                debug_control.send(self.directory, "god-on")
        self.assertEqual(debug_control.read_ini(self.directory / "DaysGoneCoop-debug.ini").getint("Debug", "Request"), 2)
        self.assertFalse((self.directory / "DaysGoneCoop-debug.lock").exists())

    def test_bad_input_is_explicit_and_preserves_existing_request(self):
        mailbox = self.directory / "DaysGoneCoop-debug.ini"
        for text in ("[Debug]\nRequest=-1\n", "[Debug]\nRequest=invalid\n", "not an ini\n"):
            mailbox.write_text(text)
            with self.assertRaises((ValueError, configparser.Error)):
                debug_control.send(self.directory, "end")
            self.assertEqual(mailbox.read_text(), text)
            self.assertFalse((self.directory / "DaysGoneCoop-debug.lock").exists())

    def test_parallel_writer_is_refused(self):
        lock = self.directory / "DaysGoneCoop-debug.lock"
        lock.touch()
        with self.assertRaises(FileExistsError):
            debug_control.send(self.directory, "begin")
        self.assertTrue(lock.exists())

    def test_wrapping_identifier_cannot_accept_a_stale_ack(self):
        (self.directory / "DaysGoneCoop-debug.ini").write_text("[Debug]\nRequest=2147483647\n")
        (self.directory / "DaysGoneCoop-debug-result.ini").write_text("[Result]\nRequest=1\nState=ok\n")
        with patch.object(debug_control.time, "monotonic", side_effect=(0, 0, 21)):
            with self.assertRaises(TimeoutError):
                debug_control.send(self.directory, "status")
        self.assertEqual(debug_control.read_ini(self.directory / "DaysGoneCoop-debug.ini").getint("Debug", "Request"), 2)

    def test_invalid_ack_is_not_success(self):
        result = self.directory / "DaysGoneCoop-debug-result.ini"
        def respond(_):
            request = debug_control.read_ini(self.directory / "DaysGoneCoop-debug.ini").getint("Debug", "Request")
            result.write_text(f"[Result]\nState=unknown\nRequest={request}\n")
        with patch.object(debug_control.time, "sleep", side_effect=respond):
            with self.assertRaises(ValueError):
                debug_control.send(self.directory, "begin")
        self.assertFalse((self.directory / "DaysGoneCoop-debug.lock").exists())


if __name__ == "__main__":
    unittest.main()
