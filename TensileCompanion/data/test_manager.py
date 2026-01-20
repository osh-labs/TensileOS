"""
Test Manager for organizing and managing test files with metadata
Handles date-based folder organization and metadata storage
"""

import os
import csv
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Optional, Tuple
import re


class TestManager:
    """Manages test file organization and metadata"""
    
    def __init__(self, tests_directory: str = "./Tests"):
        """Initialize test manager
        
        Args:
            tests_directory: Base directory for test storage
        """
        self.tests_directory = Path(tests_directory)
        self.tests_directory.mkdir(parents=True, exist_ok=True)
    
    def generate_test_filepath(self, test_name: str, test_datetime: str = None) -> Path:
        """Generate filepath for a new test
        
        Args:
            test_name: Name of the test
            test_datetime: DateTime string (YYYY-MM-DD HH:MM:SS), uses now() if None
            
        Returns:
            Path object for the test file
        """
        if test_datetime is None:
            dt = datetime.now()
        else:
            dt = datetime.strptime(test_datetime, "%Y-%m-%d %H:%M:%S")
        
        # Create date folder (YYYY-MM-DD)
        date_folder = self.tests_directory / dt.strftime("%Y-%m-%d")
        date_folder.mkdir(parents=True, exist_ok=True)
        
        # Sanitize test name for filename
        safe_name = self._sanitize_filename(test_name)
        
        # Generate filename with timestamp
        filename = f"{safe_name}_{dt.strftime('%H%M%S')}.csv"
        
        return date_folder / filename
    
    def save_test_with_metadata(self, filepath: Path, metadata: Dict[str, str],
                                timestamps: List[float], current_readings: List[float],
                                peak_readings: List[float]) -> None:
        """Save test data with metadata header
        
        Args:
            filepath: Path to save file
            metadata: Dictionary with test metadata
            timestamps: List of timestamps
            current_readings: List of current force readings
            peak_readings: List of peak force readings
        """
        with open(filepath, 'w', newline='') as f:
            # Write metadata as comments
            f.write(f"# Test Name: {metadata.get('test_name', 'Unknown')}\n")
            f.write(f"# Date: {metadata.get('datetime', datetime.now().strftime('%Y-%m-%d %H:%M:%S'))}\n")
            f.write(f"# Technician: {metadata.get('technician', 'Unknown')}\n")
            
            # Calculate and write peak force
            peak_force = peak_readings[-1] if peak_readings else 0.0
            f.write(f"# Peak Force: {peak_force:.3f} kN\n")
            
            # Write notes if present
            notes = metadata.get('notes', '').strip()
            if notes:
                # Handle multi-line notes
                for line in notes.split('\n'):
                    f.write(f"# Notes: {line}\n")
            else:
                f.write(f"# Notes: \n")
            
            f.write("#\n")  # Separator line
            
            # Write CSV data
            writer = csv.writer(f)
            writer.writerow(['timestamp_s', 'current_kN', 'peak_kN'])
            
            for t, c, p in zip(timestamps, current_readings, peak_readings):
                writer.writerow([f"{t:.3f}", f"{c:.3f}", f"{p:.3f}"])
    
    def read_test_metadata(self, filepath: Path) -> Optional[Dict[str, str]]:
        """Read metadata from a test file
        
        Args:
            filepath: Path to test file
            
        Returns:
            Dictionary with metadata, or None if file doesn't exist
        """
        if not filepath.exists():
            return None
        
        metadata = {
            'test_name': '',
            'datetime': '',
            'technician': '',
            'peak_force': '',
            'notes': '',
            'filepath': str(filepath)
        }
        
        try:
            with open(filepath, 'r') as f:
                notes_lines = []
                for line in f:
                    line = line.strip()
                    if not line.startswith('#'):
                        break  # End of metadata
                    
                    # Remove leading # and whitespace
                    content = line[1:].strip()
                    
                    if content.startswith('Test Name:'):
                        metadata['test_name'] = content[10:].strip()
                    elif content.startswith('Date:'):
                        metadata['datetime'] = content[5:].strip()
                    elif content.startswith('Technician:'):
                        metadata['technician'] = content[11:].strip()
                    elif content.startswith('Peak Force:'):
                        # Remove " kN" unit suffix before storing
                        peak_str = content[11:].strip()
                        metadata['peak_force'] = peak_str.replace(' kN', '').strip()
                    elif content.startswith('Notes:'):
                        note_content = content[6:].strip()
                        if note_content:
                            notes_lines.append(note_content)
                
                metadata['notes'] = '\n'.join(notes_lines)
        
        except Exception as e:
            print(f"Error reading metadata from {filepath}: {e}")
            return None
        
        return metadata
    
    def update_test_metadata(self, filepath: Path, metadata: Dict[str, str]) -> bool:
        """Update metadata in an existing test file
        
        Args:
            filepath: Path to test file
            metadata: New metadata dictionary
            
        Returns:
            True if successful, False otherwise
        """
        if not filepath.exists():
            return False
        
        try:
            # Read existing data rows
            data_rows = []
            with open(filepath, 'r') as f:
                reader = csv.reader(f)
                in_data = False
                for row in f:
                    if not row.startswith('#'):
                        in_data = True
                    if in_data:
                        data_rows.append(row)
            
            # Rewrite file with new metadata
            with open(filepath, 'w', newline='') as f:
                # Write updated metadata
                f.write(f"# Test Name: {metadata.get('test_name', 'Unknown')}\n")
                f.write(f"# Date: {metadata.get('datetime', '')}\n")
                f.write(f"# Technician: {metadata.get('technician', 'Unknown')}\n")
                f.write(f"# Peak Force: {metadata.get('peak_force', '0.000')} kN\n")
                
                notes = metadata.get('notes', '').strip()
                if notes:
                    for line in notes.split('\n'):
                        f.write(f"# Notes: {line}\n")
                else:
                    f.write(f"# Notes: \n")
                
                f.write("#\n")
                
                # Write data rows
                for row in data_rows:
                    f.write(row)
            
            return True
        
        except Exception as e:
            print(f"Error updating metadata for {filepath}: {e}")
            return False
    
    def list_all_tests(self) -> List[Dict[str, str]]:
        """List all tests in the tests directory
        
        Returns:
            List of metadata dictionaries for all tests
        """
        tests = []
        
        # Walk through date folders
        for date_folder in sorted(self.tests_directory.iterdir(), reverse=True):
            if date_folder.is_dir():
                # Find CSV files in this date folder
                for csv_file in sorted(date_folder.glob("*.csv"), reverse=True):
                    metadata = self.read_test_metadata(csv_file)
                    if metadata:
                        metadata['date_folder'] = date_folder.name
                        tests.append(metadata)
        
        return tests
    
    def get_tests_by_date(self, date_str: str) -> List[Dict[str, str]]:
        """Get all tests for a specific date
        
        Args:
            date_str: Date string in YYYY-MM-DD format
            
        Returns:
            List of metadata dictionaries
        """
        date_folder = self.tests_directory / date_str
        if not date_folder.exists():
            return []
        
        tests = []
        for csv_file in sorted(date_folder.glob("*.csv")):
            metadata = self.read_test_metadata(csv_file)
            if metadata:
                metadata['date_folder'] = date_str
                tests.append(metadata)
        
        return tests
    
    def _sanitize_filename(self, name: str) -> str:
        """Sanitize a string for use as filename
        
        Args:
            name: Original name
            
        Returns:
            Sanitized filename-safe string
        """
        # Remove or replace invalid characters
        safe_name = re.sub(r'[<>:"/\\|?*]', '_', name)
        # Remove leading/trailing dots and spaces
        safe_name = safe_name.strip('. ')
        # Limit length
        safe_name = safe_name[:100]
        # Replace spaces with underscores
        safe_name = safe_name.replace(' ', '_')
        
        return safe_name if safe_name else "test"
