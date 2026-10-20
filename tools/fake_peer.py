#!/usr/bin/env python3
"""Simulated co-op partner for testing DaysGoneTogether.asi with a single game instance.

It speaks the plugin's UDP protocol, waits for the game's first state packet and
then walks a ghost partner in a circle near the real player. With --serve-save it
also acts as a host sharing a SaveGames folder; with --fetch-save it acts as a
guest downloading the game's saves (the experimental save transfer).
"""
import argparse
import hashlib
import math
import os
import random
import re
import socket
import struct
import time

MAGIC = 0x4F434744
VERSION = 6
HAS_PAWN = 1
WORLD_STATE = 1 << 8
SYNC_ENEMIES = 1 << 9
BASE_FORMAT = '<IHHII3fff3f16s'
FORMAT = BASE_FORMAT + '6I7f4BIfBBIffBB'
SIZE = struct.calcsize(FORMAT)
assert SIZE == 144
POSE_MAGIC = 0x534F5044
ENEMY_INFO_MAGIC = 0x4E454D44
POSE_HEADER = '<IHBBIIIBBH3fff'
ENEMY_INFO = '<IHBBII' + '120s' * 4
POSE_ENEMY = 2
POSE_VERSION = 2
POSE_BONE_ROTATION = 11
POSE_LARGEST_MASK = 0x03
POSE_HAS_TRANSLATION = 0x04
GEAR_MAGIC = 0x52474D44
GEAR = '<IHBBII4f3f3f64s120s'
GEAR_KINDS = {0: 'none', 1: 'skeletal', 2: 'static'}
SAVE_MAGIC = 0x56534744
SAVE_VERSION = 1
SAVE_HEADER = '<IHBBII'
SAVE_HEADER_SIZE = struct.calcsize(SAVE_HEADER)
SAVE_REQUEST, SAVE_OFFER, SAVE_PULL, SAVE_CHUNK = 1, 2, 3, 4
SAVE_CHUNK_BYTES = 1024
SAVE_ENTRY = '<32sI32s'
SAVE_MAX_FILES = 32
SAVE_MAX_BYTES = 64 << 20
SAVE_NAME = re.compile(r'^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,26}\.[sS][aA][vV]$')


SAVE_DEVICE = re.compile(r'^(con|prn|aux|nul|com[0-9]|lpt[0-9])$', re.IGNORECASE)


def valid_save_name(name):
    return bool(SAVE_NAME.match(name)) and '..' not in name and not SAVE_DEVICE.match(name.split('.', 1)[0])


def build_save_bundle(files):
    """files: list of (name, bytes) -> bundle bytes in the plugin's layout."""
    if not files or len(files) > SAVE_MAX_FILES or any(not valid_save_name(n) for n, _ in files):
        raise ValueError('invalid save file list')
    head = struct.pack('<I', len(files)) + b''.join(
        struct.pack(SAVE_ENTRY, n.encode(), len(d), hashlib.sha256(d).digest()) for n, d in files)
    bundle = head + b''.join(d for _, d in files)
    if len(bundle) > SAVE_MAX_BYTES:
        raise ValueError('bundle too large')
    return bundle


def parse_save_bundle(bundle):
    (count,) = struct.unpack_from('<I', bundle)
    if not 0 < count <= SAVE_MAX_FILES:
        raise ValueError('bad file count')
    offset, files = 4 + count * struct.calcsize(SAVE_ENTRY), []
    for i in range(count):
        raw, size, digest = struct.unpack_from(SAVE_ENTRY, bundle, 4 + i * struct.calcsize(SAVE_ENTRY))
        name = raw.split(b'\0', 1)[0].decode('ascii')
        data = bundle[offset:offset + size]
        if not valid_save_name(name) or len(data) != size or hashlib.sha256(data).digest() != digest:
            raise ValueError(f'bad entry {name!r}')
        files.append((name, data))
        offset += size
    if offset != len(bundle):
        raise ValueError('trailing bytes')
    return files


