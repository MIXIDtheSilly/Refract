"""Refract pose server with controller input (replaces `refract-host-bridge --serve`).

Streams v4 Refract pose records (fixed head, two active Touch controllers) on TCP
38490 and fills the controller buttons from:
  * an Xbox/XInput gamepad (always):
      A/B -> right A/B, X/Y -> left X/Y, RT/LT -> triggers, RB/LB -> grips,
      sticks -> thumbsticks, stick clicks -> thumbstick clicks, Start -> left menu
  * the keyboard, only while the foreground window title matches --focus:
      Space/Enter -> right A, Backspace -> right B, Z -> left X, X -> left Y,
      E/Q -> right/left trigger, R/F -> right/left grip, Tab/M -> left menu (settings),
      arrow keys -> right stick
      WASD -> walk (moves the tracked head and hands, like walking in your room;
      Yeeps has no stick locomotion), Shift = faster, Home = back to the start
  * UDP text commands on 127.0.0.1:38495 for scripting, e.g.
      hold right a 3        (press right A for 3 s)
      hold left trigger 1.5
      stick right 0 1 2     (right stick up for 2 s)
      view <yaw> <pitch> <trigger> <grip> [<reach>]   (viewer mouse state; degrees, 0/1, meters)
"""
import argparse
import ctypes
import math
import re
import socket
import struct
import threading
import time

MAGIC, VERSION, TYPE = 0x54434652, 4, 1  # v4 adds aim poses/flags and the recommended eye size.
TRACKED = 15  # Orientation + position valid and tracked.
PRIMARY, SECONDARY, MENU, STICK_CLICK = 1, 2, 4, 8
PRIMARY_TOUCH, SECONDARY_TOUCH, TRIGGER_TOUCH, STICK_TOUCH = 16, 32, 64, 128
DEFAULT_REACH, MIN_REACH, MAX_REACH = 0.4, 0.1, 1.0  # Hand distance in front of the camera, meters.

user32 = ctypes.windll.user32


class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [('wButtons', ctypes.c_ushort), ('bLeftTrigger', ctypes.c_ubyte), ('bRightTrigger', ctypes.c_ubyte),
                ('sThumbLX', ctypes.c_short), ('sThumbLY', ctypes.c_short),
                ('sThumbRX', ctypes.c_short), ('sThumbRY', ctypes.c_short)]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [('dwPacketNumber', ctypes.c_uint), ('Gamepad', XINPUT_GAMEPAD)]


try:
    xinput = ctypes.windll.xinput1_4
except OSError:
    xinput = None


class Hand:
    def __init__(self):
        self.buttons = 0
        self.trigger = self.squeeze = self.x = self.y = 0.0

    def merge(self, other):
        self.buttons |= other.buttons
        self.trigger = max(self.trigger, other.trigger)
        self.squeeze = max(self.squeeze, other.squeeze)
        if other.x * other.x + other.y * other.y > self.x * self.x + self.y * self.y:
            self.x, self.y = other.x, other.y


def stick(value):
    v = value / 32767.0
    return 0.0 if abs(v) < 0.24 else max(-1.0, min(1.0, v))


def gamepad():
    left, right = Hand(), Hand()
    if not xinput:
        return left, right
    for index in range(4):
        state = XINPUT_STATE()
        if xinput.XInputGetState(index, ctypes.byref(state)) != 0:
            continue
        pad = state.Gamepad
        b = pad.wButtons
        if b & 0x1000: right.buttons |= PRIMARY
        if b & 0x2000: right.buttons |= SECONDARY
        if b & 0x4000: left.buttons |= PRIMARY
        if b & 0x8000: left.buttons |= SECONDARY
        if b & 0x0010: left.buttons |= MENU
        if b & 0x0040: left.buttons |= STICK_CLICK
        if b & 0x0080: right.buttons |= STICK_CLICK
        left.squeeze = 1.0 if b & 0x0100 else 0.0
        right.squeeze = 1.0 if b & 0x0200 else 0.0
        left.trigger, right.trigger = pad.bLeftTrigger / 255.0, pad.bRightTrigger / 255.0
        left.x, left.y = stick(pad.sThumbLX), stick(pad.sThumbLY)
        right.x, right.y = stick(pad.sThumbRX), stick(pad.sThumbRY)
        break
    return left, right


def walk_keys(focus):
    """(strafe, forward, fast, reset) from WASD/Shift/Home while the viewer has focus."""
    if not focus.search(foreground_title()):
        return 0.0, 0.0, False, False
    down = lambda vk: bool(user32.GetAsyncKeyState(vk) & 0x8000)
    strafe = float(down(ord('D'))) - float(down(ord('A')))
    forward = float(down(ord('W'))) - float(down(ord('S')))
    return strafe, forward, down(0x10), down(0x24)


