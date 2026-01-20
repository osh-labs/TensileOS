"""
Serial communication handler for TensileOS device
Manages connection, data parsing, and command sending
"""

import serial
import serial.tools.list_ports
import json
import threading
import time
from typing import Optional, Callable, Tuple


class SerialHandler:
    """Handles serial communication with TensileOS device"""
    
    def __init__(self, data_callback: Optional[Callable] = None, 
                 error_callback: Optional[Callable] = None,
                 raw_data_callback: Optional[Callable] = None):
        """Initialize serial handler
        
        Args:
            data_callback: Function to call with parsed data (timestamp, current, peak)
            error_callback: Function to call on connection errors
            raw_data_callback: Function to call with raw serial line data
        """
        self.serial_port: Optional[serial.Serial] = None
        self.data_callback = data_callback
        self.error_callback = error_callback
        self.raw_data_callback = raw_data_callback
        self.connected = False
        self.reading_thread: Optional[threading.Thread] = None
        self.stop_reading = False
        self.debug_enabled = False
        self.device_paused = True  # Device starts in pause mode
        
    @staticmethod
    def list_ports():
        """List available COM ports
        
        Returns:
            List of tuples (port, description)
        """
        ports = serial.tools.list_ports.comports()
        return [(port.device, port.description) for port in ports]
    
    def connect(self, port: str, baudrate: int = 115200) -> bool:
        """Connect to TensileOS device
        
        Args:
            port: COM port name (e.g., 'COM3')
            baudrate: Baud rate (default 115200)
            
        Returns:
            True if connection successful, False otherwise
        """
        try:
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baudrate,
                timeout=1
            )
            
            # Wait for device to initialize (calibration output during setup)
            time.sleep(3)
            
            # Clear calibration output and any startup data
            self.serial_port.reset_input_buffer()
            
            # Device starts paused - send 'j' to switch to JSON mode
            self.send_command('j')
            time.sleep(0.8)  # Increased wait for mode switch
            
            # After 'j', device exits menu and starts running (pause_mode = false)
            # So we need to pause it again with 'x'
            self.send_command('x')
            time.sleep(0.5)  # Increased wait for pause
            
            # Now device is paused in JSON mode
            self.device_paused = True
            
            # Clear any menu text from buffer
            self.serial_port.reset_input_buffer()
            
            # Start reading thread
            self.connected = True
            self.stop_reading = False
            self.reading_thread = threading.Thread(target=self._read_loop, daemon=True)
            self.reading_thread.start()
            
            return True
            
        except Exception as e:
            if self.error_callback:
                self.error_callback(f"Connection error: {str(e)}")
            return False
    
    def disconnect(self):
        """Disconnect from device"""
        self.connected = False
        self.stop_reading = True
        
        if self.reading_thread:
            self.reading_thread.join(timeout=2)
        
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        
        self.serial_port = None
    
    def _read_loop(self):
        """Background thread for reading serial data"""
        while not self.stop_reading and self.connected:
            try:
                if self.serial_port and self.serial_port.is_open:
                    line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                    
                    # Send raw data to GUI callback
                    if line and self.raw_data_callback:
                        self.raw_data_callback(line)
                    
                    # Debug: Print all received data to console
                    if line and self.debug_enabled:
                        print(f"RAW SERIAL: {line}")
                    
                    # Skip empty lines and calibration output (non-JSON)
                    if line and line.startswith('{'):
                        if self.debug_enabled:
                            print(f"JSON DETECTED: {line}")
                        data = self._parse_json(line)
                        if data:
                            if self.debug_enabled:
                                print(f"PARSED DATA: {data}")
                            if self.data_callback:
                                self.data_callback(*data)
                        elif self.debug_enabled:
                            print("JSON PARSE FAILED")
                else:
                    time.sleep(0.1)
                            
            except serial.SerialException as e:
                self.connected = False
                if self.error_callback:
                    self.error_callback(f"Serial connection lost: {str(e)}")
                break
            except Exception as e:
                if self.error_callback:
                    self.error_callback(f"Read error: {str(e)}")
                time.sleep(0.1)
    
    def _parse_json(self, line: str) -> Optional[Tuple[float, float, float]]:
        """Parse JSON data line
        
        Args:
            line: JSON string from device
            
        Returns:
            Tuple of (timestamp, current, peak) or None if parse failed
        """
        try:
            data = json.loads(line)
            timestamp = data.get('timestamp', 0)
            current = data.get('current', 0)
            peak = data.get('peak', 0)
            return (timestamp, current, peak)
        except Exception:
            return None
    
    def send_command(self, command: str):
        """Send command to device
        
        Args:
            command: Single character command ('x', 'r', 'j', 'c')
        """
        if self.serial_port and self.serial_port.is_open:
            try:
                self.serial_port.write(command.encode())
            except Exception as e:
                if self.error_callback:
                    self.error_callback(f"Command send error: {str(e)}")
    
    def send_start_new_test(self):
        """Send command to start a new test (resets peak and timestamp)"""
        if not self.device_paused:
            # If device is running, pause it first
            self.send_command('x')
            time.sleep(0.3)
            self.device_paused = True
        
        # Now device is paused, send 'x' to start new test (resets peak and timestamp)
        self.send_command('x')
        time.sleep(0.2)
        self.device_paused = False
    
    def send_pause(self):
        """Send command to pause measurements"""
        if not self.device_paused:
            self.send_command('x')
            self.device_paused = True
    
    def send_resume(self):
        """Send command to resume measurements"""
        if self.device_paused:
            self.send_command('r')
            self.device_paused = False
    
    def is_connected(self) -> bool:
        """Check if connected to device
        
        Returns:
            True if connected, False otherwise
        """
        return self.connected and self.serial_port is not None and self.serial_port.is_open
    
    def set_debug(self, enabled: bool):
        """Enable or disable debug output to console
        
        Args:
            enabled: True to enable debug output, False to disable
        """
        self.debug_enabled = enabled
