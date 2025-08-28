# ESP32-XBee (MAVTech fork)

Firmware for **ESP32 in XBee form factor** modules (e.g., ArduSimple ESP32 XBee). 
This fork includes modifications for **robust UART → TCP forwarding**, optimized for real-time streaming (e.g., RTCM corrections for GNSS).

Based on [esp32-xbee by nebkat](https://github.com/nebkat/esp32-xbee). 
License: GPLv3.

---

##  Requirements

- [ESP-IDF v4.4](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/get-started/index.html) installed
- Python 3
- A **USB-C data cable**
- Access to serial port `/dev/ttyUSB0` (add your user to the `dialout` group):
  ```bash
  sudo usermod -aG dialout $USER
  # log out and log back in
  ```

---

## ESP-IDF 4.4 Installation (Linux)

1. Clone ESP-IDF v4.4:
   ```bash
   mkdir -p ~/esp
   cd ~/esp
   git clone -b v4.4 --recursive https://github.com/espressif/esp-idf.git
   ```

2. Install prerequisites:
   ```bash
   cd ~/esp/esp-idf
   ./install.sh esp32
   ```

3. Export environment (add this line to `~/.bashrc` if you want it permanent):
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```

Check version:
```bash
idf.py --version
```
Expected: **v4.4.x**

---

##  Build

Load ESP-IDF environment:
```bash
. $HOME/esp-idf/export.sh
```

Build the project:
```bash
cd ~/esp32-xbee
idf.py set-target esp32
idf.py build
```

---

## Menuconfig Fixes

The following `menuconfig` changes were applied and are required:

1. **Serial flasher config → Flash size → 4 MB**
2. **Partition Table → Custom partition table CSV → partitions.csv**
3. **Component config → LWIP → Enable NAT (IP_NAPT)**

Run menuconfig with:
```bash
idf.py menuconfig
```

---

##  Flashing

### Method 1 — Autoreset (if supported)

If the module supports RTS/DTR:
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

> Exit the monitor with `Ctrl+]`.

---

### Method 2 — Manual bootloader (recommended for XBee modules)

1. **Put ESP32 into bootloader mode**
   - Hold down **BOOT (IO0)**
   - Press and release **RESET/EN**
   - Release **BOOT** after ~1s 
   *(or jumper IO0 → GND and power cycle/reset)*

2. **Flash with esptool**
   ```bash
   cd build
   python $IDF_PATH/components/esptool_py/esptool/esptool.py      --chip esp32 -p /dev/ttyUSB0 -b 115200      --before no_reset --after no_reset      write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB      0x1000  bootloader/bootloader.bin      0x8000  partition_table/partition-table.bin      0x10000 esp32-xbee.bin      0x210000 www.bin
   ```

3. **Run the firmware**
   - Press **RESET**
   - Open the serial monitor:
     ```bash
     idf.py -p /dev/ttyUSB0 monitor
     ```

---

## Troubleshooting

- Error: `Failed to connect to ESP32` 
  → try the **manual bootloader method**.

- No output in the monitor 
  → make sure you pressed **RESET** and you are using the correct baud (default 115200).

- If the GNSS receiver UART disturbs flashing, temporarily disable its output to the XBee slot.

---

##  License

Distributed under GPLv3. See the [LICENSE](LICENSE) file.
