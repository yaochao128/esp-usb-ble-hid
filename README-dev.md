# Development Testing

To build and flash:

```bash
idf.py build
idf.py flash
idf.py monitor
```

Use a Bluetooth keyboard and trigger the following combos:
- Alt+Tab → expect `SEND F13` in the serial output
- Ctrl+Space → expect `SEND F14`
- Ctrl+Enter → expect `SEND F15`
