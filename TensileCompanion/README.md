# TensileCompanion

Real-time visualization and control application for TensileOS tensile testing equipment.

## Overview

TensileCompanion is a Python-based desktop application that provides real-time force measurement visualization, test control, and data export capabilities for the TensileOS tensile strength testing system. Built with tkinter and matplotlib, it offers an intuitive single-window interface for monitoring tests and managing data.

## Features

### Live Testing
- **Real-Time Visualization**: Live plotting of force vs. time at 2 Hz measurement rate
- **Automatic JSON Mode**: Connects and automatically switches device to JSON output
- **Test Control**: Start new tests, pause, resume, and discard data
- **Peak Force Tracking**: Real-time display of peak force with visual indicator
- **Raw Serial Monitor**: Live display of all incoming serial data in the connection panel
- **Debug Console Toggle**: Optional detailed debugging output to console
- **Customizable Display**: User-configurable plot colors, axis scaling, and grid settings

### Multi-Test Management
- **Metadata Capture**: Required test name and technician for each test
- **Date-Organized Storage**: Tests automatically organized in YYYY-MM-DD folders
- **Test Browser**: Tree view for browsing and selecting historical tests
- **Metadata Preview**: View test details (name, technician, datetime, peak force, notes)
- **Edit Capability**: Modify test metadata after test completion
- **Recent Technicians**: Dropdown with last 10 technicians for quick selection

### Statistical Analysis
- **Multi-Test Selection**: Select multiple tests via checkboxes
- **Statistical Summary**: Mean, standard deviation, 3-sigma range, min/max, median
- **Visual Charts**: Bar chart with individual peaks, mean line, and ±3σ bounds
- **Deviation Analysis**: Shows each test's deviation from mean
- **Export Reports**: Save statistical analysis to CSV with full metadata

### Data Management
- **Auto-Export**: Automatically exports test data to CSV when starting a new test
- **CSV Format**: Timestamp (s), Current Force (kN), Peak Force (kN)
- **Metadata Headers**: Test info stored as # comment lines at top of CSV
- **Persistent Settings**: All preferences saved between sessions
- **Error Handling**: Connection error display with manual reconnect capability

## Requirements

- Python 3.7 or higher
- TensileOS device connected via USB/Serial
- Windows, macOS, or Linux

## Installation

1. **Clone or navigate to the TensileCompanion directory:**
   ```bash
   cd TensileOS/TensileCompanion
   ```

2. **Install dependencies:**
   ```bash
   pip install -r requirements.txt
   ```

   **Note:** tkinter is included with most Python installations. If missing:
   - **Windows/macOS**: Reinstall Python with tkinter option enabled
   - **Linux**: `sudo apt-get install python3-tk`

3. **Verify installation:**
   ```bash
   python main.py
   ```

## Usage

### Starting the Application

```bash
python main.py
```

### Workflow

#### Live Test Tab

1. **Connect to Device**
   - Select your COM port from the dropdown (e.g., COM3, /dev/ttyUSB0)
   - Click "Refresh Ports" if your device isn't listed
   - Click "Connect"
   - Application automatically switches device to JSON mode
   - **Raw Serial Data**: View all incoming serial data in real-time in the "Raw Serial Data" text area
   - **Connection Errors**: Any connection issues are displayed in the red error box

2. **Capture Test Metadata**
   - Click "New Test (Auto-Export)"
   - Enter required metadata:
     - **Test Name**: Descriptive name for the test
     - **Technician**: Name of person conducting test (dropdown shows recent names)
     - **Notes**: Optional test notes or observations
   - Date/time are automatically captured
   - Click OK to begin test (Cancel to abort)

3. **Run a Test**
   - Once metadata is confirmed, measurements begin automatically
   - Monitor real-time force on the live plot
   - Peak force is displayed in the bottom center and as a horizontal line
   - All raw serial communication is visible in the left panel

4. **Debug Mode**
   - Check "Debug to Console" in the left panel to enable detailed console logging
   - Useful for troubleshooting connection or data parsing issues
   - Displays raw serial, JSON detection, and parsed data in the terminal

5. **Start Next Test**
   - Click "New Test (Auto-Export)" to:
     - Automatically save current test with metadata to `Tests/YYYY-MM-DD/testname_HHMMSS.csv`
     - Show metadata dialog for next test
     - Clear plot and reset peak tracking
     - Send new test command to device (resets timestamp)

6. **Discard Test**
   - Click "Discard & New Test" to start fresh without saving current data

7. **Pause/Resume**
   - Use "Pause" and "Resume" buttons to control data collection
   - Data continues to buffer during pause

#### Test Browser Tab

1. **Browse Historical Tests**
   - Switch to "Test Browser" tab
   - Click "Refresh" to load available tests
   - Tests are organized by date folders (YYYY-MM-DD)
   - Expand date folders to see individual tests

2. **View Test Details**
   - Click on any test to view metadata preview:
     - Test name
     - Technician
     - Date/time
     - Peak force
     - Notes

3. **Edit Metadata**
   - Select a test
   - Click "Edit Metadata"
   - Modify test name, technician, or notes
   - Date/time and peak force are read-only
   - Click OK to save changes

4. **Calculate Statistics**
   - Select checkboxes next to 2 or more tests
   - Click "Calculate Statistics"
   - View statistical analysis window with:
     - Summary statistics (mean, std dev, 3-sigma, min/max, median)
     - Bar chart showing individual peaks with mean and ±3σ lines
     - Individual test deviation table
   - Click "Export Report" to save statistics as CSV

### Debugging and Monitoring