def save_header(kind, session, transfer):
    return struct.pack(SAVE_HEADER, SAVE_MAGIC, SAVE_VERSION, kind, 0, session, transfer)


def decode_save_datagram(data):
    if len(data) < SAVE_HEADER_SIZE:
        return None
    magic, version, kind, _, session, transfer = struct.unpack_from(SAVE_HEADER, data)
    if magic != SAVE_MAGIC or version != SAVE_VERSION or not SAVE_REQUEST <= kind <= SAVE_CHUNK:
        return None
    return kind, session, transfer, data[SAVE_HEADER_SIZE:]


class SaveServer:
    """Host side: answers requests with an offer and pulls with chunks."""

    def __init__(self, files, session=1):
        self.bundle = build_save_bundle(files)
        self.sha = hashlib.sha256(self.bundle).digest()
        self.transfer = random.randint(1, 0xFFFFFFFF)
        self.session = session
        self.chunks_sent = 0

    def offer(self):
        return save_header(SAVE_OFFER, self.session, self.transfer) + struct.pack('<I', len(self.bundle)) + self.sha

    def handle(self, data, peer_session):
        packet = decode_save_datagram(data)
        if not packet or packet[1] != peer_session:
            return []
        kind, _, transfer, body = packet
        if kind == SAVE_REQUEST or (kind == SAVE_PULL and transfer != self.transfer):
            return [self.offer()]
        if kind != SAVE_PULL or len(body) < 2:
            return []
        (count,) = struct.unpack_from('<H', body)
        out = []
        for index in struct.unpack_from(f'<{min(count, (len(body) - 2) // 4)}I', body, 2):
            chunk = self.bundle[index * SAVE_CHUNK_BYTES:(index + 1) * SAVE_CHUNK_BYTES]
            if chunk:
                out.append(save_header(SAVE_CHUNK, self.session, self.transfer) +
                           struct.pack('<IH', index, len(chunk)) + chunk)
        self.chunks_sent += len(out)
        return out


class SaveFetcher:
    """Guest side: requests the host's save and pulls missing chunks until the digest matches."""

    def __init__(self, session=1, per_poll=24, retry=0.5):
        self.session, self.per_poll, self.retry = session, per_poll, retry
        self.transfer, self.size, self.sha = 0, 0, b''
        self.chunks, self.requested, self.next_request, self.files = {}, {}, 0.0, None

    def handle(self, data, peer_session):
        packet = decode_save_datagram(data)
        if not packet or packet[1] != peer_session or self.files is not None:
            return
        kind, _, transfer, body = packet
        if kind == SAVE_OFFER and len(body) >= 36 and transfer != self.transfer:
            self.transfer, self.size = transfer, struct.unpack_from('<I', body)[0]
            self.sha, self.chunks, self.requested = body[4:36], {}, {}
        elif kind == SAVE_CHUNK and transfer == self.transfer and len(body) >= 6:
            index, size = struct.unpack_from('<IH', body)
            if len(body) == 6 + size:
                self.chunks[index] = body[6:]

    def count(self):
        return (self.size + SAVE_CHUNK_BYTES - 1) // SAVE_CHUNK_BYTES

    def poll(self, now):
        if self.files is not None:
            return []
        if not self.transfer:
            if now < self.next_request:
                return []
            self.next_request = now + 1.0
            return [save_header(SAVE_REQUEST, self.session, 0)]
        if len(self.chunks) == self.count():
            bundle = b''.join(self.chunks[i] for i in range(self.count()))
            if hashlib.sha256(bundle).digest() == self.sha:
                self.files = parse_save_bundle(bundle)
            else:
                self.transfer = 0
            return []
        missing = [i for i in range(self.count())
                   if i not in self.chunks and now - self.requested.get(i, -1e9) >= self.retry][:self.per_poll]
        for i in missing:
            self.requested[i] = now
        if not missing:
            return []
        return [save_header(SAVE_PULL, self.session, self.transfer) +
                struct.pack(f'<H{len(missing)}I', len(missing), *missing)]


