import argparse
import base64
import time
import os
import sys

import serial

DEFAULT_PORT = "/dev/cu.usbserial-210"
DEFAULT_BAUD = 115200


def parse_args():
    parser = argparse.ArgumentParser(description="ESP32-C6 UART Base64 Image Saver")
    parser.add_argument("--port", default=DEFAULT_PORT, help="Serial port")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Baud rate")
    parser.add_argument("--dir", default=".", help="Directory to save images")
    return parser.parse_args()


def main():
    args = parse_args()
    os.makedirs(args.dir, exist_ok=True)

    print(f"Opening {args.port} at {args.baud} baud...")
    print(f"Listening for Base64 images from ESP32-C6...")
    print("Press Ctrl+C to exit.")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening {args.port}: {e}", file=sys.stderr)
        return

    in_image = False
    b64_data = []
    image_count = 0

    try:
        while True:
            try:
                line = ser.readline().decode("ascii", errors="ignore").strip()
                if not line:
                    continue

                if "[START_IMG]" in line:
                    print("\n[+] Image transmission started...", end="", flush=True)
                    in_image = True
                    b64_data = []
                    continue

                if "[END_IMG]" in line:
                    if in_image:
                        print(" Done.")
                        print("[-] Decoding and saving image...", end="", flush=True)
                        in_image = False

                        try:
                            # Reconstruct Base64 string and decode
                            raw_b64 = (
                                "".join(b64_data)
                                .replace("ESP32_C6: ", "")
                                .replace("ESP_LOG: ", "")
                            )
                            # strip any other log artifacts just in case
                            clean_b64 = "".join(
                                c for c in raw_b64 if c.isalnum() or c in "+/="
                            )

                            img_bytes = base64.b64decode(clean_b64)

                            filename = os.path.join(
                                args.dir, f"flood_photo_{image_count}.jpg"
                            )
                            with open(filename, "wb") as f:
                                f.write(img_bytes)

                            print(f" [✓] Saved as: {filename}")
                            image_count += 1
                        except Exception as e:
                            print(f" [x] Failed to save image: {e}")
                    continue

                if in_image:
                    b64_data.append(line)
                    if len(b64_data) % 50 == 0:
                        print(".", end="", flush=True)
                else:
                    # Print normal logs
                    print(line)

            except Exception as e:
                print(f"Serial read error: {e}")
                time.sleep(1)

    except KeyboardInterrupt:
        print("\nExiting...")

    ser.close()


if __name__ == "__main__":
    main()
