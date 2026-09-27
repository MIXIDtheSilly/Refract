"""Live window for the Refract TCP image stream (replaces stream_capture.py).

Shows the left eye (or both with --both) as frames arrive on port 38491.
The window title contains "Refract Viewer", so pose_input_server.py accepts
keyboard controls while this window has focus.
Mouse: right-drag to look around, left button = right trigger,
middle button = right grip. Mouse state goes to pose_input_server.py over UDP.
Keys: F2 save screenshot, F3 toggle both eyes, F4 toggle full/half size,
Home recenters the view.
"""
import argparse
import socket
import struct
import threading
import time
import tkinter as tk
from pathlib import Path

from capture import read_exact, write_png


class Stream:
    def __init__(self, port):
        self.lock = threading.Lock()
        self.frame = None  # (version, width, height, layers, payload, sequence)
        self.received = 0
        self.status = 'waiting for game...'
        threading.Thread(target=self.serve, args=(port,), daemon=True).start()

    def serve(self, port):
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(('127.0.0.1', port))
            server.listen(1)
            while True:
                connection, _ = server.accept()
                self.status = 'connected'
                with connection:
                    connection.settimeout(30)
                    try:
                        while True:
                            header = read_exact(connection, 64)
                            magic, version, kind, size, width, height, layers, fmt, bpp, _ = struct.unpack_from('<IHH7I', header)
                            sequence, _, payload_size = struct.unpack_from('<QQQ', header, 40)
                            if magic != 0x49585241 or payload_size > 256 * 1024 * 1024:
                                raise ValueError('bad frame header')
                            if size > 64:
                                read_exact(connection, size - 64)
                            payload = read_exact(connection, payload_size)
                            if fmt == 1 and bpp == 4:
                                with self.lock:
                                    self.frame = (version, width, height, layers, payload, sequence)
                                    self.received += 1
                    except (EOFError, ConnectionError, socket.timeout, ValueError) as error:
                        self.status = f'disconnected ({error}), waiting...'


def to_ppm(rgba, width, height, flip):
    rgb = bytearray(width * height * 3)
    rgb[0::3] = rgba[0::4]
    rgb[1::3] = rgba[1::4]
    rgb[2::3] = rgba[2::4]
    if flip:  # OpenGL/Vulkan readback rows start at the bottom of the image.
        stride = width * 3
        rgb = b''.join(rgb[y * stride:(y + 1) * stride] for y in reversed(range(height)))
    return b'P6 %d %d 255\n' % (width, height) + bytes(rgb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=38491)
    parser.add_argument('--both', action='store_true', help='show both eyes side by side')
    parser.add_argument('--shots', type=Path, default=Path('screenshots'))
    parser.add_argument('--control-port', type=int, default=38495, help='pose_input_server.py UDP port')
    parser.add_argument('--sensitivity', type=float, default=0.25, help='degrees per pixel of right-drag')
    args = parser.parse_args()

    stream = Stream(args.port)
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    mouse = {'yaw': 0.0, 'pitch': 0.0, 'trigger': 0, 'grip': 0, 'drag': None, 'sent': None, 'sent_at': 0.0}
    root = tk.Tk()
    root.title('Refract Viewer')
    root.configure(background='black')
    label = tk.Label(root, background='black', foreground='white', text=stream.status, font=('Segoe UI', 14))
    label.pack(fill='both', expand=True)
    state = {'both': args.both, 'zoom': 1, 'shown': None, 'fps_t': time.monotonic(), 'fps_n': 0, 'fps': 0.0, 'image': None}

    def save(_event=None):
        frame = state['shown']
        if not frame:
            return
        version, width, height, layers, payload, sequence = frame
        args.shots.mkdir(parents=True, exist_ok=True)
        path = args.shots / f'yeeps-{time.strftime("%Y%m%d-%H%M%S")}-seq{sequence}.png'
        write_png(path, width, height, payload[:width * height * 4])
        print(f'saved {path}', flush=True)

    def toggle_both(_event=None):
        state['both'] = not state['both']
        state['shown'] = None

    def toggle_zoom(_event=None):
        state['zoom'] = 2 if state['zoom'] == 1 else 1
        state['shown'] = None

    root.bind('<F2>', save)
    root.bind('<F3>', toggle_both)
    root.bind('<F4>', toggle_zoom)

    def look_start(event):
        mouse['drag'] = (event.x, event.y)

    def look_move(event):
        if mouse['drag'] is None:
            return
        dx, dy = event.x - mouse['drag'][0], event.y - mouse['drag'][1]
        mouse['drag'] = (event.x, event.y)
        mouse['yaw'] = (mouse['yaw'] - dx * args.sensitivity + 180.0) % 360.0 - 180.0
        mouse['pitch'] = max(-80.0, min(80.0, mouse['pitch'] - dy * args.sensitivity))

    def look_end(_event):
        mouse['drag'] = None

    def button(name, value):
        def handler(_event):
            mouse[name] = value
        return handler

    def recenter(_event=None):
        mouse['yaw'] = mouse['pitch'] = 0.0

    root.bind('<ButtonPress-3>', look_start)
    root.bind('<B3-Motion>', look_move)
    root.bind('<ButtonRelease-3>', look_end)
    root.bind('<ButtonPress-1>', button('trigger', 1))
    root.bind('<ButtonRelease-1>', button('trigger', 0))
    root.bind('<ButtonPress-2>', button('grip', 1))
    root.bind('<ButtonRelease-2>', button('grip', 0))
    root.bind('<Home>', recenter)

    def send_mouse():
        state_now = (round(mouse['yaw'], 2), round(mouse['pitch'], 2), mouse['trigger'], mouse['grip'])
        now = time.monotonic()
        # Resend periodically too, so a restarted input server picks the view back up.
        if state_now != mouse['sent'] or now - mouse['sent_at'] > 1.0:
            control.sendto(('view %.2f %.2f %d %d' % state_now).encode(), ('127.0.0.1', args.control_port))
            mouse['sent'], mouse['sent_at'] = state_now, now

    def tick():
        send_mouse()
        with stream.lock:
            frame, received = stream.frame, stream.received
        if frame is not None and frame is not state['shown']:
            version, width, height, layers, payload, sequence = frame
            eye = width * height * 4
            # Stereo scene frames (v1/v2) arrive bottom-up; quad panel frames (v4) top-down.
            flip = version != 4
            image = tk.PhotoImage(data=to_ppm(payload[:eye], width, height, flip), format='PPM')
            if state['both'] and layers >= 2 and len(payload) >= eye * 2:
                right = tk.PhotoImage(data=to_ppm(payload[eye:eye * 2], width, height, flip), format='PPM')
                combined = tk.PhotoImage(width=width * 2, height=height)
                combined.tk.call(combined, 'copy', image, '-to', 0, 0)
                combined.tk.call(combined, 'copy', right, '-to', width, 0)
                image = combined
            if state['zoom'] == 2:
                image = image.subsample(2, 2)
            label.configure(image=image, text='')
            state['image'] = image
            state['shown'] = frame
            state['fps_n'] += 1
        elif frame is None:
            label.configure(text=stream.status)
        now = time.monotonic()
        if now - state['fps_t'] >= 1.0:
            state['fps'] = state['fps_n'] / (now - state['fps_t'])
            state['fps_t'], state['fps_n'] = now, 0
            root.title(f'Refract Viewer  |  {state["fps"]:.1f} fps  |  right-drag look, '
                       f'left click trigger  |  F2 screenshot, F4 size, Home recenter  |  {stream.status}')
        root.after(8, tick)

    root.after(8, tick)
    root.mainloop()


if __name__ == '__main__':
    main()