def load_save_folder(folder):
    names = sorted(n for n in os.listdir(folder) if n.lower().endswith('.sav') and valid_save_name(n))
    files = []
    for name in names:
        with open(os.path.join(folder, name), 'rb') as f:
            files.append((name, f.read()))
    return files


def decode_gear_datagram(data):
    """Returns (kind, socket, path) for a held-item datagram."""
    if len(data) != struct.calcsize(GEAR) or struct.unpack_from('<I', data)[0] != GEAR_MAGIC:
        return None
    magic, version, kind, reserved, session, number, *rest = struct.unpack(GEAR, data)
    socket, path = (v.split(b'\0', 1)[0].decode('ascii', 'replace') for v in rest[-2:])
    if version != 1 or kind not in GEAR_KINDS or reserved:
        return None
    return (GEAR_KINDS[kind], socket, path)


def decode_enemy_datagram(data):
    """Returns ('info', entity, paths) or ('pose', entity, bones, root, alive) for enemy datagrams."""
    if len(data) == struct.calcsize(ENEMY_INFO) and struct.unpack_from('<I', data)[0] == ENEMY_INFO_MAGIC:
        magic, version, count, reserved, session, entity, *paths = struct.unpack(ENEMY_INFO, data)
        if version != 1 or not entity or not 1 <= count <= 4:
            return None
        return ('info', entity, [p.split(b'\0', 1)[0].decode('ascii', 'replace') for p in paths[:count]])
    if len(data) >= struct.calcsize(POSE_HEADER) and struct.unpack_from('<I', data)[0] == POSE_MAGIC:
        (magic, version, kind, count, session, time_ms, entity, chunk, chunks, flags,
         x, y, z, yaw, health) = struct.unpack_from(POSE_HEADER, data)
        if kind != POSE_ENEMY or not entity or version != POSE_VERSION:
            return None
        offset = struct.calcsize(POSE_HEADER)
        for _ in range(count):
            if offset + POSE_BONE_ROTATION > len(data):
                return None
            info = data[offset + 4]
            if info & ~(POSE_LARGEST_MASK | POSE_HAS_TRANSLATION):
                return None
            offset += POSE_BONE_ROTATION + (6 if info & POSE_HAS_TRANSLATION else 0)
        if offset != len(data):
            return None
        return ('pose', entity, count, (x, y, z), bool(flags & 1))
    return None


def encode(seq, time_ms, pos, yaw, pitch, vel, name, flags=HAS_PAWN, *,
           session=1, target_session=0, consent=0, target_consent=0, attack_id=0, attack_ack=0,
           damage=0.0, bike_pos=(0.0, 0.0, 0.0), bike_rotation=(0.0, 0.0, 0.0),
           jacket=0, trousers=0, cap_color=0, cap=1, attack_kind=0, health=100.0,
           actions=0, weapon_type=0, shot_sequence=0, world_time_of_day=0.0,
           world_relative_time=0.0, world_weather=0, world_transition_rate=0):
    return struct.pack(FORMAT, MAGIC, VERSION, flags, seq & 0xFFFFFFFF, time_ms & 0xFFFFFFFF,
                       *pos, yaw, pitch, *vel, name.encode()[:15],
                       session, target_session, consent, target_consent, attack_id, attack_ack,
                       damage, *bike_pos, *bike_rotation, jacket, trousers, cap_color, cap, attack_kind, health,
                       actions, weapon_type, shot_sequence & 0xFFFFFFFF, world_time_of_day,
                       world_relative_time, world_weather, world_transition_rate)


