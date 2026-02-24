### Commands to run:

`idf.py set-target esp32` (esp32s2 doesn't seem to work?)

Build: `idf.py build`

Flash: `idf.py -p /dev/tty.usbserial-210 flash` (your port will be different)

Monitor: `idf.py -p /dev/tty.usbserial-210 monitor`

Both: `idf.py -p /dev/tty.usbserial-210 flash monitor`

---

### Bleutooth setup

idf.py menuconfig

component config -> bluetooth (enable)
