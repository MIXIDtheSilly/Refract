"""Stay connected to the Refract TCP image stream and save a left-eye PNG periodically.

Unlike capture.py (one frame, then disconnect), this keeps the Android side's
send path connected, so the game is not stalled by reconnect attempts.
"""
import argparse
import socket
import struct
import time
from pathlib import Path

from capture import read_exact, write_png


def serve(args):
    args.out.mkdir(parents=True, exist_ok=True)
    saved = 0
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        next_save = time.monotonic() + args.first
        received = 0
        while saved < args.count:
            connection, _ = server.accept()
            print('connected', flush=True)
            with connection:
                connection.settimeout(30)
                try:
                    while saved < args.count:
                        header = read_exact(connection, 64)
                        magic, version, kind, size, width, height, layers, fmt, bpp, _ = struct.unpack_from('<IHH7I', header)
                        sequence, _, payload_size = struct.unpack_from('<QQQ', header, 40)
                        if magic != 0x49585241 or payload_size > 256 * 1024 * 1024:
                            raise ValueError(f'bad header magic={magic:#x} size={payload_size}')
                        if size > 64:
                            read_exact(connection, size - 64)
                        payload = read_exact(connection, payload_size)
                        received += 1
                        if time.monotonic() < next_save or fmt != 1 or bpp != 4:
                            continue
                        eye = payload[:width * height * 4]
                        lit = sum(1 for i in range(0, len(eye), 4 * 97) if eye[i] | eye[i + 1] | eye[i + 2])
                        total = len(eye) // (4 * 97)
                        path = args.out / f'frame-{saved:03d}-seq{sequence}.png'
                        write_png(path, width, height, eye)
                        write_png(args.out / 'latest.png', width, height, eye)
                        if version == 4 and layers >= 2:
                            # Quad frames carry panel 2 in the second image slot.
                            write_png(path.with_name(path.stem + '-panel2.png'), width, height,
                                      payload[width * height * 4:width * height * 8])
                        print(f'saved {path.name} v{version} {width}x{height} layers={layers} '
                              f'lit={100 * lit / max(total, 1):.1f}% received={received}', flush=True)
                        saved += 1
                        next_save = time.monotonic() + args.every
                except (EOFError, ConnectionError, socket.timeout) as error:
                    print(f'disconnected: {error}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', type=Path)
    parser.add_argument('--port', type=int, default=38491)
    parser.add_argument('--first', type=float, default=5.0, help='seconds before the first save')
    parser.add_argument('--every', type=float, default=5.0, help='seconds between saves')
    parser.add_argument('--count', type=int, default=24)
    serve(parser.parse_args())