def decode(data):
    if len(data) != SIZE:
        return None
    values = struct.unpack(FORMAT, data)
    magic, version, flags, seq, time_ms, x, y, z, yaw, pitch, vx, vy, vz, name = values[:14]
    session, target_session, consent, target_consent, attack_id, attack_ack = values[14:20]
    damage, bx, by, bz, bp, byaw, br = values[20:27]
    jacket, trousers, cap_color, cap, attack_kind, health, actions, weapon_type, shot_sequence = values[27:36]
    world_time_of_day, world_relative_time, world_weather, world_transition_rate = values[36:]
    if magic != MAGIC or version != VERSION or flags & ~1023:
        return None
    if any(not math.isfinite(v) or abs(v) > 1e8 for v in (x, y, z, yaw, pitch, vx, vy, vz, damage,
                                                        bx, by, bz, bp, byaw, br, health,
                                                        world_time_of_day, world_relative_time)):
        return None
    if max(jacket, trousers, cap_color) > 6 or cap > 1 or not 0 <= damage <= 1000 or not 0 <= health <= 100000:
        return None
    if actions & ~7 or weapon_type > 3:
        return None
    if flags & WORLD_STATE and not (0 <= world_time_of_day <= 24 and 0 <= world_relative_time <= 1):
        return None
    if flags & 8 and (not flags & 2 or not flags & HAS_PAWN):
        return None
    if flags & 2 and not flags & HAS_PAWN:
        return None
    if attack_kind > 2 or (attack_kind == 0 and (attack_id or damage)):
        return None
    if attack_kind and (not all((attack_id, session, target_session, consent, target_consent)) or
                        not flags & 16 or damage <= 0):
        return None
    return {'flags': flags, 'seq': seq, 'time_ms': time_ms, 'pos': (x, y, z), 'yaw': yaw,
            'pitch': pitch, 'vel': (vx, vy, vz), 'name': name.split(b'\0', 1)[0].decode(errors='replace'),
            'session': session, 'target_session': target_session, 'consent': consent,
            'target_consent': target_consent, 'attack_id': attack_id, 'attack_ack': attack_ack, 'damage': damage,
            'bike_pos': (bx, by, bz), 'bike_rotation': (bp, byaw, br), 'jacket': jacket, 'trousers': trousers,
            'cap_color': cap_color, 'cap': cap, 'attack_kind': attack_kind, 'health': health,
            'actions': actions, 'weapon_type': weapon_type, 'shot_sequence': shot_sequence,
            'world_time_of_day': world_time_of_day, 'world_relative_time': world_relative_time,
            'world_weather': world_weather, 'world_transition_rate': world_transition_rate}


def circle_state(anchor, t, radius, speed):
    omega = speed / radius
    a = omega * t
    pos = (anchor[0] + radius * math.cos(a), anchor[1] + radius * math.sin(a), anchor[2])
    vel = (-radius * omega * math.sin(a), radius * omega * math.cos(a), 0.0)
    yaw = math.degrees(math.atan2(vel[1], vel[0])) - 90.0  # Days Gone meshes face +Y.
    return pos, yaw, vel


def far_from(pos, anchor, args, slack=5000.0):
    return math.dist(pos, anchor) > args.offset + args.radius + slack


def follow_step(ghost, target, dt, max_speed, stop_dist=250.0, snap_dist=5000.0):
    """Moves the ghost toward the player; speed grows with distance so walk/run/sprint all show."""
    d = math.dist(ghost[:2], target[:2])
    if d > snap_dist:
        return tuple(target), (0.0, 0.0, 0.0)
    if d <= stop_dist:
        return (ghost[0], ghost[1], target[2]), (0.0, 0.0, 0.0)
    speed = min(max_speed, (d - stop_dist) * 1.5 + 100.0)
    step = min(speed * dt, d - stop_dist)
    ux, uy = (target[0] - ghost[0]) / d, (target[1] - ghost[1]) / d
    pos = (ghost[0] + ux * step, ghost[1] + uy * step, target[2])
    return pos, (ux * speed, uy * speed, 0.0)


