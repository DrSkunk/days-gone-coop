#!/usr/bin/env python3
"""Simulated co-op partner for testing DaysGoneCoop.asi with a single game instance.

It speaks the plugin's UDP protocol, waits for the game's first state packet and
then walks a ghost partner in a circle near the real player.
"""
import argparse
import math
import socket
import struct
import time

MAGIC = 0x4F434744
VERSION = 4
HAS_PAWN = 1
WORLD_STATE = 1 << 8
BASE_FORMAT = '<IHHII3fff3f16s'
FORMAT = BASE_FORMAT + '6I7f4BIfBBIffBB'
SIZE = struct.calcsize(FORMAT)
assert SIZE == 144


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
    if magic != MAGIC or version != VERSION or flags & ~511:
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
    ap.add_argument('--follow', action='store_true',
                    help='walk after the player instead of circling (--speed is the maximum, e.g. 700)')
    args = ap.parse_args()
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
    while not args.duration or time.monotonic() - start < args.duration:
        while True:
            try:
                data, sender = sock.recvfrom(512)
            except BlockingIOError:
                break
            if sender != game:
                continue
            state = decode(data)
            if not state:
                continue
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
        t = time.monotonic() - start
        seq += 1
        now = time.monotonic()
        dt, last_step = now - last_step, now
        if args.follow and player is not None:
            ghost, vel = follow_step(ghost or player, player, dt, args.speed or 700.0, stop_dist=80.0)
            ghost_yaw = math.degrees(math.atan2(vel[1], vel[0])) - 90.0 if vel[0] or vel[1] else player_yaw
            sock.sendto(encode(seq, int(t * 1000), ghost, ghost_yaw, 0.0, vel, 'GhostPartner'), game)
        elif anchor is not None:
            if args.speed > 0:
                pos, yaw, vel = circle_state(anchor, t, args.radius, args.speed)
            else:
                pos, yaw, vel = anchor, 0.0, (0.0, 0.0, 0.0)
            sock.sendto(encode(seq, int(t * 1000), pos, yaw, 0.0, vel, 'GhostPartner'), game)
        else:
            # A host-mode game needs our first packet before it can reply with its position.
            sock.sendto(encode(seq, int(t * 1000), (0, 0, 0), 0, 0, (0, 0, 0), 'GhostPartner', flags=0), game)
        time.sleep(1 / 30)
    sock.close()


if __name__ == '__main__':
    main()
