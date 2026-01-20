"""
Data manager for TensileOS test data
Handles data buffering, CSV export, and test session management
"""

import csv
from datetime import datetime
from pathlib import Path
from typing import List, Tuple


class DataManager:
    """Manages test data collection and export"""
    
    def __init__(self, export_directory: str = "./exports"):
        """Initialize data manager
        
        Args:
            export_directory: Directory for CSV exports
        """
        self.export_directory = Path(export_directory)
        self.timestamps: List[float] = []
        self.current_readings: List[float] = []
        self.peak_readings: List[float] = []
        self.test_start_time = datetime.now()
        
        # Create export directory if it doesn't exist
        self.export_directory.mkdir(parents=True, exist_ok=True)
    
    def add_data_point(self, timestamp: float, current: float, peak: float):
        """Add a data point to current test
        
        Args:
            timestamp: Test timestamp in seconds
            current: Current force reading in kN
            peak: Peak force reading in kN
        """
        self.timestamps.append(timestamp)
        self.current_readings.append(current)
        self.peak_readings.append(peak)
    
    def get_data(self) -> Tuple[List[float], List[float], List[float]]:
        """Get current test data
        
        Returns:
            Tuple of (timestamps, current_readings, peak_readings)
        """
        return (self.timestamps.copy(), 
                self.current_readings.copy(), 
                self.peak_readings.copy())
    
    def get_current_peak(self) -> float:
        """Get the current peak force value
        
        Returns:
            Peak force in kN, or 0 if no data
        """
        return self.peak_readings[-1] if self.peak_readings else 0.0
    
    def has_data(self) -> bool:
        """Check if current test has any data
        
        Returns:
            True if data exists, False otherwise
        """
        return len(self.timestamps) > 0
    
    def save_current_test(self, filename: str = None) -> str:
        """Save current test data to CSV file
        
        Args:
            filename: Optional custom filename. If None, generates timestamp-based name
            
        Returns:
            Full path to saved file
        """
        if not self.has_data():
            raise ValueError("No data to save")
        
        # Generate filename if not provided
        if filename is None:
            timestamp_str = self.test_start_time.strftime("%Y%m%d_%H%M%S")
            filename = f"test_{timestamp_str}.csv"
        
        filepath = self.export_directory / filename
        
        # Write CSV file
        with open(filepath, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['timestamp_s', 'current_kN', 'peak_kN'])
            
            for t, c, p in zip(self.timestamps, self.current_readings, self.peak_readings):
                writer.writerow([f"{t:.3f}", f"{c:.3f}", f"{p:.3f}"])
        
        return str(filepath)
    
    def discard_current_test(self):
        """Discard current test data without saving"""
        self.clear()
    
    def clear(self):
        """Clear all current test data and start fresh"""
        self.timestamps.clear()
        self.current_readings.clear()
        self.peak_readings.clear()
        self.test_start_time = datetime.now()
    
    def set_export_directory(self, directory: str):
        """Set the export directory
        
        Args:
            directory: Path to export directory
        """
        self.export_directory = Path(directory)
        self.export_directory.mkdir(parents=True, exist_ok=True)
    
    def get_data_count(self) -> int:
        """Get number of data points in current test
        
        Returns:
            Number of data points
        """
        return len(self.timestamps)