def follow_spot(player_pos, mesh_yaw, ahead=250.0, side=200.0):
    """Spot in front-right of the player so the camera sees the ghost; mesh yaw + 90 is the facing."""
    a = math.radians(mesh_yaw + 90.0)
    fx, fy = math.cos(a), math.sin(a)
    return (player_pos[0] + fx * ahead - fy * side, player_pos[1] + fy * ahead + fx * side, player_pos[2])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', default='127.0.0.1:27777', help='host:port of the game plugin')
    ap.add_argument('--port', type=int, default=27778, help='local UDP port (set as PeerPort in the ini)')
    ap.add_argument('--radius', type=float, default=400.0, help='circle radius in cm')
    ap.add_argument('--speed', type=float, default=300.0, help='walking speed in cm/s (0 = stand still)')
    ap.add_argument('--offset', type=float, default=600.0, help='distance of the circle centre from the player, cm')
    ap.add_argument('--duration', type=float, default=0.0, help='stop after this many seconds (0 = until Ctrl-C)')
    ap.add_argument('--sync-enemies', action='store_true',
                    help="request the host's enemy streams and report what arrives")
    ap.add_argument('--follow', action='store_true',
                    help='walk after the player instead of circling (--speed is the maximum, e.g. 700)')
    ap.add_argument('--serve-save', metavar='DIR',
                    help='act as the host and share the *.sav files in DIR with a GuestSaveGuard=1 game')
    ap.add_argument('--fetch-save', metavar='DIR',
                    help='act as the guest and download a ShareSave=1 game host\'s saves into DIR')
    args = ap.parse_args()
    if args.serve_save and args.fetch_save:
        ap.error('--serve-save and --fetch-save are exclusive')
    host, port = args.game.rsplit(':', 1)
    if not 0 <= args.port <= 65535 or not 1 <= int(port) <= 65535:
        ap.error('ports must be between 1 and 65535 (local port 0 selects an ephemeral port)')
    if any(not math.isfinite(v) for v in (args.radius, args.speed, args.offset, args.duration)):
        ap.error('radius, speed, offset and duration must be finite')
    if args.radius <= 0 or min(args.speed, args.offset, args.duration) < 0:
        ap.error('radius must be positive; speed, offset and duration must be nonnegative')
    game = (socket.gethostbyname(host), int(port))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', args.port))
    sock.setblocking(False)
    print(f'fake peer on UDP {args.port}, sending to {game[0]}:{game[1]}; waiting for the game...')
    anchor, seq, start, last_log = None, 0, time.monotonic(), 0.0
    player, player_yaw, ghost, last_step = None, 0.0, None, time.monotonic()
    flags = HAS_PAWN | (SYNC_ENEMIES if args.sync_enemies else 0)
    enemies, enemy_packets, enemy_bones, enemy_paths, last_enemy_log = {}, 0, 0, {}, time.monotonic()
    held = None
    server = SaveServer(load_save_folder(args.serve_save)) if args.serve_save else None
    fetcher = SaveFetcher() if args.fetch_save else None
    if server:
        print(f'serving {len(server.bundle)} byte save bundle from {args.serve_save}')
    game_session, last_save_log = 0, 0.0
    while not args.duration or time.monotonic() - start < args.duration:
        while True:
            try:
                data, sender = sock.recvfrom(2048)
            except BlockingIOError:
                break
            if sender != game:
                continue
            if decode_save_datagram(data):
                for reply in server.handle(data, game_session) if server else []:
                    sock.sendto(reply, game)
                if fetcher:
                    fetcher.handle(data, game_session)
                continue
            gear = decode_gear_datagram(data)
            if gear and gear != held:
                held = gear
                print(f'held item: {gear[0]} {gear[2] or "-"} at {gear[1] or "-"}')
            enemy = decode_enemy_datagram(data)
            if enemy and enemy[0] == 'info':
                if enemy_paths.get(enemy[1]) != enemy[2]:
                    print(f'enemy {enemy[1]} look: {", ".join(enemy[2])}')
                enemy_paths[enemy[1]] = enemy[2]
            elif enemy:
                enemies[enemy[1]] = (enemy[3], enemy[4])
                enemy_packets += 1
                enemy_bones += enemy[2]
            state = decode(data)
            if not state:
                continue
            game_session = state['session']
            # (Re)anchor on first contact and whenever the player jumps far away (save load, fast travel).
            if state['flags'] & HAS_PAWN and (anchor is None or far_from(state['pos'], anchor, args)):
                x, y, z = state['pos']
                anchor = (x + args.offset, y, z)
                print(f"player '{state['name']}' at {state['pos']}, ghost circles {anchor}")
            if state['flags'] & HAS_PAWN:
                player = follow_spot(state['pos'], state['yaw'])
                player_yaw = state['yaw']
            now = time.monotonic()
            if now - last_log > 2:
                last_log = now
                print(f"game state seq={state['seq']} pos={tuple(round(v) for v in state['pos'])} "
                      f"yaw={state['yaw']:.0f} flags={state['flags']}")
        if args.sync_enemies and time.monotonic() - last_enemy_log >= 5:
            span = time.monotonic() - last_enemy_log
            alive = sum(1 for _, a in enemies.values() if a)
            print(f'enemies: {len(enemies)} streamed ({alive} alive), {enemy_packets / span:.1f} pose pkt/s, '
                  f'{enemy_bones / max(enemy_packets, 1):.0f} bones/pkt')
            enemies, enemy_packets, enemy_bones, last_enemy_log = {}, 0, 0, time.monotonic()
        if fetcher and game_session:
            for packet in fetcher.poll(time.monotonic()):
                sock.sendto(packet, game)
            if fetcher.files is not None and fetcher.transfer:
                os.makedirs(args.fetch_save, exist_ok=True)
                for name, data in fetcher.files:
                    with open(os.path.join(args.fetch_save, name), 'wb') as f:
                        f.write(data)
                    print(f'fetched {name} {len(data)} bytes sha256={hashlib.sha256(data).hexdigest()}')
                fetcher.transfer = 0
            elif time.monotonic() - last_save_log > 2 and fetcher.transfer:
                last_save_log = time.monotonic()
                print(f'save transfer {len(fetcher.chunks)}/{fetcher.count()} chunks')
        if server and server.chunks_sent and time.monotonic() - last_save_log > 2:
            last_save_log = time.monotonic()
            print(f'save transfer: {server.chunks_sent} chunks sent')
        t = time.monotonic() - start
        seq += 1
        now = time.monotonic()
        dt, last_step = now - last_step, now
        if args.follow and player is not None:
            ghost, vel = follow_step(ghost or player, player, dt, args.speed or 700.0, stop_dist=80.0)
            ghost_yaw = math.degrees(math.atan2(vel[1], vel[0])) - 90.0 if vel[0] or vel[1] else player_yaw
            sock.sendto(encode(seq, int(t * 1000), ghost, ghost_yaw, 0.0, vel, 'GhostPartner', flags=flags), game)
        elif anchor is not None:
            if args.speed > 0:
                pos, yaw, vel = circle_state(anchor, t, args.radius, args.speed)
            else:
                pos, yaw, vel = anchor, 0.0, (0.0, 0.0, 0.0)
            sock.sendto(encode(seq, int(t * 1000), pos, yaw, 0.0, vel, 'GhostPartner', flags=flags), game)
        else:
            # A host-mode game needs our first packet before it can reply with its position.
            sock.sendto(encode(seq, int(t * 1000), (0, 0, 0), 0, 0, (0, 0, 0), 'GhostPartner', flags=0), game)
        time.sleep(1 / 30)
    sock.close()


if __name__ == '__main__':
    main()