def foreground_title():
    hwnd = user32.GetForegroundWindow()
    buffer = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buffer, 512)
    return buffer.value


def keyboard(focus):
    left, right = Hand(), Hand()
    if not focus.search(foreground_title()):
        return left, right
    down = lambda vk: user32.GetAsyncKeyState(vk) & 0x8000
    if down(0x20) or down(0x0D): right.buttons |= PRIMARY
    if down(0x08): right.buttons |= SECONDARY
    if down(ord('Z')): left.buttons |= PRIMARY
    if down(ord('X')): left.buttons |= SECONDARY
    if down(0x09) or down(ord('M')): left.buttons |= MENU
    if down(ord('E')): right.trigger = 1.0
    if down(ord('Q')): left.trigger = 1.0
    if down(ord('R')): right.squeeze = 1.0
    if down(ord('F')): left.squeeze = 1.0
    right.x = (1.0 if down(0x27) else 0.0) - (1.0 if down(0x25) else 0.0)
    right.y = (1.0 if down(0x26) else 0.0) - (1.0 if down(0x28) else 0.0)
    return left, right


class Scripted:
    """Timed inputs from UDP commands."""
    NAMES = {'a': PRIMARY, 'x': PRIMARY, 'b': SECONDARY, 'y': SECONDARY, 'menu': MENU, 'stick': STICK_CLICK}

    def __init__(self, port):
        self.lock = threading.Lock()
        self.events = []  # (until, hand_index, kind, value)
        # Mouse state from the viewer: head yaw/pitch (degrees), right trigger/grip.
        self.yaw = self.pitch = 0.0
        self.reach = DEFAULT_REACH
        self.mouse_trigger = self.mouse_grip = 0.0
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', port))
        threading.Thread(target=self.listen, args=(sock,), daemon=True).start()

    def listen(self, sock):
        while True:
            data, _ = sock.recvfrom(256)
            words = data.decode(errors='replace').split()
            if words[:1] == ['view'] and len(words) in (5, 6):
                try:
                    yaw, pitch, trigger, grip, *reach = (float(w) for w in words[1:])
                except ValueError:
                    continue
                with self.lock:
                    self.yaw, self.pitch = yaw, max(-80.0, min(80.0, pitch))
                    self.mouse_trigger, self.mouse_grip = trigger, grip
                    if reach:
                        self.reach = max(MIN_REACH, min(MAX_REACH, reach[0]))
                continue
            try:
                command, hand = words[0], ('left', 'right').index(words[1])
                if command == 'hold':
                    event = (time.monotonic() + float(words[3]), hand, words[2], 1.0)
                elif command == 'stick':
                    event = (time.monotonic() + float(words[4]), hand, 'xy', (float(words[2]), float(words[3])))
                else:
                    raise ValueError(command)
            except (ValueError, IndexError) as error:
                print(f'bad command {words}: {error}', flush=True)
                continue
            print(f'command {" ".join(words)}', flush=True)
            with self.lock:
                self.events.append(event)

    def hands(self):
        hands = (Hand(), Hand())
        now = time.monotonic()
        with self.lock:
            self.events = [e for e in self.events if e[0] > now]
            for _, index, kind, value in self.events:
                hand = hands[index]
                if kind in self.NAMES: hand.buttons |= self.NAMES[kind]
                elif kind == 'trigger': hand.trigger = value
                elif kind in ('grip', 'squeeze'): hand.squeeze = value
                elif kind == 'xy': hand.x, hand.y = value
            hands[1].trigger = max(hands[1].trigger, self.mouse_trigger)
            hands[1].squeeze = max(hands[1].squeeze, self.mouse_grip)
        return hands

    def view(self):
        with self.lock:
            return self.yaw, self.pitch, self.reach


def rotate(q, v):
    """Rotate vector v by unit quaternion q (x, y, z, w)."""
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + y * tz - z * ty,
            v[1] + w * ty + z * tx - x * tz,
            v[2] + w * tz + x * ty - y * tx)


def look_rotation(yaw_deg, pitch_deg):
    """Quaternion (x, y, z, w) for yaw about +Y then pitch about +X (OpenXR: -Z forward)."""
    y, p = math.radians(yaw_deg) / 2, math.radians(pitch_deg) / 2
    sy, cy, sp, cp = math.sin(y), math.cos(y), math.sin(p), math.cos(p)
    return (cy * sp, sy * cp, -sy * sp, cy * cp)


