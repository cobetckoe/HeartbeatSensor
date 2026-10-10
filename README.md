# ESP32 Radar Tracker with HLK-LD2450

Real-time radar tracking system using the HLK-LD2450 24GHz mmWave radar sensor with TFT radar display visualization. Firmware is a unified PlatformIO project (ESP32-S3 and ESP32-C3 targets), alongside multiple CircuitPython implementations.

## 🎯 Features

- **Multi-Target Tracking**: Simultaneously tracks up to 3 targets with X/Y coordinates
- **Real-time Visualization**: OLED display with animated radar sweep and target positions
- **Dual Platform Support**: 
  - PlatformIO C++ (Arduino framework) with optimized performance
  - CircuitPython with multiple visualization modes
- **Multiple Visualization Modes**:
  - Classic radar sweep display
  - Alien-style animated tracking
  - Diagnostic data output
  - Raw protocol debugging

## 📡 Hardware Requirements

- **ESP32-S3** (tested on ProS3 board) or **ESP32-C3** (SuperMini)
- **HLK-LD2450** 24GHz mmWave Radar Sensor
- **1.8" TFT ST7735** display, 160x128, SPI
- USB-C cable for programming and power

### Wiring Diagram

**Radar to ESP32-S3 (default pins):**
- Radar TX -> ESP32 GPIO43 (RX)
- Radar RX -> ESP32 GPIO44 (TX)
- VCC -> 5V, GND -> GND

**Radar to ESP32-C3 (default pins):**
- Radar TX -> ESP32 GPIO4 (RX)
- Radar RX -> ESP32 GPIO5 (TX)
- VCC -> 5V, GND -> GND

**TFT to ESP32-S3 (SuperMini):**
- CS=GPIO1, DC=GPIO2, RST=GPIO3, MOSI=GPIO6, SCLK=GPIO7, BLK=GPIO8
- VCC=3.3V, GND=GND

**TFT to ESP32-C3 (SuperMini):**
- CS=GPIO1, DC=GPIO2, RST=GPIO3, MOSI=GPIO7, SCLK=GPIO6, BLK=GPIO10
- VCC=3.3V, GND=GND

Radar UART and baud are auto-detected on first boot (both TX/RX orders tried), then locked in NVS.


### PlatformIO Setup (recommended)

The firmware is a unified PlatformIO project. One codebase (src/main.cpp) serves both boards:

| Environment | Board | Radar UART | TFT pins |
|---|---|---|---|
| esp32s3 | ESP32-S3 DevKitC-1 | Serial0 GPIO43/44 (auto-detect) | CS=1 DC=2 RST=3 MOSI=6 SCLK=7 BLK=8 |
| esp32c3 | ESP32-C3 DevKitM-1 | Serial1 GPIO4(RX)/GPIO5(TX) | CS=1 DC=2 RST=3 MOSI=7 SCLK=6 BLK=10 |

Build / upload / monitor:

    pio run -e esp32s3                # build S3 firmware (or: -e esp32c3)
    pio run -e esp32s3 -t upload      # flash over USB serial
    pio device monitor -e esp32s3     # 115200 baud serial monitor

Libraries (Adafruit GFX, ST7735/ST7789, BusIO) are resolved automatically from platformio.ini.
On first boot the firmware auto-detects the radar UART and baud, then caches it in NVS (Preferences).

### CircuitPython Setup

1. Install CircuitPython 9.x on your ESP32-S3
2. Install required libraries on device:
   - adafruit_displayio_ssd1306
   - adafruit_display_text
3. Choose a visualization mode from `circuitpython/` folder
4. Deploy using the utility tool:

```powershell
python utils/deploy_circuitpython.py
```

Available CircuitPython modes:
- `code_correct_protocol.py` - Standard radar sweep display
- `code_alien_style.py` - Sci-fi animated tracking
- `code_dual_both.py` - Dual protocol support
- `code_diagnostic.py` - Debug output
- `code_realtime_sweep.py` - Fast refresh radar sweep

## 🛠️ Utilities

### Serial Monitor
Monitor ESP32 serial output in real-time:
```powershell
python utils/serial_monitor.py
```

### Code Deployment
Deploy CircuitPython code to ESP32:
```powershell
python utils/deploy_circuitpython.py
```

See [utils/README.md](utils/README.md) for detailed utility documentation.

## 📊 HLK-LD2450 Sensor Details

- **Detection Range**: 0-6 meters
- **Angle Coverage**: ±60°
- **Maximum Targets**: 3 simultaneous
- **Protocol**: UART (115200 baud)
- **Update Rate**: ~10 Hz
- **Technology**: 24GHz FMCW radar

## 🎨 Visualization Modes

### PlatformIO Version (firmware)

- Full-screen semicircle radar with heartbeat ping rings
- Up to 3 simultaneous targets with distance and angle tracking
- ECG heartbeat trace, NATO phosphor-green palette
- 160x128 TFT (ST7735) with flicker-free double buffering
- EEPROM-locked UART config (never re-scans after first detection)


**Standard Mode** (`code_correct_protocol.py`)
- Classic radar sweep animation
- Target position markers
- Distance labels

**Alien Style** (`code_alien_style.py`)
- Animated target tracking
- Fade effects
- Sci-fi aesthetic

**Diagnostic Mode** (`code_diagnostic.py`)
- Raw data output
- Protocol debugging
- Sensor status monitoring

## 🔧 Troubleshooting

### No Radar Data
1. Check UART wiring (TX/RX)
2. Verify sensor power (5V)
3. Run diagnostic mode to check protocol

### Display Not Working
1. Verify I2C address (0x3C default)
2. Check SDA/SCL wiring
3. Run `i2c_scan.py` to detect display

### COM Port Issues
- Windows: Check Device Manager for COM port
- Linux/Mac: Use `ls /dev/tty*` to find port
- Ensure no other program is using the port

## 📁 Project Structure

```
esp32-radar-tracker/
├── platformio.ini          # PlatformIO project (envs: esp32s3, esp32c3)
├── src/
│   └── main.cpp            # Unified firmware (S3/C3 via build flags)
├── include/
│   ├── HLK_LD2450.h        # Radar protocol parser (header-only)
│   └── RadarDisplay.h      # Semicircle sweep + ECG renderer (header-only)
├── circuitpython/          # Multiple CircuitPython versions
│   ├── code_correct_protocol.py
│   ├── code_alien_style.py
│   ├── code_diagnostic.py
│   └── ...                 # Various visualization modes
├── utils/                  # Deployment and monitoring tools
│   ├── deploy_circuitpython.py
│   ├── serial_monitor.py
│   └── README.md
├── requirements.txt        # Python dependencies
└── README.md
```

## 🤝 Contributing

Contributions welcome! Areas for improvement:
- Additional visualization modes
- Enhanced target tracking algorithms
- Multi-sensor support
- Data logging features

## 📝 License

MIT License - see [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- **Adafruit**: Display libraries and CircuitPython support
- **Hi-Link**: HLK-LD2450 sensor and protocol documentation
- **ESP32 Community**: Hardware support and examples

## 📚 Resources

- [HLK-LD2450 Datasheet](https://www.hlktech.net/index.php?id=1163)
- [CircuitPython Documentation](https://circuitpython.org/)
- [ESP32-S3 Technical Reference](https://www.espressif.com/en/products/socs/esp32-s3)
- [Adafruit SSD1306 Guide](https://learn.adafruit.com/monochrome-oled-breakouts)

---

**Status**: ✅ Fully functional - tested with ESP32-S3 ProS3 and HLK-LD2450
