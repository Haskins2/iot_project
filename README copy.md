# Setup guide

first get into the ESP-IDF terminal
set the correct chip target using `idf.py set-target esp32c6`.

Theres also a project configuration menu (`idf.py menuconfig`).
Not really sure what this does yet

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project.
for me the port is `/dev/cu.usbserial-210` (macos)

(To exit the serial monitor, type `Ctrl-]`.)
