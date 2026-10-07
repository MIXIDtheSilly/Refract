"""Refract pose server with controller input (replaces `refract-host-bridge --serve`).

Streams v4 Refract pose records (fixed head, two active Touch controllers) on TCP
38490 and fills the controller buttons from:
  * an Xbox/XInput gamepad (always):
      A/B -> right A/B, X/Y -> left X/Y, RT/LT -> triggers, RB/LB -> grips,
      sticks -> thumbsticks, stick clicks -> thumbstick clicks, Start -> left menu
  * the keyboard, only while the foreground window title matches --focus (on Linux, the keys
    the viewer reports as held while it has focus, `keys <names>` on the UDP port below):
      Space/Enter -> right A, Backspace -> right B, Z -> left X, X -> left Y,
      E/Q -> right/left trigger, R/F -> right/left grip, Tab/M -> left menu (settings),
      arrow keys -> right stick
      WASD -> walk (moves the tracked head and hands, like walking in your room;
      Yeeps has no stick locomotion), Shift = faster, Home = back to the start
      T (hold) -> T-pose: look level, arms straight out to the sides (calibration
      screens, e.g. AC Nexus's "look ahead and fully extend your arms")
      C (hold) -> right controller to the centre of the view, `reach` meters out (scroll
      wheel), to touch things you look at (e.g. AC Nexus's "touch the cube")
  * UDP text commands on 127.0.0.1:38495 for scripting, e.g.
      hold right a 3        (press right A for 3 s)
      hold left trigger 1.5
      stick right 0 1 2     (right stick up for 2 s)
      view <yaw> <pitch> <trigger> <grip> [<reach>]   (viewer mouse state; degrees, 0/1, meters)
      keys w shift space    (keys held in the Linux viewer; resent twice a second)
      tpose 3               (hold the T-pose for 3 s)
      center 3              (right controller at the view centre for 3 s)
"""
import argparse
import ctypes
import math
import re
import socket
import struct
import sys
import threading
import time

MAGIC, VERSION, TYPE = 0x54434652, 4, 1  # v4 adds aim poses/flags and the recommended eye size.
TRACKED = 15  # Orientation + position valid and tracked.
PRIMARY, SECONDARY, MENU, STICK_CLICK = 1, 2, 4, 8
PRIMARY_TOUCH, SECONDARY_TOUCH, TRIGGER_TOUCH, STICK_TOUCH = 16, 32, 64, 128
DEFAULT_REACH, MIN_REACH, MAX_REACH = 0.4, 0.1, 1.0  # Hand distance in front of the camera, meters.
TPOSE_HALF_SPAN, TPOSE_DROP = 0.8, -0.22  # T-pose controllers: meters beside and below the eyes.

WINDOWS = sys.platform == 'win32'
user32 = ctypes.windll.user32 if WINDOWS else None
# Keys by the names the Linux viewer reports; Windows reads the same keys as virtual-key codes.
VK = {'space': 0x20, 'enter': 0x0D, 'backspace': 0x08, 'tab': 0x09, 'shift': 0x10, 'home': 0x24,
      'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28, **{c: ord(c.upper()) for c in 'wasdzxmeqrftc'}}


class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [('wButtons', ctypes.c_ushort), ('bLeftTrigger', ctypes.c_ubyte), ('bRightTrigger', ctypes.c_ubyte),
                ('sThumbLX', ctypes.c_short), ('sThumbLY', ctypes.c_short),
                ('sThumbRX', ctypes.c_short), ('sThumbRY', ctypes.c_short)]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [('dwPacketNumber', ctypes.c_uint), ('Gamepad', XINPUT_GAMEPAD)]


try:
    xinput = ctypes.windll.xinput1_4 if WINDOWS else None
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


def key_reader(focus, scripted):
    """down(name) for this pose: the keys the Linux viewer reports, and on Windows the keyboard while the viewer has focus."""
    held = scripted.keys()
    if WINDOWS and focus.search(foreground_title()):
        return lambda name: name in held or bool(user32.GetAsyncKeyState(VK[name]) & 0x8000)
    return lambda name: name in held


def walk_keys(down):
    """(strafe, forward, fast, reset, tpose, center) from WASD/Shift/Home/T/C."""
    strafe = float(down('d')) - float(down('a'))
    forward = float(down('w')) - float(down('s'))
    return strafe, forward, down('shift'), down('home'), down('t'), down('c')


def foreground_title():
    hwnd = user32.GetForegroundWindow()
    buffer = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buffer, 512)
    return buffer.value


