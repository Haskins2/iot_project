# live camera viewer script

import argparse
import io
import sys
import time
import struct

import serial
from PIL import Image
import matplotlib.pyplot as plt
import matplotlib.animation as animation

# CHANGE PORT
DEFAULT_PORT = "/dev/tty.usbserial-1320"  # must be a better way to do this? without having to use --port?
DEFAULT_BAUD = 921600


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ESP-EYE UART camera viewer")
    parser.add_argument("--port", default=DEFAULT_PORT, help="Serial port")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Baud rate")
    return parser.parse_args()


def open_serial(port: str, baud: int) -> serial.Serial:
    """Open the serial port, printing an error and exiting on failure."""
    try:
        return serial.Serial(port, baud, timeout=None)
    except serial.SerialException as e:
        print(f"Error opening {port}: {e}", file=sys.stderr)
        sys.exit(1)


def read_line(ser: serial.Serial) -> str:
    """Read bytes until newline, discarding runs of binary garbage > 64 bytes."""
    buf = b""
    while True:
        ch = ser.read(1)
        if ch == b"\n":
            return buf.decode("ascii", errors="ignore").strip()
        buf += ch
        if len(buf) > 64:
            buf = b""


def read_frame(ser: serial.Serial) -> bytes:
    """Block until a binary frame header arrives, then return the complete JPEG bytes."""
    header_fmt = "<HLHH"  # uint16 magic, uint32 len, uint16 chunk_id, uint16 total_chunks (Little Endian)
    header_size = struct.calcsize(header_fmt)
    magic_byte1 = b"\xca"
    magic_byte2 = b"\xfe"  # 0xFECA total

    while True:
        # Synching logic: read 1 byte at a time until we find the magic 0xFECA (Little endian: 0xca first)
        sync1 = ser.read(1)
        if not sync1:
            continue

        if sync1 == magic_byte1:
            sync2 = ser.read(1)
            if sync2 == magic_byte2:
                # We found magic, now read the rest of the struct (length, chunk_id, total_chunks) -> 8 bytes
                header_data = ser.read(header_size - 2)
                if len(header_data) < header_size - 2:
                    continue  # Timeout / invalid read length

                _, length, chunk_id, total_chunks = struct.unpack(
                    header_fmt, sync1 + sync2 + header_data
                )

                # Check for sane values (basic defense vs false sync)
                if length == 0 or length > 500_000:
                    continue

                # Read the frame body
                data = bytearray(length)
                received = 0
                while received < length:
                    chunk = ser.read(length - received)
                    if not chunk:
                        break  # Serial timeout/error
                    data[received : received + len(chunk)] = chunk
                    received += len(chunk)

                if received < length:
                    continue  # Drop incomplete frame

                return bytes(data)

        elif sync1 == b"C":
            # Just in case we get a string error message: error starts with "CAM_ERR:"
            rest = ser.read(7)
            if rest == b"AM_ERR:":
                # read to newline
                err_msg = bytearray(b"C" + rest)
                while True:
                    ch = ser.read(1)
                    if not ch or ch == b"\n":
                        break
                    err_msg.extend(ch)
                print(
                    f"[ESP32] {err_msg.decode('ascii', errors='ignore').strip()}",
                    file=sys.stderr,
                )


def decode_jpeg(data: bytes) -> Image.Image | None:
    """Decode raw JPEG bytes into a PIL Image, returning None on failure."""
    try:
        return Image.open(io.BytesIO(data))
    except Exception as e:
        print(f"JPEG decode error: {e}", file=sys.stderr)
        return None


class Viewer:
    """Holds all display state for the live camera window."""

    def __init__(self, ser: serial.Serial):
        self.ser = ser
        self.fig, self.ax = plt.subplots(figsize=(6, 5))
        self.fig.suptitle("ESP-EYE Live View  (close window to quit)")
        self.ax.axis("off")
        self.img_display = None
        self._fps_times: list[float] = []

    def _fps(self) -> float:
        now = time.monotonic()
        self._fps_times = [t for t in self._fps_times if now - t < 2.0]
        self._fps_times.append(now)
        return len(self._fps_times) / 2.0

    def _update_display(self, img: Image.Image, byte_count: int) -> None:
        if self.img_display is None:
            self.img_display = self.ax.imshow(img)
        else:
            self.img_display.set_data(img)
        ble_fps_low = (100 * 1024) / byte_count if byte_count else 0
        ble_fps_high = (200 * 1024) / byte_count if byte_count else 0
        self.ax.set_title(
            f"{img.width}x{img.height}  |  {byte_count} bytes  |  {self._fps():.1f} fps"
            f"  |  BLE: {ble_fps_low:.1f}\u2013{ble_fps_high:.1f} fps (100\u2013200 KB/s)",
            fontsize=9,
        )
        self.fig.canvas.draw_idle()

    def update(self, _frame_idx: int) -> None:
        """Called by FuncAnimation on each tick."""
        data = read_frame(self.ser)
        img = decode_jpeg(data)
        if img is not None:
            self._update_display(img, len(data))


def main() -> None:
    args = parse_args()
    print(f"Opening {args.port} at {args.baud} baud...")
    ser = open_serial(args.port, args.baud)
    print("Waiting for first frame...")

    viewer = Viewer(ser)
    _ani = animation.FuncAnimation(
        viewer.fig, viewer.update, interval=10, cache_frame_data=False
    )

    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("Serial port closed.")


if __name__ == "__main__":
    main()
