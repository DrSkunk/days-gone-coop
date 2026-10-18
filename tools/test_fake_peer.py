import math
from pathlib import Path
import socket
import struct
import subprocess
import sys
import unittest

import fake_peer


class ProtocolTest(unittest.TestCase):
    def test_wire_size_matches_plugin(self):
        self.assertEqual(fake_peer.SIZE, 128)

    def test_round_trip(self):
        data = fake_peer.encode(7, 1234, (1.0, -2.0, 3.5), 90.0, -5.0, (10.0, 0.0, -1.0), 'Boozer')
        state = fake_peer.decode(data)
        self.assertEqual(state['seq'], 7)
        self.assertEqual(state['pos'], (1.0, -2.0, 3.5))
        self.assertEqual(state['vel'], (10.0, 0.0, -1.0))
        self.assertEqual(state['name'], 'Boozer')
        self.assertEqual(state['flags'], fake_peer.HAS_PAWN)

    def test_little_endian_layout(self):
        data = fake_peer.encode(1, 2, (0.0, 0.0, 0.0), 0.0, 0.0, (0.0, 0.0, 0.0), 'x')
        self.assertEqual(data[:4], b'DGCO')
        self.assertEqual(struct.unpack_from('<H', data, 4)[0], fake_peer.VERSION)

    def test_rejects_foreign_packets(self):
        self.assertIsNone(fake_peer.decode(b'\0' * 64))
        self.assertIsNone(fake_peer.decode(b'DGCO'))

    def test_circle_speed(self):
        _, _, vel = fake_peer.circle_state((0, 0, 0), 3.0, 400.0, 300.0)
        self.assertAlmostEqual(math.hypot(vel[0], vel[1]), 300.0, places=3)

    def test_rejects_nonfinite_and_unknown_flags(self):
        for bad in (float('nan'), float('inf'), 1e9):
            data = fake_peer.encode(1, 0, (bad, 0, 0), 0, 0, (0, 0, 0), 'x')
            self.assertIsNone(fake_peer.decode(data))
        data = fake_peer.encode(1, 0, (0, 0, 0), 0, 0, (0, 0, 0), 'x', flags=0x8000)
        self.assertIsNone(fake_peer.decode(data))

    def test_sequence_wrap(self):
        data = fake_peer.encode(0x100000000, 0, (0, 0, 0), 0, 0, (0, 0, 0), 'x')
        self.assertEqual(fake_peer.decode(data)['seq'], 0)

    def test_follow_approaches_and_stops(self):
        target = (2000.0, 0.0, 50.0)
        ghost, vel = fake_peer.follow_step((0.0, 0.0, 0.0), target, 0.1, 700.0)
        self.assertAlmostEqual(math.hypot(vel[0], vel[1]), 700.0)
        self.assertAlmostEqual(ghost[0], 70.0)
        self.assertEqual(ghost[2], 50.0)
        _, vel = fake_peer.follow_step((1800.0, 0.0, 0.0), target, 0.1, 700.0)
        self.assertEqual(vel, (0.0, 0.0, 0.0))
        ghost, _ = fake_peer.follow_step((0.0, 0.0, 0.0), (99999.0, 0.0, 0.0), 0.1, 700.0)
        self.assertLess(math.dist(ghost, (99999.0, 0.0, 0.0)), 300.0)

    def test_follow_spot_is_front_right(self):
        # Mesh yaw -90 means the character faces +X; right is +Y in Unreal.
        spot = fake_peer.follow_spot((0.0, 0.0, 10.0), -90.0)
        self.assertAlmostEqual(spot[0], 250.0)
        self.assertAlmostEqual(spot[1], 200.0)
        self.assertEqual(spot[2], 10.0)
        spot = fake_peer.follow_spot((0.0, 0.0, 0.0), 0.0)
        self.assertAlmostEqual(spot[0], -200.0)
        self.assertAlmostEqual(spot[1], 250.0)

    def test_host_mode_handshake(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as game:
            game.bind(('127.0.0.1', 0))
            game.settimeout(3)
            process = subprocess.Popen(
                [sys.executable, str(Path(fake_peer.__file__)), '--game',
                 f'127.0.0.1:{game.getsockname()[1]}', '--port', '0', '--duration', '1'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                packet, peer = game.recvfrom(512)
                self.assertEqual(fake_peer.decode(packet)['flags'], 0)
                game.sendto(fake_peer.encode(1, 0, (100, 200, 300), 0, 0, (0, 0, 0), 'Game'), peer)
                for _ in range(30):
                    packet, _ = game.recvfrom(512)
                    state = fake_peer.decode(packet)
                    if state['flags'] & fake_peer.HAS_PAWN:
                        self.assertEqual(state['pos'][2], 300)
                        break
                else:
                    self.fail('peer never sent a pawn after receiving the game state')
                stdout, stderr = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0, stdout + stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate()


if __name__ == '__main__':
    unittest.main()
