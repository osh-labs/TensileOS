"""
TensileCompanion - Main GUI Application
Real-time visualization and control for TensileOS tensile testing equipment
"""

import tkinter as tk
from tkinter import ttk, filedialog, colorchooser, messagebox
import sys
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))

from utils.serial_handler import SerialHandler
from data.data_manager import DataManager
from visualization.plotter import TensilePlotter
from config.settings import Settings


class TensileCompanionApp:
    """Main application class for TensileCompanion"""
    
    def __init__(self, root):
        """Initialize the application
        
        Args:
            root: Tkinter root window
        """
        self.root = root
        self.root.title("TensileCompanion - TensileOS Control & Visualization")
        self.root.geometry("1400x800")
        
        # Initialize components
        self.settings = Settings("config.json")
        self.data_manager = DataManager(self.settings.get('export_directory'))
        self.serial_handler = SerialHandler(
            data_callback=self.on_data_received,
            error_callback=self.on_error,
            raw_data_callback=self.on_raw_data
        )
        
        # State variables
        self.is_test_running = False
        
        # Build GUI
        self._create_gui()
        
        # Load saved settings
        self._apply_settings_to_gui()
    
    def _create_gui(self):
        """Create the main GUI layout"""
        # Create main container
        main_container = ttk.Frame(self.root, padding="5")
        main_container.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Configure grid weights
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_container.columnconfigure(1, weight=1)
        main_container.rowconfigure(0, weight=1)
        
        # Create three panels
        self._create_left_panel(main_container)
        self._create_center_panel(main_container)
        self._create_right_panel(main_container)
    
    def _create_left_panel(self, parent):
        """Create left sidebar for connection controls"""
        left_frame = ttk.LabelFrame(parent, text="Connection", padding="10")
        left_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), padx=5)
        
        # COM Port selection
        ttk.Label(left_frame, text="COM Port:").pack(anchor=tk.W, pady=5)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(left_frame, textvariable=self.port_var, width=20)
        self.port_combo.pack(fill=tk.X, pady=5)
        
        # Refresh ports button
        ttk.Button(left_frame, text="Refresh Ports", 
                  command=self._refresh_ports).pack(fill=tk.X, pady=5)
        
        # Connect button
        self.connect_btn = ttk.Button(left_frame, text="Connect", 
                                      command=self._toggle_connection)
        self.connect_btn.pack(fill=tk.X, pady=5)
        
        # Connection status
        self.status_label = ttk.Label(left_frame, text="Disconnected", 
                                      foreground="red", font=("", 10, "bold"))
        self.status_label.pack(pady=10)
        
        # Error display section
        ttk.Label(left_frame, text="Connection Errors:", font=("", 9, "bold")).pack(anchor=tk.W, pady=(5, 2))
        self.error_text = tk.Text(left_frame, height=4, wrap=tk.WORD, 
                                 bg="#FFEEEE", fg="red", font=("", 8))
        self.error_text.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # Debug toggle
        self.debug_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(left_frame, text="Debug to Console", variable=self.debug_var,
                       command=self._toggle_debug).pack(anchor=tk.W, pady=5)
        
        # Raw serial data display
        ttk.Label(left_frame, text="Raw Serial Data:").pack(anchor=tk.W, pady=(10, 2))
        
        # Frame for text and scrollbar
        serial_frame = ttk.Frame(left_frame)
        serial_frame.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # Scrollbar
        scrollbar = ttk.Scrollbar(serial_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Raw data text widget
        self.raw_data_text = tk.Text(serial_frame, height=10, wrap=tk.WORD,
                                     bg="#F0F0F0", fg="#000000", font=("", 8),
                                     yscrollcommand=scrollbar.set)
        self.raw_data_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=self.raw_data_text.yview)
        
        # Reconnect button (initially hidden)
        self.reconnect_btn = ttk.Button(left_frame, text="Reconnect", 
                                       command=self._reconnect)
        
        # Initialize port list
        self._refresh_ports()
    
    def _create_center_panel(self, parent):
        """Create center panel for plot display"""
        center_frame = ttk.Frame(parent, padding="10")
        center_frame.grid(row=0, column=1, sticky=(tk.W, tk.E, tk.N, tk.S), padx=5)
        center_frame.columnconfigure(0, weight=1)
        center_frame.rowconfigure(0, weight=1)
        
        # Plot container
        plot_frame = ttk.LabelFrame(center_frame, text="Real-Time Force Measurement", padding="5")
        plot_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        plot_frame.columnconfigure(0, weight=1)
        plot_frame.rowconfigure(0, weight=1)
        
        # Create plotter
        self.plotter = TensilePlotter(plot_frame, self.settings.get_all())
        self.plotter.get_canvas_widget().pack(fill=tk.BOTH, expand=True)
        
        # Control buttons below plot
        control_frame = ttk.Frame(center_frame)
        control_frame.grid(row=1, column=0, pady=10)
        
        self.new_test_btn = ttk.Button(control_frame, text="New Test (Auto-Export)", 
                                       command=self._new_test, state=tk.DISABLED)
        self.new_test_btn.pack(side=tk.LEFT, padx=5)
        
        self.discard_btn = ttk.Button(control_frame, text="Discard & New Test", 
                                      command=self._discard_and_new, state=tk.DISABLED)
        self.discard_btn.pack(side=tk.LEFT, padx=5)
        
        self.pause_btn = ttk.Button(control_frame, text="Pause", 
                                    command=self._pause_test, state=tk.DISABLED)
        self.pause_btn.pack(side=tk.LEFT, padx=5)
        
        self.resume_btn = ttk.Button(control_frame, text="Resume", 
                                     command=self._resume_test, state=tk.DISABLED)
        self.resume_btn.pack(side=tk.LEFT, padx=5)
        
        # Peak force display
        peak_frame = ttk.Frame(center_frame)
        peak_frame.grid(row=2, column=0, pady=5)
        
        ttk.Label(peak_frame, text="Peak Force:", font=("", 12, "bold")).pack(side=tk.LEFT, padx=5)
        self.peak_label = ttk.Label(peak_frame, text="0.000 kN", 
                                    font=("", 14, "bold"), foreground="#FF5722")
        self.peak_label.pack(side=tk.LEFT, padx=5)
    
    def _create_right_panel(self, parent):
        """Create right sidebar for settings"""
        right_frame = ttk.LabelFrame(parent, text="Settings", padding="10")
        right_frame.grid(row=0, column=2, sticky=(tk.W, tk.E, tk.N, tk.S), padx=5)
        
        # Plot colors section
        ttk.Label(right_frame, text="Plot Colors:", font=("", 10, "bold")).pack(anchor=tk.W, pady=(0, 5))
        
        # Current line color
        color_frame1 = ttk.Frame(right_frame)
        color_frame1.pack(fill=tk.X, pady=2)
        ttk.Label(color_frame1, text="Current Line:").pack(side=tk.LEFT)
        self.plot_color_btn = tk.Button(color_frame1, text="  ", width=3, 
                                        command=lambda: self._choose_color('plot_line_color'))
        self.plot_color_btn.pack(side=tk.RIGHT)
        
        # Peak line color
        color_frame2 = ttk.Frame(right_frame)
        color_frame2.pack(fill=tk.X, pady=2)
        ttk.Label(color_frame2, text="Peak Line:").pack(side=tk.LEFT)
        self.peak_color_btn = tk.Button(color_frame2, text="  ", width=3,
                                        command=lambda: self._choose_color('peak_line_color'))
        self.peak_color_btn.pack(side=tk.RIGHT)
        
        # Grid color
        color_frame3 = ttk.Frame(right_frame)
        color_frame3.pack(fill=tk.X, pady=2)
        ttk.Label(color_frame3, text="Grid:").pack(side=tk.LEFT)
        self.grid_color_btn = tk.Button(color_frame3, text="  ", width=3,
                                        command=lambda: self._choose_color('grid_color'))
        self.grid_color_btn.pack(side=tk.RIGHT)
        
        # Grid enabled
        self.grid_enabled_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(right_frame, text="Show Grid", variable=self.grid_enabled_var,
                       command=self._update_plot_settings).pack(anchor=tk.W, pady=5)
        
        ttk.Separator(right_frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)
        
        # Axis scaling section
        ttk.Label(right_frame, text="Axis Scaling:", font=("", 10, "bold")).pack(anchor=tk.W, pady=(0, 5))
        
        # X-axis
        self.auto_scale_x_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(right_frame, text="Auto-scale X", variable=self.auto_scale_x_var,
                       command=self._toggle_x_scale).pack(anchor=tk.W)
        
        x_frame = ttk.Frame(right_frame)
        x_frame.pack(fill=tk.X, pady=2)
        ttk.Label(x_frame, text="X Min:").grid(row=0, column=0, sticky=tk.W)
        self.x_min_var = tk.StringVar(value="0")
        self.x_min_entry = ttk.Entry(x_frame, textvariable=self.x_min_var, width=8)
        self.x_min_entry.grid(row=0, column=1, padx=5)
        
        ttk.Label(x_frame, text="X Max:").grid(row=1, column=0, sticky=tk.W)
        self.x_max_var = tk.StringVar(value="60")
        self.x_max_entry = ttk.Entry(x_frame, textvariable=self.x_max_var, width=8)
        self.x_max_entry.grid(row=1, column=1, padx=5)
        
        # Y-axis
        self.auto_scale_y_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(right_frame, text="Auto-scale Y", variable=self.auto_scale_y_var,
                       command=self._toggle_y_scale).pack(anchor=tk.W, pady=(10, 0))
        
        y_frame = ttk.Frame(right_frame)
        y_frame.pack(fill=tk.X, pady=2)
        ttk.Label(y_frame, text="Y Min:").grid(row=0, column=0, sticky=tk.W)
        self.y_min_var = tk.StringVar(value="0")
        self.y_min_entry = ttk.Entry(y_frame, textvariable=self.y_min_var, width=8)
        self.y_min_entry.grid(row=0, column=1, padx=5)
        
        ttk.Label(y_frame, text="Y Max:").grid(row=1, column=0, sticky=tk.W)
        self.y_max_var = tk.StringVar(value="30")
        self.y_max_entry = ttk.Entry(y_frame, textvariable=self.y_max_var, width=8)
        self.y_max_entry.grid(row=1, column=1, padx=5)
        
        ttk.Button(right_frame, text="Apply Scale", 
                  command=self._update_plot_settings).pack(fill=tk.X, pady=5)
        
        ttk.Separator(right_frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)
        
        # Export directory section
        ttk.Label(right_frame, text="Export Directory:", font=("", 10, "bold")).pack(anchor=tk.W, pady=(0, 5))
        
        export_frame = ttk.Frame(right_frame)
        export_frame.pack(fill=tk.X, pady=2)
        self.export_dir_var = tk.StringVar(value="./exports")
        ttk.Entry(export_frame, textvariable=self.export_dir_var).pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Button(export_frame, text="Browse", command=self._browse_export_dir).pack(side=tk.RIGHT, padx=(5, 0))
        
        ttk.Button(right_frame, text="Set Export Dir", 
                  command=self._set_export_dir).pack(fill=tk.X, pady=5)
        
        ttk.Separator(right_frame, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)
        
        # Restore defaults button
        ttk.Button(right_frame, text="Restore Defaults", 
                  command=self._restore_defaults).pack(fill=tk.X, pady=5)
    
    def _refresh_ports(self):
        """Refresh available COM ports"""
        ports = SerialHandler.list_ports()
        port_names = [port[0] for port in ports]
        self.port_combo['values'] = port_names
        
        if port_names:
            # Try to select last used port
            last_port = self.settings.get('last_com_port', '')
            if last_port in port_names:
                self.port_var.set(last_port)
            else:
                self.port_var.set(port_names[0])
    
    def _toggle_connection(self):
        """Connect or disconnect from device"""
        if self.serial_handler.is_connected():
            self._disconnect()
        else:
            self._connect()
    
    def _connect(self):
        """Connect to selected COM port"""
        port = self.port_var.get()
        if not port:
            messagebox.showerror("Error", "Please select a COM port")
            return
        
        if self.serial_handler.connect(port):
            self.status_label.config(text="Connected", foreground="green")
            self.connect_btn.config(text="Disconnect")
            self.new_test_btn.config(state=tk.NORMAL)
            self.discard_btn.config(state=tk.NORMAL)
            self.pause_btn.config(state=tk.NORMAL)
            self.resume_btn.config(state=tk.NORMAL)
            self.reconnect_btn.pack_forget()
            
            # Save last used port
            self.settings.set('last_com_port', port)
            
            # Start a new test
            self.is_test_running = True
        else:
            messagebox.showerror("Connection Error", "Failed to connect to device")
    
    def _disconnect(self):
        """Disconnect from device"""
        self.serial_handler.disconnect()
        self.status_label.config(text="Disconnected", foreground="red")
        self.connect_btn.config(text="Connect")
        self.new_test_btn.config(state=tk.DISABLED)
        self.discard_btn.config(state=tk.DISABLED)
        self.pause_btn.config(state=tk.DISABLED)
        self.resume_btn.config(state=tk.DISABLED)
        self.is_test_running = False
    
    def _reconnect(self):
        """Attempt to reconnect"""
        self.error_text.delete(1.0, tk.END)
        self.reconnect_btn.pack_forget()
        self._connect()
    
    def _new_test(self):
        """Start new test with auto-export of current test"""
        if self.data_manager.has_data():
            try:
                filepath = self.data_manager.save_current_test()
                messagebox.showinfo("Test Saved", f"Test data exported to:\n{filepath}")
            except Exception as e:
                messagebox.showerror("Export Error", f"Failed to save test:\n{str(e)}")
        
        # Clear data and plot
        self.data_manager.clear()
        self.plotter.clear_plot()
        
        # Send command to device
        if self.serial_handler.is_connected():
            self.serial_handler.send_start_new_test()
            self.is_test_running = True
    
    def _discard_and_new(self):
        """Discard current test and start new without saving"""
        if self.data_manager.has_data():
            if messagebox.askyesno("Confirm", "Discard current test data without saving?"):
                self.data_manager.discard_current_test()
                self.plotter.clear_plot()
                
                if self.serial_handler.is_connected():
                    self.serial_handler.send_start_new_test()
                    self.is_test_running = True
        else:
            # No data, just start fresh
            if self.serial_handler.is_connected():
                self.serial_handler.send_start_new_test()
                self.is_test_running = True
    
    def _pause_test(self):
        """Pause measurement"""
        if self.serial_handler.is_connected():
            self.serial_handler.send_pause()
            self.is_test_running = False
    
    def _resume_test(self):
        """Resume measurement"""
        if self.serial_handler.is_connected():
            self.serial_handler.send_resume()
            self.is_test_running = True
    
    def on_data_received(self, timestamp: float, current: float, peak: float):
        """Callback for new data from device
        
        Args:
            timestamp: Test timestamp in seconds
            current: Current force in kN
            peak: Peak force in kN
        """
        # Add to data manager
        self.data_manager.add_data_point(timestamp, current, peak)
        
        # Update plot
        timestamps, currents, peaks = self.data_manager.get_data()
        self.plotter.update_data(timestamps, currents, peak)
        
        # Update peak display
        self.peak_label.config(text=f"{peak:.3f} kN")
    
    def on_error(self, error_message: str):
        """Callback for connection errors
        
        Args:
            error_message: Error description
        """
        self.error_text.insert(tk.END, f"{error_message}\n")
        self.error_text.see(tk.END)
        
        if "connection lost" in error_message.lower():
            self.status_label.config(text="Connection Lost", foreground="orange")
            self.reconnect_btn.pack(fill=tk.X, pady=5)
            self.is_test_running = False
    
    def on_raw_data(self, line: str):
        """Callback for raw serial data
        
        Args:
            line: Raw serial line
        """
        self.raw_data_text.insert(tk.END, f"{line}\n")
        self.raw_data_text.see(tk.END)
        
        # Limit text buffer to last 1000 lines
        line_count = int(self.raw_data_text.index('end-1c').split('.')[0])
        if line_count > 1000:
            self.raw_data_text.delete('1.0', '2.0')
    
    def _toggle_debug(self):
        """Toggle console debug output"""
        self.serial_handler.set_debug(self.debug_var.get())
    
    def _choose_color(self, setting_key: str):
        """Open color chooser dialog
        
        Args:
            setting_key: Settings key for the color
        """
        current_color = self.settings.get(setting_key, '#000000')
        color = colorchooser.askcolor(initialcolor=current_color, title=f"Choose {setting_key}")
        
        if color[1]:  # color[1] is hex string
            self.settings.set(setting_key, color[1])
            self._apply_settings_to_gui()
            self._update_plot_settings()
    
    def _toggle_x_scale(self):
        """Toggle X-axis auto-scaling"""
        auto = self.auto_scale_x_var.get()
        self.x_min_entry.config(state=tk.DISABLED if auto else tk.NORMAL)
        self.x_max_entry.config(state=tk.DISABLED if auto else tk.NORMAL)
        self.settings.set('auto_scale_x', auto)
        self._update_plot_settings()
    
    def _toggle_y_scale(self):
        """Toggle Y-axis auto-scaling"""
        auto = self.auto_scale_y_var.get()
        self.y_min_entry.config(state=tk.DISABLED if auto else tk.NORMAL)
        self.y_max_entry.config(state=tk.DISABLED if auto else tk.NORMAL)
        self.settings.set('auto_scale_y', auto)
        self._update_plot_settings()
    
    def _update_plot_settings(self):
        """Apply current settings to plot"""
        # Update settings dictionary
        try:
            self.settings.set('grid_enabled', self.grid_enabled_var.get())
            self.settings.set('x_min', float(self.x_min_var.get()))
            self.settings.set('x_max', float(self.x_max_var.get()))
            self.settings.set('y_min', float(self.y_min_var.get()))
            self.settings.set('y_max', float(self.y_max_var.get()))
        except ValueError:
            pass  # Invalid number input, ignore
        
        # Apply to plotter
        self.plotter.apply_settings(self.settings.get_all())
    
    def _browse_export_dir(self):
        """Browse for export directory"""
        directory = filedialog.askdirectory(initialdir=self.export_dir_var.get())
        if directory:
            self.export_dir_var.set(directory)
    
    def _set_export_dir(self):
        """Set the export directory"""
        directory = self.export_dir_var.get()
        self.data_manager.set_export_directory(directory)
        self.settings.set('export_directory', directory)
        messagebox.showinfo("Success", f"Export directory set to:\n{directory}")
    
    def _restore_defaults(self):
        """Restore default settings"""
        if messagebox.askyesno("Confirm", "Restore all settings to defaults?"):
            self.settings.restore_defaults()
            self._apply_settings_to_gui()
            self._update_plot_settings()
    
    def _apply_settings_to_gui(self):
        """Apply loaded settings to GUI elements"""
        # Colors
        self.plot_color_btn.config(bg=self.settings.get('plot_line_color'))
        self.peak_color_btn.config(bg=self.settings.get('peak_line_color'))
        self.grid_color_btn.config(bg=self.settings.get('grid_color'))
        
        # Grid
        self.grid_enabled_var.set(self.settings.get('grid_enabled', True))
        
        # Axis scaling
        self.auto_scale_x_var.set(self.settings.get('auto_scale_x', True))
        self.auto_scale_y_var.set(self.settings.get('auto_scale_y', True))
        
        self.x_min_var.set(str(self.settings.get('x_min', 0)))
        self.x_max_var.set(str(self.settings.get('x_max', 60)))
        self.y_min_var.set(str(self.settings.get('y_min', 0)))
        self.y_max_var.set(str(self.settings.get('y_max', 30)))
        
        self._toggle_x_scale()
        self._toggle_y_scale()
        
        # Export directory
        self.export_dir_var.set(self.settings.get('export_directory', './exports'))
    
    def on_closing(self):
        """Handle application closing"""
        if self.serial_handler.is_connected():
            self.serial_handler.disconnect()
        self.root.destroy()


def main():
    """Main entry point"""
    root = tk.Tk()
    app = TensileCompanionApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()


if __name__ == "__main__":
    main()