The left connection panel provides several monitoring tools:

- **Connection Status**: Shows "Connected" (green) or "Disconnected" (red)
- **Connection Errors**: Red text box displays connection issues and errors
- **Debug to Console**: Checkbox to enable detailed debug output to terminal/console
- **Raw Serial Data**: Scrolling text area showing all incoming serial communication
  - Automatically limited to last 1000 lines
  - Useful for verifying data transmission
  - Shows both JSON and non-JSON data from device

### Customization

#### Plot Colors
- **Current Line**: Color of the real-time force line
- **Peak Line**: Color of the horizontal peak indicator
- **Grid**: Grid line color
- Click color buttons in settings panel to open color picker

#### Axis Scaling
- **Auto-scale**: Automatically adjusts axes to fit data (default)
- **Manual**: Set custom X/Y min/max values
- Changes apply immediately to the plot

#### Export Directory
- Default: `./exports` (created automatically)
- Click "Browse" to select custom directory
- Click "Set Export Dir" to apply

### Data Export Format

CSV files are saved with metadata headers and test data:

```csv
# TensileOS Test Data Export
# test_name: Sample_Test_1
# technician: John Doe
# datetime: 2024-01-15 14:30:45
# notes: First test of the day
# peak_force: 24.567
timestamp_s,current_kN,peak_kN
0.000,0.125,0.125
0.500,0.342,0.342
1.000,1.250,1.250
1.500,2.500,2.500
```

**Metadata Headers (prefixed with #):**
- `test_name`: User-provided test name
- `technician`: Name of person conducting test
- `datetime`: Test date and time (YYYY-MM-DD HH:MM:SS)
- `notes`: Optional test notes
- `peak_force`: Maximum force reached during test (kN)

**Data Columns:**
- `timestamp_s`: Test time in seconds (resets for each new test)
- `current_kN`: Current force reading in kilonewtons
- `peak_kN`: Peak force reading in kilonewtons (cumulative max)

**File Organization:**
- Location: `./Tests/YYYY-MM-DD/`
- Naming: `testname_HHMMSS.csv`
- Example: `./Tests/2024-01-15/Sample_Test_1_143045.csv`

## Configuration

Settings are automatically saved to `config.json` in the application directory. This file stores:

- Plot colors (hex codes)
- Grid settings
- Axis scaling preferences
- Last used COM port
- Export directory (deprecated, now uses Tests directory)
- Tests directory (default: `./Tests`)
- Last technician name (for auto-fill)
- Recent technicians list (last 10)

Click "Restore Defaults" to reset all settings to factory values.

## Troubleshooting

### Connection Issues

**Problem:** Device not listed in COM ports
- **Solution**: Check USB cable connection, click "Refresh Ports"

**Problem:** "Connection Error" when clicking Connect
- **Solution**: 
  - Ensure device is powered and not connected to another application
  - Check that the correct COM port is selected
  - On Linux, verify user has permission: `sudo usermod -a -G dialout $USER` (requires logout)

**Problem:** "Connection Lost" error during test
- **Solution**: Click "Reconnect" button, check cable connection

### Data Issues

**Problem:** No data appearing on plot
- **Solution**: 
  - Verify device is sending data (check Arduino Serial Monitor)
  - Ensure JSON mode is active (app does this automatically)
  - Try disconnecting and reconnecting

**Problem:** Plot updates slowly or freezes
- **Solution**: 
  - Reduce data collection time (each test accumulates unlimited points)
  - Start a new test to clear buffer
  - Close and restart application

### Export Issues

**Problem:** "Failed to save test" error
- **Solution**: Check export directory exists and has write permissions

**Problem:** Can't find exported files
- **Solution**: Check export directory path in settings panel, default is `./exports` relative to application directory

## Application Architecture

```
TensileCompanion/
├── main.py                     # Main GUI application
├── config/
│   ├── settings.py             # Settings management
│   └── config.json             # Persistent configuration
├── utils/
│   └── serial_handler.py       # Serial communication
├── data/
│   └── data_manager.py         # Data buffering and CSV export
├── visualization/
│   └── plotter.py              # Matplotlib plotting engine
├── exports/                    # Default export directory (auto-created)
├── requirements.txt            # Python dependencies
└── README.md                   # This file
```

### Key Components

- **SerialHandler**: Manages connection, auto-enables JSON mode, parses data stream
- **DataManager**: Buffers test data, handles CSV export with timestamps
- **TensilePlotter**: Real-time matplotlib visualization with customizable styling
- **Settings**: Persistent configuration with JSON storage

## Keyboard Shortcuts

Currently none implemented. All functionality is available through GUI buttons.

## Known Limitations

- No plot data point limit (long tests may consume significant memory)
- Single device connection (cannot monitor multiple devices simultaneously)
- No real-time statistics (min, average, standard deviation)
- No data filtering or smoothing options

## Future Enhancements

- Export to additional formats (JSON, Excel)
- Statistical analysis display
- Data filtering and smoothing
- Multiple simultaneous device support
- Keyboard shortcuts
- Plot zoom and pan controls
- Test annotations and notes

## Compatibility

- **TensileOS**: v1.0+ with JSON output support
- **Python**: 3.7+
- **Platforms**: Windows, macOS, Linux

## License

This project is part of TensileOS and is licensed under the MIT License.

## Support

For issues or questions:
- Check TensileOS main README for device-specific troubleshooting
- Review device firmware is current version with JSON support
- Verify Python dependencies are correctly installed

## Author

SE Expedition Medical - Manufacturing Equipment Division

Part of the TensileOS project for tensile strength testing equipment.
