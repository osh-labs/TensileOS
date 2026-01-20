"""
Settings management for TensileCompanion
Handles loading, saving, and managing user preferences
"""

import json
import os
from pathlib import Path


class Settings:
    """Manages application settings with persistent JSON storage"""
    
    DEFAULT_SETTINGS = {
        "plot_line_color": "#2196F3",
        "peak_line_color": "#FF5722",
        "grid_color": "#CCCCCC",
        "grid_enabled": True,
        "x_min": 0,
        "x_max": 60,
        "y_min": 0,
        "y_max": 30,
        "auto_scale_x": True,
        "auto_scale_y": True,
        "export_directory": "./exports",
        "tests_directory": "./Tests",
        "last_com_port": "",
        "last_technician": "",
        "recent_technicians": [],
        "company_name": ""
    }
    
    def __init__(self, config_path="config.json"):
        """Initialize settings manager
        
        Args:
            config_path: Path to configuration JSON file
        """
        self.config_path = Path(config_path)
        self.settings = self.DEFAULT_SETTINGS.copy()
        self.load()
    
    def load(self):
        """Load settings from JSON file"""
        try:
            if self.config_path.exists():
                with open(self.config_path, 'r') as f:
                    loaded_settings = json.load(f)
                    # Update settings, keeping defaults for missing keys
                    self.settings.update(loaded_settings)
        except Exception as e:
            print(f"Error loading settings: {e}. Using defaults.")
    
    def save(self):
        """Save current settings to JSON file"""
        try:
            # Create directory if it doesn't exist
            self.config_path.parent.mkdir(parents=True, exist_ok=True)
            
            with open(self.config_path, 'w') as f:
                json.dump(self.settings, f, indent=2)
        except Exception as e:
            print(f"Error saving settings: {e}")
    
    def get(self, key, default=None):
        """Get a setting value
        
        Args:
            key: Setting key
            default: Default value if key not found
            
        Returns:
            Setting value
        """
        return self.settings.get(key, default)
    
    def set(self, key, value):
        """Set a setting value
        
        Args:
            key: Setting key
            value: Setting value
        """
        self.settings[key] = value
        self.save()
    
    def restore_defaults(self):
        """Restore all settings to default values"""
        self.settings = self.DEFAULT_SETTINGS.copy()
        self.save()
    
    def get_all(self):
        """Get all settings as dictionary
        
        Returns:
            Dictionary of all settings
        """
        return self.settings.copy()
