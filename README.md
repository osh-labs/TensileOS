# TensileOS

Tensile strength testing system using SparkFun OpenScale (HX711-based load cell amplifier) for force measurement and peak load tracking.

## Overview

TensileOS provides a simple serial interface for real-time force measurements in tensile/break testing equipment. The system continuously monitors load cell readings, tracks peak forces, and outputs data in CSV format for logging and analysis.

The project includes **TensileCompanion**, a comprehensive Python GUI application for real-time data visualization, test management, and statistical analysis.

## Components

### TensileOS (Firmware)
Arduino-based firmware for force measurement and data acquisition.

### TensileCompanion (Desktop Application)
Python GUI application providing:
- Real-time force visualization
- Test data management and organization
- Statistical analysis across multiple tests
- Professional PDF report generation
- Historical test browsing and loading

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
- **Test Timestamp Tracking**: Independent test timer in decimal seconds, reset with each new test
- **Multi-Point Calibration**: 10-point calibration curve for accurate measurements across range
- **Interactive Serial Interface**: Simple command-driven control
- **Dual Output Formats**: Toggle between CSV and JSON output modes
  - CSV format for simple logging
  - JSON format with test-relative timestamps for data visualization applications
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
{"timestamp":12.345,"current":1.234,"peak":5.678}
{"timestamp":12.845,"current":2.345,"peak":5.678}
```

Both formats output forces in kilonewtons (kN) with measurements taken at 2 Hz. JSON format includes test timestamps in decimal seconds (relative to test start) for time-series visualization.

### Commands

During normal operation:
- **`x`** - Pause measurements and enter menu mode

In pause menu:
- **`r`** - Resume measurements (keep peak reading)
- **`x`** - Start new test (resets peak reading and test timestamp)
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
   {"timestamp":5.234,"current":1.250,"peak":2.500}
   ```
7. Press `x` again to start a new test (resets peak and timestamp)
8. Use menu options to resume, start new test, or change output format

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
        print(f"Time: {data['timestamp']:.3f}s, Current: {data['current']}kN, Peak: {data['peak']}kN")
        # Add your visualization code here
```

```

## TensileCompanion GUI Application

### Overview

TensileCompanion is a professional desktop application for controlling TensileOS hardware, visualizing real-time data, and managing test results. Built with Python and Tkinter, it provides a complete solution for tensile testing operations.

### Features

#### Live Testing
- **Real-time Visualization**: Live force vs. time plotting during tests
- **Serial Communication**: Automatic COM port detection and connection
- **Test Metadata**: Capture test name, technician, project, and notes
- **Date-Based Organization**: Automatic file organization by test date
- **CSV Export**: Save test data with embedded metadata headers

#### Test Browser
- **Hierarchical View**: Tests organized by date folders
- **Flexible Selection**: 
  - Select All checkbox in header
  - Click date folders to select/deselect all tests in that date
  - Individual test checkboxes
  - Smart state tracking (folder checkboxes update automatically)
- **Test Loading**: Load historical tests into the main window for review
- **Metadata Editing**: Edit test metadata (name, technician, project, notes) after saving
- **Quick Preview**: View test details in preview pane

#### Statistical Analysis
- **Multi-Test Analysis**: Calculate statistics across selected tests
- **Comprehensive Metrics**:
  - Number of tests
  - Average, median, min/max peak forces
  - Standard deviation
  - 3-Sigma range (mean ± 3σ)
  - 3-Sigma MBS (Minimum Breaking Strength)
- **Visualization**: Bar chart with mean and 3-sigma bounds
- **Chronological Ordering**: Results sorted by test timestamp
- **Individual Deviations**: View each test's deviation from mean

#### PDF Report Generation
- **Professional Layout**: Clean, compact design with optimized spacing
- **Text Wrapping**: Automatically wraps long project names
- **Center-Aligned Tables**: Clean presentation
- **Optimized Pagination**: Minimum 5 test results per page
- **Complete Statistics**: All metrics and visualizations included
- **Company Branding**: Includes company name and software version

#### Settings
- **Plot Customization**: Configure colors, grid, and axis ranges
- **Directory Management**: Set export and test storage locations
- **Company Information**: Set company name for reports
- **Technician History**: Recently used technician names

### Installation

#### Requirements
- Python 3.8 or higher
- Windows, macOS, or Linux

#### Dependencies
Install required packages:
```bash
cd TensileCompanion
python -m pip install -r requirements.txt
```

Required packages:
- `pyserial` - Serial communication
- `matplotlib` - Data visualization
- `reportlab` - PDF generation

#### Running TensileCompanion
```bash
cd TensileCompanion
python main.py
```

### Usage Guide

#### Starting a Test

1. **Connect Hardware**
   - Connect TensileOS device via USB
   - Launch TensileCompanion
   
2. **Configure Serial Connection**
   - Select COM port from dropdown
   - Click "Connect"
   - Status will show "Connected"

3. **Enter Test Metadata**
   - Click "Enter Metadata" button
   - Fill in:
     - Test Name (required)
     - Date/Time (auto-populated)
     - Technician name
     - Project name
     - Notes (optional)
   - Click "Start Test"

4. **Run Test**
   - Watch live force plot
   - Click "Stop Test" when complete
   - Review peak force
   - Click "Save Test" to store data

5. **Test Saved**
   - Automatically organized in date folder
   - CSV file includes metadata header
   - Ready for analysis

#### Loading Historical Tests