def record(sequence, t, hands, yaw=0.0, pitch=0.0, reach=DEFAULT_REACH, offset=(0.0, 0.0), eye_size=(1024, 1024),
           refresh_rate=90.0):
    head_pos = (offset[0] + math.sin(t) * 0.02, 1.65, offset[1] - 0.05)
    rotation = look_rotation(yaw, pitch)
    head = (*head_pos, *rotation)

    def hand_pose(side, bob):
        # Hands are fixed in the camera's frame (they turn and tilt with it), `reach` meters
        # in front, and aim where the camera looks.
        dx, dy, dz = rotate(rotation, (0.22 * side, -0.28 + bob, -reach))
        return (head_pos[0] + dx, head_pos[1] + dy, head_pos[2] + dz, *rotation)

    left_pose = hand_pose(-1, math.sin(t * 2) * 0.02)
    right_pose = hand_pose(1, math.cos(t * 2) * 0.02)
    data = struct.pack('<IHHQQ', MAGIC, VERSION, TYPE, sequence, time.monotonic_ns())
    data += struct.pack('<7f7f7fI', *head, *left_pose, *right_pose, 0)
    for hand in hands:
        buttons = hand.buttons
        if buttons & PRIMARY: buttons |= PRIMARY_TOUCH
        if buttons & SECONDARY: buttons |= SECONDARY_TOUCH
        if hand.trigger > 0.05: buttons |= TRIGGER_TOUCH
        if buttons & STICK_CLICK or hand.x or hand.y: buttons |= STICK_TOUCH
        data += struct.pack('<II4f', 1, buttons, hand.trigger, hand.squeeze, hand.x, hand.y)
    # v3/v4 tail: aim poses (= grip poses), grip/aim flags, aim active, two inactive
    # hand skeletons (26 joints x 40 bytes each), hand tracking unsupported,
    # display period (Refract paces xrWaitFrame to it), recommended eye size.
    data += struct.pack('<7f7f', *left_pose, *right_pose)
    data += struct.pack('<QQQQII', TRACKED, TRACKED, TRACKED, TRACKED, 1, 1)
    data += bytes(2 * (8 + 26 * 40))
    data += struct.pack('<IIII', 0, round(1e9 / refresh_rate), *eye_size)
    assert len(data) == 2368
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, default=38490)
    parser.add_argument('--control-port', type=int, default=38495)
    parser.add_argument('--focus', default=r'Refract Viewer|Android Emulator|Emulator', help='regex for keyboard focus window title')
    parser.add_argument('--refresh-rate', type=float, default=250.0,
                        help='display rate the game paces to, 40-250 Hz; poses are sent at this rate too')
    parser.add_argument('--eye-width', type=int, default=1024, help='recommended eye render width (read by the game at startup)')
    parser.add_argument('--eye-height', type=int, default=1024)
    parser.add_argument('--walk-speed', type=float, default=1.5, help='WASD speed in m/s (Shift doubles it)')
    args = parser.parse_args()
    focus = re.compile(args.focus, re.I)
    scripted = Scripted(args.control_port)
    print(f'XInput: {"available" if xinput else "missing"}; control udp {args.control_port}', flush=True)
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('0.0.0.0', args.port))
        server.listen(1)
        print(f'listening on {args.port}', flush=True)
        sequence, previous = 0, None
        position = [0.0, 0.0]  # Walked offset on the floor (x, z), meters.
        last = time.monotonic()
        while True:
            client, _ = server.accept()
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print('client connected', flush=True)
            try:
                while True:
                    hands = (Hand(), Hand())
                    for source in (gamepad(), keyboard(focus), scripted.hands()):
                        hands[0].merge(source[0])
                        hands[1].merge(source[1])
                    summary = tuple((h.buttons, round(h.trigger, 2), round(h.squeeze, 2), round(h.x, 2), round(h.y, 2)) for h in hands)
                    if summary != previous:
                        print(f'input left={summary[0]} right={summary[1]}', flush=True)
                        previous = summary
                    now = time.monotonic()
                    dt, last = min(now - last, 0.1), now
                    yaw, pitch, reach = scripted.view()
                    strafe, forward, fast, reset = walk_keys(focus)
                    if reset:
                        position = [0.0, 0.0]
                    if strafe or forward:
                        # Walk where the head faces: OpenXR forward is -Z, yaw turns about +Y.
                        r = math.radians(yaw)
                        step = args.walk_speed * (2.0 if fast else 1.0) * dt / math.hypot(strafe, forward)
                        position[0] += (strafe * math.cos(r) - forward * math.sin(r)) * step
                        position[1] += (-strafe * math.sin(r) - forward * math.cos(r)) * step
                    client.sendall(record(sequence, sequence / 90.0, hands, yaw, pitch, reach, position,
                                                (args.eye_width, args.eye_height), args.refresh_rate))
                    sequence += 1
                    time.sleep(1.0 / args.refresh_rate)
            except OSError as error:
                print(f'client disconnected: {error}', flush=True)
                client.close()


if __name__ == '__main__':
    main()