def keyboard(down):
    left, right = Hand(), Hand()
    if down('space') or down('enter'): right.buttons |= PRIMARY
    if down('backspace'): right.buttons |= SECONDARY
    if down('z'): left.buttons |= PRIMARY
    if down('x'): left.buttons |= SECONDARY
    if down('tab') or down('m'): left.buttons |= MENU
    if down('e'): right.trigger = 1.0
    if down('q'): left.trigger = 1.0
    if down('r'): right.squeeze = 1.0
    if down('f'): left.squeeze = 1.0
    right.x = (1.0 if down('right') else 0.0) - (1.0 if down('left') else 0.0)
    right.y = (1.0 if down('up') else 0.0) - (1.0 if down('down') else 0.0)
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
        self.tpose_until = self.center_until = 0.0
        self.held, self.held_at = frozenset(), 0.0  # Keys held in the Linux viewer, and when it last said so.
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
            if words[:1] == ['keys']:
                with self.lock:
                    self.held, self.held_at = frozenset(w for w in words[1:] if w in VK), time.monotonic()
                continue
            if words[:1] in (['tpose'], ['center']) and len(words) == 2:
                try:
                    until = time.monotonic() + float(words[1])
                except ValueError:
                    continue
                print(f'command {" ".join(words)}', flush=True)
                with self.lock:
                    if words[0] == 'tpose': self.tpose_until = until
                    else: self.center_until = until
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

    def keys(self):
        # The viewer resends its keys twice a second; a viewer that went away holds nothing.
        with self.lock:
            return self.held if time.monotonic() - self.held_at < 1.5 else frozenset()

    def view(self):
        with self.lock:
            return self.yaw, self.pitch, self.reach

    def tpose(self):
        with self.lock:
            return time.monotonic() < self.tpose_until

    def center(self):
        with self.lock:
            return time.monotonic() < self.center_until


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
           refresh_rate=90.0, tpose=False, center=False):
    head_pos = (offset[0] + math.sin(t) * 0.02, 1.65, offset[1] - 0.05)
    rotation = look_rotation(yaw, 0.0 if tpose else pitch)  # T-pose: look straight ahead.
    head = (*head_pos, *rotation)

    def hand_pose(side, bob):
        if tpose:
            # Arms straight out to the sides at shoulder height, controllers pointing forward
            # (about 1.6 m between the controllers, like a 1.75 m adult's arm span).
            dx, dy, dz = rotate(rotation, (TPOSE_HALF_SPAN * side, TPOSE_DROP, 0.0))
            return (head_pos[0] + dx, head_pos[1] + dy, head_pos[2] + dz, *rotation)
        # Hands are fixed in the camera's frame (they turn and tilt with it), `reach` meters
        # in front, and aim where the camera looks. `center` puts the right one on the view axis.
        local = (0.0, 0.0, -reach) if center and side > 0 else (0.22 * side, -0.28 + bob, -reach)
        dx, dy, dz = rotate(rotation, local)
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
                    down = key_reader(focus, scripted)
                    for source in (gamepad(), keyboard(down), scripted.hands()):
                        hands[0].merge(source[0])
                        hands[1].merge(source[1])
                    summary = tuple((h.buttons, round(h.trigger, 2), round(h.squeeze, 2), round(h.x, 2), round(h.y, 2)) for h in hands)
                    if summary != previous:
                        print(f'input left={summary[0]} right={summary[1]}', flush=True)
                        previous = summary
                    now = time.monotonic()
                    dt, last = min(now - last, 0.1), now
                    yaw, pitch, reach = scripted.view()
                    strafe, forward, fast, reset, tpose_key, center_key = walk_keys(down)
                    tpose = tpose_key or scripted.tpose()
                    center = center_key or scripted.center()
                    if reset:
                        position = [0.0, 0.0]
                    if strafe or forward:
                        # Walk where the head faces: OpenXR forward is -Z, yaw turns about +Y.
                        r = math.radians(yaw)
                        step = args.walk_speed * (2.0 if fast else 1.0) * dt / math.hypot(strafe, forward)
                        position[0] += (strafe * math.cos(r) - forward * math.sin(r)) * step
                        position[1] += (-strafe * math.sin(r) - forward * math.cos(r)) * step
                    client.sendall(record(sequence, sequence / 90.0, hands, yaw, pitch, reach, position,
                                                (args.eye_width, args.eye_height), args.refresh_rate, tpose, center))
                    sequence += 1
                    time.sleep(1.0 / args.refresh_rate)
            except OSError as error:
                print(f'client disconnected: {error}', flush=True)
                client.close()


if __name__ == '__main__':
    main()
