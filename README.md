# TensileOS

Tensile strength testing system using SparkFun OpenScale (HX711-based load cell amplifier) for force measurement and peak load tracking.

## Overview

TensileOS provides a simple serial interface for real-time force measurements in tensile/break testing equipment. The system continuously monitors load cell readings, tracks peak forces, and outputs data in CSV format for logging and analysis.

## Hardware Requirements

- Arduino-compatible microcontroller
- SparkFun OpenScale or HX711 load cell amplifier
- Load cell appropriate for testing range
- **Pin Configuration:**
  - Data Pin: Digital 2
  - Clock Pin: Digital 3

## Features

- **Real-time Force Measurement**: Continuous readings at 2 Hz (500ms intervals)
- **Peak Force Tracking**: Automatically records and maintains maximum reading
- **Multi-Point Calibration**: 10-point calibration curve for accurate measurements across range
- **Interactive Serial Interface**: Simple command-driven control
- **Dual Output Formats**: Toggle between CSV and JSON output modes
  - CSV format for simple logging
  - JSON format with timestamps for data visualization applications
- **Modular Code Architecture**: Clean, maintainable function-based design

## Serial Interface

### Baud Rate
115200 bps

### Data Output Formats

**CSV Format (Default):**
```
current_reading,max_reading
1.234,5.678
2.345,5.678
```

**JSON Format:**
```json
{"timestamp":12345,"current":1.234,"peak":5.678}
{"timestamp":12845,"current":2.345,"peak":5.678}
```

Both formats output forces in kilonewtons (kN) with measurements taken at 2 Hz. JSON format includes millisecond timestamps since device startup for time-series visualization.

### Commands

During normal operation:
- **`x`** - Pause measurements and enter menu mode

In pause menu:
- **`r`** - Resume measurements (keep peak reading)
- **`x`** - Clear peak reading and resume measurements
- **`j`** - Toggle output format between CSV and JSON
- **`c`** - Enter calibration mode *(future feature)*

## Calibration

The system uses 10 calibration points mapping raw HX711 values to known forces. Current calibration:

| Index | Raw Value | Force (kN) |
|-------|-----------|------------|
| 0     | 12697     | 0.000      |
| 1     | 81470     | 0.994      |
| 2     | 420690    | 4.940      |
| 3     | 609031    | 7.362      |
| 4     | 875551    | 10.490     |
| 5     | 1086675   | 12.684     |
| 6     | 1245437   | 14.723     |
| 7     | 1564460   | 18.404     |
| 8     | 1855791   | 21.717     |
| 9     | 2202545   | 25.766     |

### Recalibration

To recalibrate for your specific load cell:

1. Apply known test weights across your measurement range
2. Record the raw HX711 values and corresponding weights
3. Update the `scale.setCalibrate()` calls in `setup()` with your data
4. Rebuild and upload the firmware

## Installation

1. Install [PlatformIO](https://platformio.org/) for your development environment
2. Clone or download this repository
3. Open the project folder in PlatformIO
4. Build and upload to your Arduino:
   ```
   pio run --target upload
   ```

## Dependencies

- **Arduino Framework**
- **HX711_MP Library** - Multi-point calibration library for HX711 load cell amplifier

Dependencies are managed automatically by PlatformIO via `platformio.ini`.

## Usage Example

1. Connect your Arduino to the test equipment
2. Open a serial terminal at 115200 baud
3. The system will output calibration test data on startup (5 second delay)
4. Continuous measurements begin automatically in CSV format:
   ```
   0.125,0.125
   0.342,0.342
   1.250,1.250
   2.500,2.500
   ```
5. Press `x` to pause and view the peak force
6. Press `j` to toggle to JSON mode for data visualization:
   ```json
   {"timestamp":5234,"current":1.250,"peak":2.500}
   ```
7. Use menu options to resume, reset, or change output format

## Example Data Logging

### CSV Format
Using a serial logger or terminal program that supports file output:

```bash
# Example using PlatformIO device monitor with output redirect
pio device monitor > test_data.csv
```

### JSON Format for Python Visualization
Sample Python code to read and process JSON data:

```python
import serial
import json

ser = serial.Serial('COM3', 115200)  # Adjust port as needed

while True:
    line = ser.readline().decode('utf-8').strip()
    if line.startswith('{'):
        data = json.loads(line)
        print(f"Time: {data['timestamp']}ms, Current: {data['current']}kN, Peak: {data['peak']}kN")
        # Add your visualization code here
```

## Project Structure

```
TensileOS/
├── src/
│   └── main.cpp          # Main application code
├── include/              # Header files
├── lib/                  # Project-specific libraries
├── test/                 # Unit tests
├── Examples/             # Example sketches
│   ├── calibrate.cpp     # Calibration utilities
│   └── main.cpp          # Alternative main implementations
├── platformio.ini        # PlatformIO configuration
└── README.md            # This file
```

## Code Architecture

The codebase uses a modular function-based design:

- **`measurementMode()`** - Continuous measurement loop
- **`displayMenu()`** - Pause menu display
- **`handleMenuInput()`** - Menu command processing
- **`outputReading()`** - Output format router
- **`outputCSV()`** - CSV formatting
- **`outputJSON()`** - JSON formatting with timestamps

This modular structure makes the code easy to maintain, test, and extend.

## Future Enhancements

- Interactive calibration mode (command `c`)
- Additional export formats (XML, binary)
- Tare/zero adjustment
- Configurable measurement rates
- EEPROM storage for calibration data
- Real-time graphing capability

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Author

SE Expedition Medical - Manufacturing Equipment Division

## Support

For questions or issues, please contact the SE Expedition Medical manufacturing team.
