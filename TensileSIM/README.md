# TensileSIM - TensileOS Simulator

Simulation version of TensileOS for testing the TensileCompanion application without physical hardware.

## Overview

TensileSIM provides a software-only version of TensileOS that simulates realistic force measurements. This allows development and testing of the TensileCompanion visualization application without requiring the actual tensile testing hardware.

## Simulation Behavior

When a test is started, the simulator follows this pattern:

1. **Ramp Up** (20 seconds): Force gradually increases from 0 to 24 kN
2. **Hold** (3 seconds): Force holds steady at 24 kN
3. **Ramp Down** (2 seconds): Force drops back to zero
4. **Repeat**: Cycle automatically repeats

Random noise (±0.05 kN) is added to simulate real sensor readings.

## Features

- **No Hardware Required**: Runs on any Arduino or computer serial connection
- **Identical Interface**: Same serial commands and output format as TensileOS
- **Realistic Data**: Smooth curves with noise for testing visualization
- **JSON/CSV Modes**: Toggle between output formats just like the real system
- **Peak Tracking**: Maintains peak values within each test

## Installation

### Option 1: Arduino IDE

1. Open `src/main.cpp` in Arduino IDE
2. Select any Arduino board (e.g., Uno, Mega)
3. Upload to Arduino or use a serial port emulator

### Option 2: PlatformIO

```bash
cd TensileSIM
pio run --target upload
```

### Option 3: Serial Port Emulator (No Hardware)

Use tools like `com0com` (Windows) or `socat` (Linux/Mac) to create virtual serial ports for pure software testing.

## Usage

### Running the Simulator

1. Connect Arduino or open serial port emulator
2. Open serial monitor at 115200 baud
3. Simulator starts automatically

### Testing with TensileCompanion

1. Start the simulator on a COM port
2. Launch TensileCompanion
3. Connect to the simulator's COM port
4. Watch simulated force readings plot in real-time
5. Test all TensileCompanion features:
   - New test (resets cycle)
   - Auto-export
   - Pause/resume
   - Plot customization

## Serial Commands

Same as TensileOS:

- **`x`** - Pause / Start new test
- **`r`** - Resume measurements
- **`j`** - Toggle JSON/CSV output
- **`c`** - (Not implemented)

## Simulation Parameters

Modify these constants in `main.cpp` to change behavior:

```cpp
const float MAX_FORCE = 24.0;           // Peak force in kN
const float RAMP_UP_TIME = 20000;       // Time to reach peak (ms)
const float HOLD_TIME = 3000;           // Time at peak (ms)
const float RAMP_DOWN_TIME = 2000;      // Time to drop to zero (ms)
```

## Differences from TensileOS

- **No HX711 Library**: Removed hardware dependencies
- **Simulated Readings**: Mathematical curve instead of load cell
- **Automatic Cycling**: Test repeats continuously
- **No Calibration**: Not needed for simulation

## Use Cases

- **Application Development**: Test TensileCompanion without hardware
- **UI Testing**: Verify plot rendering, colors, scaling
- **Export Testing**: Validate CSV export with known data pattern
- **Demo Mode**: Show TensileOS capabilities without physical setup
- **Training**: Learn the interface before using real equipment

## Data Output

### CSV Format
```
0.000,0.000
0.600,0.600
1.200,1.200
...
24.000,24.000
```

### JSON Format
```json
{"timestamp":0.000,"current":0.000,"peak":0.000}
{"timestamp":0.500,"current":0.600,"peak":0.600}
{"timestamp":1.000,"current":1.200,"peak":1.200}
...
{"timestamp":20.000,"current":24.000,"peak":24.000}
```

## Known Limitations

- Not suitable for validating load cell calibration
- Simplified noise model (real sensors have more complex behavior)
- Fixed cycle pattern (doesn't respond to physical events)

## License

MIT License - Same as TensileOS

## Author

SE Expedition Medical - Manufacturing Equipment Division

Part of the TensileOS project ecosystem.
