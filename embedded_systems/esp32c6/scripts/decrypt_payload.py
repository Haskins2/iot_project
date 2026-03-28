import json
import base64
import os


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    payload_file = os.path.join(script_dir, "payload.json")
    output_image = os.path.join(script_dir, "decoded_image.jpg")

    if not os.path.exists(payload_file):
        print(f"File not found: {payload_file}")
        return

    with open(payload_file, "r") as f:
        try:
            payload = json.load(f)
        except json.JSONDecodeError as e:
            print(f"Error parsing JSON: {e}")
            return

    # Extract sensor data
    water_sensor = payload.get("water_sensor")
    raindrop_sensor = payload.get("raindrop_sensor", {})
    timestamp = payload.get("timestamp_ms")
    image_base64 = payload.get("image_base64")

    print("--- Decoded Payload Data ---")
    print(f"Timestamp (ms): {timestamp}")
    print(f"Water Sensor: {water_sensor}")
    print(
        f"Raindrop Sensor: Digital={raindrop_sensor.get('digital')}, Analog={raindrop_sensor.get('analog')}"
    )
    print("----------------------------")

    # Decode and save the image
    if image_base64:
        try:
            image_data = base64.b64decode(image_base64)
            with open(output_image, "wb") as img_file:
                img_file.write(image_data)
            print(f"Successfully decoded base64 image and saved to {output_image}")
        except Exception as e:
            print(f"Error decoding base64 image: {e}")
    else:
        print("No image_base64 data found in payload.")


if __name__ == "__main__":
    main()
