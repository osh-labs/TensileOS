"""
Real-time plotting engine for TensileOS data
Uses matplotlib for live force vs. time visualization
"""

import matplotlib
matplotlib.use('TkAgg')
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
import matplotlib.pyplot as plt
from typing import List, Optional


class TensilePlotter:
    """Manages real-time plotting of tensile test data"""
    
    def __init__(self, parent_frame, settings: dict = None):
        """Initialize plotter with matplotlib figure
        
        Args:
            parent_frame: Tkinter frame to embed canvas in
            settings: Dictionary of plot settings
        """
        self.settings = settings or {}
        
        # Create matplotlib figure
        self.figure = Figure(figsize=(8, 6), dpi=100)
        self.ax = self.figure.add_subplot(111)
        
        # Create canvas
        self.canvas = FigureCanvasTkAgg(self.figure, master=parent_frame)
        self.canvas_widget = self.canvas.get_tk_widget()
        
        # Data storage
        self.timestamps: List[float] = []
        self.current_readings: List[float] = []
        self.peak_value: float = 0.0
        
        # Plot elements
        self.current_line = None
        self.peak_line = None
        
        # Initialize plot
        self._setup_plot()
        self.apply_settings(self.settings)
    
    def _setup_plot(self):
        """Setup initial plot configuration"""
        self.ax.set_xlabel('Time (seconds)', fontsize=10)
        self.ax.set_ylabel('Force (kN)', fontsize=10)
        self.ax.set_title('Real-Time Force Measurement', fontsize=12, fontweight='bold')
        
        # Create empty line objects
        self.current_line, = self.ax.plot([], [], lw=2, label='Current Force')
        self.peak_line = self.ax.axhline(y=0, linestyle='--', lw=2, label='Peak Force')
        
        self.ax.legend(loc='upper left')
        self.ax.grid(True, alpha=0.3)
        
        self.figure.tight_layout()
    
    def update_data(self, timestamps: List[float], current_readings: List[float], peak: float):
        """Update plot with new data
        
        Args:
            timestamps: List of timestamps in seconds
            current_readings: List of current force readings in kN
            peak: Current peak force value in kN
        """
        self.timestamps = timestamps
        self.current_readings = current_readings
        self.peak_value = peak
        
        # Update line data
        if len(self.timestamps) > 0:
            self.current_line.set_data(self.timestamps, self.current_readings)
            self.peak_line.set_ydata([peak, peak])
            
            # Auto-scale axes if enabled
            if self.settings.get('auto_scale_x', True):
                max_time = max(self.timestamps) if self.timestamps else 1
                self.ax.set_xlim(0, max(max_time * 1.1, 10))  # At least 10 seconds visible
            else:
                self.ax.set_xlim(self.settings.get('x_min', 0), self.settings.get('x_max', 60))
            
            if self.settings.get('auto_scale_y', True):
                max_reading = max(self.current_readings) if self.current_readings else 1
                self.ax.set_ylim(0, max(max_reading * 1.2, 5))  # At least 5 kN visible
            else:
                self.ax.set_ylim(self.settings.get('y_min', 0), self.settings.get('y_max', 30))
        
        # Redraw canvas
        self.canvas.draw()
        self.canvas.flush_events()
    
    def clear_plot(self):
        """Clear all data from plot"""
        self.timestamps.clear()
        self.current_readings.clear()
        self.peak_value = 0.0
        
        self.current_line.set_data([], [])
        self.peak_line.set_ydata([0, 0])
        
        # Reset to reasonable defaults or respect settings
        if self.settings.get('auto_scale_x', True):
            self.ax.set_xlim(0, 60)
        else:
            self.ax.set_xlim(self.settings.get('x_min', 0), self.settings.get('x_max', 60))
        
        if self.settings.get('auto_scale_y', True):
            self.ax.set_ylim(0, 30)
        else:
            self.ax.set_ylim(self.settings.get('y_min', 0), self.settings.get('y_max', 30))
        
        self.canvas.draw()
        self.canvas.flush_events()
    
    def apply_settings(self, settings: dict):
        """Apply visual settings to plot
        
        Args:
            settings: Dictionary with plot settings
        """
        self.settings = settings
        
        # Apply colors
        plot_color = settings.get('plot_line_color', '#2196F3')
        peak_color = settings.get('peak_line_color', '#FF5722')
        grid_color = settings.get('grid_color', '#CCCCCC')
        grid_enabled = settings.get('grid_enabled', True)
        
        if self.current_line:
            self.current_line.set_color(plot_color)
        
        if self.peak_line:
            self.peak_line.set_color(peak_color)
        
        # Grid settings
        self.ax.grid(grid_enabled, color=grid_color, alpha=0.3)
        
        # Apply axis limits if not auto-scaling
        if not settings.get('auto_scale_x', True):
            x_min = settings.get('x_min', 0)
            x_max = settings.get('x_max', 60)
            self.ax.set_xlim(x_min, x_max)
        
        if not settings.get('auto_scale_y', True):
            y_min = settings.get('y_min', 0)
            y_max = settings.get('y_max', 30)
            self.ax.set_ylim(y_min, y_max)
        
        self.canvas.draw()
    
    def get_canvas_widget(self):
        """Get the tkinter canvas widget
        
        Returns:
            Canvas widget for packing in GUI
        """
        return self.canvas_widget
    
    def set_title(self, title: str):
        """Update the plot title
        
        Args:
            title: New title for the plot
        """
        self.ax.set_title(title, fontsize=12, fontweight='bold')
        self.canvas.draw()
        self.canvas.flush_events()
