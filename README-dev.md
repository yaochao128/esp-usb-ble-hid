# Development Testing

To build and flash:

```bash
idf.py build
idf.py flash
idf.py monitor
```

If using an ESP32-S3 board:

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

After flashing, your host should detect a **USB HID Keyboard**.

Use a Bluetooth keyboard and trigger the following combos:
- Alt+Tab → expect `SEND F13` in the serial output
- Ctrl+Space → expect `SEND F14`
- Ctrl+Enter → expect `SEND F15`
