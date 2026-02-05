## Hardware Components

ESP model nos & datasheets:

- `esp32-c6-wroom-1`
  - https://documentation.espressif.com/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf
- `esp-eye`
  - https://github.com/espressif/esp-who/blob/master/docs/en/get-started/ESP-EYE_Getting_Started_Guide.md
  - this is basically an esp32 just with a 2mp camera
  - it has no GPIO pins so cant be used to actuate anything.
  - so use this for cloud communication?

Pump

- `(unknown)`

Servos:

- `DIYMORE-DM996 (x2)`
  - https://www.handsontec.com/dataspecs/motor_fan/MG996R.pdf

Water sensors:

- `(unknown)`

---

# Setup Guide

install the ESP-IDF vscode extension
should be prompted to install the ESP-IDF installation manager

good set up instructions: https://github.com/espressif/vscode-esp-idf-extension/blob/master/README.md

---

# Project specific setup

first get into the ESP-IDF terminal
set the correct chip target using `idf.py set-target esp32c6`.

Theres also a project configuration menu (`idf.py menuconfig`).
Not really sure what this does yet, think we edit the Kconfig file for interactive configuration.

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project.
for me the port is `/dev/cu.usbserial-210` (macos)

(To exit the serial monitor, type `Ctrl-]`.)

---

# Wiring Setup

Water Sensor:

- red/black to 3.3v/ground
- yellow signal pin to GPIO 4

Servo:

- red/brown to 5v/ground
- orange (PWM) to GPIO 21