1. Go to **Test Browser** tab
2. Click on any test row (not the checkbox)
3. Click **"Load Test"** button
4. Test appears in Live Test tab with:
   - Complete force vs. time graph
   - Peak force line
   - All metadata displayed
5. Review historical test data

#### Selecting Multiple Tests

**Select All Tests:**
- Click "☐ Select All" in the header
- All tests across all dates selected
- Click again to deselect all

**Select Date Folder:**
- Click checkbox next to date (e.g., "☐ 2026-01-20")
- All tests in that date selected
- Date folder shows "☑" when all tests selected

**Select Individual Tests:**
- Click checkbox next to specific tests
- Parent folder and "Select All" update automatically
- Selection count shown in toolbar

#### Statistical Analysis

1. Go to **Test Browser** tab
2. Select tests using checkboxes:
   - Click individual test checkboxes, or
   - Click date folder checkbox to select all in folder, or
   - Click "Select All" header to select everything
3. Click **"Calculate Statistics"** button
4. Review statistics window:
   - Summary metrics
   - Peak force distribution chart
   - Individual test results table (sorted by timestamp)
5. Click **"Export Report"** to generate PDF

#### Editing Metadata

1. Go to **Test Browser** tab
2. Select a test (click on the row, not checkbox)
3. Click **"Edit Metadata"** button
4. Modify fields as needed
5. Click "Save" to update

### File Organization

Tests are automatically organized in date-based folders:

```
Tests/
├── 2026-01-20/
│   ├── Test_1_210645.csv
│   ├── Test_2_211047.csv
│   └── Test_3_211234.csv
├── 2026-01-21/
│   └── Sample_Test_140523.csv
└── ...
```

Each CSV file includes:
- Metadata header (test name, date/time, technician, project, notes, peak force)
- Tabular data (timestamp, current_kN, peak_kN)

### PDF Report Features

Statistical reports include:
- Company name and software version
- Generation timestamp
- Summary statistics table (single-spaced, compact)
- Peak force distribution chart
- Individual test results table with:
  - Test names, projects, technicians
  - Peak forces and deviations from mean
  - Automatic text wrapping for long names
  - Center-aligned columns
  - Minimum 5 rows per page

### Configuration

Settings are stored in `config.json`:
- Plot colors and appearance
- Directory paths
- Last used COM port
- Recent technician names
- Company information
- Software version

## Project Structure

```
TensileOS/
├── src/
│   └── main.cpp              # Arduino firmware
├── include/                  # Header files
├── lib/                      # Project libraries
├── Examples/                 # Example sketches
│   ├── calibrate.cpp
│   └── main.cpp
├── TensileCompanion/         # Python GUI application
│   ├── main.py              # Application entry point
│   ├── config.json          # User settings
│   ├── requirements.txt     # Python dependencies
│   ├── analysis/            # Statistical analysis
│   │   └── statistics.py
│   ├── config/              # Settings management
│   │   └── settings.py
│   ├── data/                # Data management
│   │   ├── data_manager.py
│   │   └── test_manager.py
│   ├── ui/                  # User interface components
│   │   ├── metadata_dialog.py
│   │   ├── metadata_edit_dialog.py
│   │   ├── statistics_window.py
│   │   └── test_browser.py
│   ├── utils/               # Utilities
│   │   └── serial_handler.py
│   ├── visualization/       # Plotting
│   │   └── plotter.py
│   ├── exports/            # CSV exports (legacy)
│   └── Tests/              # Organized test files
│       ├── 2026-01-20/
│       ├── 2026-01-21/
│       └── ...
├── platformio.ini           # PlatformIO configuration
└── README.md               # This file
```

## TensileCompanion Architecture

### Components

- **Serial Handler**: Manages USB communication with TensileOS hardware
- **Data Manager**: Buffers and manages live test data
- **Test Manager**: Organizes tests in date-based folders, handles metadata
- **Plotter**: Real-time and historical data visualization
- **Statistics Module**: Multi-test statistical analysis
- **UI Components**: Dialogs for metadata entry/editing
- **Test Browser**: Hierarchical test selection and management

### Data Flow

1. Arduino sends JSON data via serial
2. Serial Handler parses and forwards to Data Manager
3. Data Manager buffers readings and updates Plotter
4. User saves test → Test Manager creates CSV with metadata
5. Test Browser reads and displays saved tests
6. Statistics Module analyzes selected tests
7. PDF reports generated from statistical analysis

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

### Firmware
- Interactive calibration mode (command `c`)
- Additional export formats (XML, binary)
- Tare/zero adjustment
- Configurable measurement rates
- EEPROM storage for calibration data

### TensileCompanion
- Export to Excel format
- Custom report templates
- Multi-language support
- Cloud backup integration
- Advanced filtering and search
- Batch test operations

## Version History

### v1.5.2 (January 2026)
- Enhanced statistics PDF export with text wrapping and compact layout
- Improved test browser with hierarchical selection (Select All, date folders, individual tests)
- Added ability to load historical tests into main window
- Optimized PDF pagination (minimum 5 rows per page)
- Fixed statistics window positioning bug
- Chronological sorting by test timestamp
- Renamed "3-Sigma Average Peak Force" to "3-Sigma MBS"

### v1.5.1
- Initial TensileCompanion release
- Real-time visualization
- Test metadata management
- Statistical analysis
- Basic PDF reports

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Author

SE Expedition Medical - Manufacturing Equipment Division

## Support

For questions or issues, please contact the SE Expedition Medical manufacturing team.
