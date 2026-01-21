"""
TensileCompanion - Main GUI Application
Real-time visualization and control for TensileOS tensile testing equipment
"""

import tkinter as tk
from tkinter import ttk, filedialog, colorchooser, messagebox
import sys
from pathlib import Path
from typing import List, Dict
from datetime import datetime

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))

from utils.serial_handler import SerialHandler
from data.data_manager import DataManager
from data.test_manager import TestManager
from visualization.plotter import TensilePlotter
from config.settings import Settings
from ui.metadata_dialog import MetadataDialog
from ui.metadata_edit_dialog import MetadataEditDialog
from ui.test_browser import TestBrowser
from ui.statistics_window import StatisticsWindow
from analysis.statistics import TestStatistics


class TensileCompanionApp:
    """Main application class for TensileCompanion"""
    
    def __init__(self, root):
        """Initialize the application
        
        Args:
            root: Tkinter root window
        """
        self.root = root
        
        # Initialize components
        self.settings = Settings("config.json")
        
        # Get version and set title
        version = self.settings.get('software_version', '1.0.0')
        self.root.title(f"TensileCompanion v{version} - TensileOS Control & Visualization")
        self.root.geometry("1400x800")
        self.data_manager = DataManager(self.settings.get('export_directory'))
        self.test_manager = TestManager(self.settings.get('tests_directory', './Tests'))
        self.serial_handler = SerialHandler(
            data_callback=self.on_data_received,
            error_callback=self.on_error,
            raw_data_callback=self.on_raw_data
        )
        
        # State variables
        self.is_test_running = False
        self.has_metadata = False  # Track if current session is a formal test with metadata
        
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
        main_container.columnconfigure(0, weight=1)
        main_container.rowconfigure(0, weight=1)
        
        # Create notebook (tabbed interface)
        self.notebook = ttk.Notebook(main_container)
        self.notebook.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Create tabs
        self.live_test_tab = ttk.Frame(self.notebook)
        self.test_browser_tab = ttk.Frame(self.notebook)
        self.settings_tab = ttk.Frame(self.notebook)
        
        self.notebook.add(self.live_test_tab, text="Live Test")
        self.notebook.add(self.test_browser_tab, text="Test Browser")
        self.notebook.add(self.settings_tab, text="Settings")
        
        # Build Live Test tab
        self._create_live_test_tab()
        
        # Build Test Browser tab
        self._create_test_browser_tab()
        
        # Build Settings tab
        self._create_settings_tab()
    
    def _create_live_test_tab(self):
        """Create the live test tab layout"""
        # Configure grid for three-panel layout
        self.live_test_tab.columnconfigure(0, weight=0, minsize=100)  # Fixed narrower left panel
        self.live_test_tab.columnconfigure(1, weight=1)
        self.live_test_tab.rowconfigure(0, weight=1)
        
        # Create three panels
        self._create_left_panel(self.live_test_tab)
        self._create_center_panel(self.live_test_tab)
        self._create_right_panel(self.live_test_tab)
    
    def _create_test_browser_tab(self):
        """Create the test browser tab"""
        # Create test browser
        self.test_browser = TestBrowser(
            self.test_browser_tab,
            self.test_manager,
            on_calculate_stats=self._on_calculate_statistics,
            on_edit_metadata=self._on_edit_metadata,
            on_load_test=self._on_load_test
        )
        self.test_browser.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
    
    def _create_settings_tab(self):
        """Create the settings tab"""
        # Main container
        settings_container = ttk.Frame(self.settings_tab, padding="20")
        settings_container.pack(fill=tk.BOTH, expand=True)
        
        # Title
        title_label = ttk.Label(settings_container, text="Application Settings", 
                               font=("", 14, "bold"))
        title_label.pack(pady=(0, 20))
        
        # Company Information Section
        company_frame = ttk.LabelFrame(settings_container, text="Company Information", padding="15")
        company_frame.pack(fill=tk.X, pady=(0, 20))
        
        # Company Name
        ttk.Label(company_frame, text="Company Name:", font=("", 10, "bold")).grid(
            row=0, column=0, sticky=tk.W, pady=5, padx=5)
        self.company_var = tk.StringVar(value=self.settings.get('company_name', ''))
        company_entry = ttk.Entry(company_frame, textvariable=self.company_var, width=40)
        company_entry.grid(row=0, column=1, sticky=(tk.W, tk.E), pady=5, padx=5)
        
        # Technician Management Section
        tech_frame = ttk.LabelFrame(settings_container, text="Technician Management", padding="15")
        tech_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 20))
        
        # Default Technician
        ttk.Label(tech_frame, text="Default Technician:", font=("", 10, "bold")).grid(
            row=0, column=0, sticky=tk.W, pady=5, padx=5)
        self.default_tech_var = tk.StringVar(value=self.settings.get('last_technician', ''))
        default_tech_entry = ttk.Entry(tech_frame, textvariable=self.default_tech_var, width=40)
        default_tech_entry.grid(row=0, column=1, sticky=(tk.W, tk.E), pady=5, padx=5)
        
        # Recent Technicians List
        ttk.Label(tech_frame, text="Recent Technicians:", font=("", 10, "bold")).grid(
            row=1, column=0, sticky=(tk.W, tk.N), pady=5, padx=5)
        
        # Frame for listbox and scrollbar
        list_frame = ttk.Frame(tech_frame)
        list_frame.grid(row=1, column=1, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5, padx=5)
        
        # Scrollbar
        tech_scrollbar = ttk.Scrollbar(list_frame)
        tech_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Listbox
        self.tech_listbox = tk.Listbox(list_frame, height=6, 
                                       yscrollcommand=tech_scrollbar.set)
        self.tech_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tech_scrollbar.config(command=self.tech_listbox.yview)
        
        # Populate listbox
        for tech in self.settings.get('recent_technicians', []):
            self.tech_listbox.insert(tk.END, tech)
        
        # Buttons for technician list management
        tech_btn_frame = ttk.Frame(tech_frame)
        tech_btn_frame.grid(row=2, column=1, sticky=tk.W, pady=5, padx=5)
        
        ttk.Button(tech_btn_frame, text="Add Technician", 
                  command=self._add_technician).pack(side=tk.LEFT, padx=5)
        ttk.Button(tech_btn_frame, text="Remove Selected", 
                  command=self._remove_technician).pack(side=tk.LEFT, padx=5)
        ttk.Button(tech_btn_frame, text="Clear All", 
                  command=self._clear_technicians).pack(side=tk.LEFT, padx=5)
        
        # Configure grid weights
        tech_frame.columnconfigure(1, weight=1)
        tech_frame.rowconfigure(1, weight=1)
        company_frame.columnconfigure(1, weight=1)
        
        # Save button
        save_btn_frame = ttk.Frame(settings_container)
        save_btn_frame.pack(fill=tk.X, pady=(10, 0))
        
        ttk.Button(save_btn_frame, text="Save Settings", 
                  command=self._save_settings, width=20).pack(side=tk.RIGHT, padx=5)
    
    def _add_technician(self):
        """Add a new technician to the list"""
        # Simple dialog to get technician name
        dialog = tk.Toplevel(self.root)
        dialog.title("Add Technician")
        dialog.geometry("300x120")
        dialog.transient(self.root)
        dialog.grab_set()
        
        ttk.Label(dialog, text="Technician Name:").pack(pady=10)
        name_var = tk.StringVar()
        entry = ttk.Entry(dialog, textvariable=name_var, width=30)
        entry.pack(pady=5)
        entry.focus()
        
        def add():
            name = name_var.get().strip()
            if name and name not in self.tech_listbox.get(0, tk.END):
                self.tech_listbox.insert(tk.END, name)
            dialog.destroy()
        
        btn_frame = ttk.Frame(dialog)
        btn_frame.pack(pady=10)
        ttk.Button(btn_frame, text="Add", command=add).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="Cancel", command=dialog.destroy).pack(side=tk.LEFT, padx=5)
        
        entry.bind('<Return>', lambda e: add())
        dialog.bind('<Escape>', lambda e: dialog.destroy())
    
    def _remove_technician(self):
        """Remove selected technician from the list"""
        selection = self.tech_listbox.curselection()
        if selection:
            self.tech_listbox.delete(selection[0])
    
    def _clear_technicians(self):
        """Clear all technicians from the list"""
        if messagebox.askyesno("Confirm Clear", 
                              "Are you sure you want to clear all technicians?",
                              parent=self.root):
            self.tech_listbox.delete(0, tk.END)
    
    def _save_settings(self):
        """Save settings from the settings tab"""
        # Update settings
        self.settings.set('company_name', self.company_var.get().strip())
        self.settings.set('last_technician', self.default_tech_var.get().strip())
        
        # Update technicians list
        tech_list = list(self.tech_listbox.get(0, tk.END))
        self.settings.set('recent_technicians', tech_list)
        
        messagebox.showinfo("Settings Saved", 
                          "Settings have been saved successfully.",
                          parent=self.root)
    
    def _create_left_panel(self, parent):
        """Create left sidebar for connection controls"""
        left_frame = ttk.LabelFrame(parent, text="Connection", padding="10")
        left_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), padx=5)
        
        # COM Port selection
        ttk.Label(left_frame, text="COM Port:").pack(anchor=tk.W, pady=5)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(left_frame, textvariable=self.port_var, width=10)
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
        self.error_text = tk.Text(left_frame, height=4, width=15, wrap=tk.WORD, 
                                 bg="#FFEEEE", fg="red", font=("", 8))
        self.error_text.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # Debug toggle
        self.debug_var = tk.BooleanVar(value=False)  # Start with debug disabled
        ttk.Checkbutton(left_frame, text="Debug to Console", variable=self.debug_var,
                       command=self._toggle_debug).pack(anchor=tk.W, pady=5)
        
        # Enable debug immediately
        self._toggle_debug()
        
        # Raw serial data display
        ttk.Label(left_frame, text="Raw Serial Data:").pack(anchor=tk.W, pady=(10, 2))
        
        # Frame for text and scrollbar
        serial_frame = ttk.Frame(left_frame)
        serial_frame.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # Scrollbar
        scrollbar = ttk.Scrollbar(serial_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Raw data text widget
        self.raw_data_text = tk.Text(serial_frame, height=10, width=15, wrap=tk.WORD,
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
        
        self.new_test_btn = ttk.Button(control_frame, text="New Test", 
                                       command=self._new_test, state=tk.DISABLED)
        self.new_test_btn.pack(side=tk.LEFT, padx=5)
        
        self.save_test_btn = ttk.Button(control_frame, text="Save Test", 
                                        command=self._save_test, state=tk.DISABLED)
        self.save_test_btn.pack(side=tk.LEFT, padx=5)
        
        self.discard_btn = ttk.Button(control_frame, text="Discard Data", 
                                      command=self._discard_data, state=tk.DISABLED)
        self.discard_btn.pack(side=tk.LEFT, padx=5)
        
        self.pause_btn = ttk.Button(control_frame, text="Pause", 
                                    command=self._pause_test, state=tk.DISABLED)
        self.pause_btn.pack(side=tk.LEFT, padx=5)
        
        self.resume_btn = ttk.Button(control_frame, text="Resume", 
                                     command=self._resume_test, state=tk.DISABLED)
        self.resume_btn.pack(side=tk.LEFT, padx=5)
        
        self.export_chart_btn = ttk.Button(control_frame, text="Export Chart", 
                                           command=self._export_chart)
        self.export_chart_btn.pack(side=tk.LEFT, padx=5)
        
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
            # Enable all control buttons - device is ready
            self.new_test_btn.config(state=tk.NORMAL)
            self.save_test_btn.config(state=tk.NORMAL)
            self.discard_btn.config(state=tk.NORMAL)
            self.pause_btn.config(state=tk.NORMAL)
            self.resume_btn.config(state=tk.NORMAL)  # Always enabled
            self.reconnect_btn.pack_forget()
            
            # Save last used port
            self.settings.set('last_com_port', port)
            
            # Device is paused, waiting for user action
            self.is_test_running = False
            self.has_metadata = False
        else:
            messagebox.showerror("Connection Error", "Failed to connect to device")
    
    def _disconnect(self):
        """Disconnect from device"""
        self.serial_handler.disconnect()
        self.status_label.config(text="Disconnected", foreground="red")
        self.connect_btn.config(text="Connect")
        self.new_test_btn.config(state=tk.DISABLED)
        self.save_test_btn.config(state=tk.DISABLED)
        self.discard_btn.config(state=tk.DISABLED)
        self.pause_btn.config(state=tk.DISABLED)
        self.resume_btn.config(state=tk.DISABLED)
        self.is_test_running = False
        self.has_metadata = False
    
    def _reconnect(self):
        """Attempt to reconnect"""
        self.error_text.delete(1.0, tk.END)
        self.reconnect_btn.pack_forget()
        self._connect()
    
    def _new_test(self):
        """Start new test with metadata capture"""
        # Show metadata dialog for NEW test
        dialog = MetadataDialog(
            self.root, 
            last_technician=self.settings.get('last_technician', ''),
            recent_technicians=self.settings.get('recent_technicians', [])
        )
        metadata = dialog.get_result()
        
        # Check if user cancelled
        if metadata is None:
            return
        
        # Save technician for next time
        self.settings.set('last_technician', metadata.get('technician', ''))
        
        # Update recent technicians list
        recent = self.settings.get('recent_technicians', [])
        technician = metadata.get('technician', '')
        if technician and technician not in recent:
            recent.insert(0, technician)
            recent = recent[:10]  # Keep last 10
            self.settings.set('recent_technicians', recent)
        
        # Clear data and plot FIRST
        self.data_manager.clear()
        self.plotter.clear_plot()
        
        # THEN set metadata for the new test (after clearing)
        self.data_manager.set_test_metadata(metadata)
        self.has_metadata = True  # Mark this as a formal test with metadata
        
        # Send command to device
        if self.serial_handler.is_connected():
            self.serial_handler.send_start_new_test()
            self.is_test_running = True
            
            # Update chart title with test name
            test_name = metadata.get('test_name', 'Test')
            self.plotter.set_title(f"Real-Time Force Measurement - {test_name}")
            
            # Update footer with project name from metadata
            project_name = metadata.get('project', '')
            if project_name:
                self.plotter.set_project_name(project_name)
            
            # Update button states
            self.pause_btn.config(state=tk.NORMAL)
            # Resume button always enabled
    
    def _save_test(self):
        """Save current test data to file"""
        if not self.data_manager.has_data():
            messagebox.showwarning("No Data", "No test data to save.", parent=self.root)
            return
        
        try:
            # Get metadata from data_manager
            metadata = self.data_manager.get_test_metadata()
            
            if metadata is None:
                messagebox.showerror("No Metadata", 
                                   "Test has no metadata. Please start tests using 'New Test' button.",
                                   parent=self.root)
                return
            
            # Get test data
            timestamps, currents, peaks = self.data_manager.get_data()
            
            # Calculate peak force
            if peaks:
                peak_force = max(peaks)
            else:
                peak_force = 0.0
            
            # Add peak force to metadata
            metadata['peak_force'] = f"{peak_force:.3f}"
            
            # Generate filepath
            filepath = self.test_manager.generate_test_filepath(
                test_name=metadata.get('test_name', 'test'),
                test_datetime=metadata.get('datetime')
            )
            
            # Save using TestManager
            self.test_manager.save_test_with_metadata(
                filepath=filepath,
                metadata=metadata,
                timestamps=timestamps,
                current_readings=currents,
                peak_readings=peaks
            )
            
            messagebox.showinfo("Test Saved", f"Test data exported to:\n{filepath}", parent=self.root)
            
            # Clear data after successful save
            self.data_manager.clear()
            self.plotter.clear_plot()
            self.peak_label.config(text="0.000 kN")
            
            # Clear test state but keep buttons enabled for live viewing or new test
            self.is_test_running = False
            self.has_metadata = False
            
        except Exception as e:
            messagebox.showerror("Save Error", f"Failed to save test:\n{str(e)}", parent=self.root)
    
    def _discard_data(self):
        """Discard current test data without saving"""
        if self.data_manager.has_data():
            if messagebox.askyesno("Confirm", "Discard current test data without saving?", parent=self.root):
                self.data_manager.clear()
                self.plotter.clear_plot()
                self.peak_label.config(text="0.000 kN")
                
                # Clear test state but keep buttons enabled
                self.is_test_running = False
                self.has_metadata = False
        else:
            messagebox.showinfo("No Data", "No test data to discard.", parent=self.root)
    
    def _export_chart(self):
        """Export the current chart as an image file"""
        # Ask user for save location
        file_path = filedialog.asksaveasfilename(
            parent=self.root,
            title="Export Chart As Image",
            defaultextension=".png",
            filetypes=[
                ("PNG Image", "*.png"),
                ("JPEG Image", "*.jpg"),
                ("PDF Document", "*.pdf"),
                ("SVG Vector", "*.svg"),
                ("All Files", "*.*")
            ],
            initialfile=f"chart_export_{datetime.now().strftime('%Y%m%d_%H%M%S')}.png"
        )
        
        if file_path:
            try:
                self.plotter.save_figure(file_path)
                messagebox.showinfo("Success", f"Chart exported successfully to:\n{file_path}", parent=self.root)
            except Exception as e:
                messagebox.showerror("Export Error", f"Failed to export chart:\n{str(e)}", parent=self.root)
    
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
    
    def _on_calculate_statistics(self, test_metadata_list: List[Dict[str, str]]):
        """Callback for calculating statistics on selected tests
        
        Args:
            test_metadata_list: List of test metadata dictionaries
        """
        if not test_metadata_list:
            messagebox.showwarning("No Tests Selected", 
                                 "Please select at least one test to calculate statistics.",
                                 parent=self.root)
            return
        
        if len(test_metadata_list) < 2:
            messagebox.showwarning("Insufficient Tests", 
                                 "Please select at least 2 tests for statistical analysis.",
                                 parent=self.root)
            return
        
        try:
            # Extract peak forces from metadata
            peak_forces = []
            valid_metadata = []
            
            for metadata in test_metadata_list:
                if metadata and 'peak_force' in metadata:
                    try:
                        peak_forces.append(float(metadata['peak_force']))
                        valid_metadata.append(metadata)
                    except (ValueError, TypeError):
                        pass
            
            if not peak_forces:
                messagebox.showerror("Error", "Could not read peak forces from selected tests.",
                                   parent=self.root)
                return
            
            # Calculate statistics
            stats = TestStatistics(valid_metadata)
            
            # Show statistics window
            StatisticsWindow(self.root, valid_metadata, stats, self.settings)
            
        except Exception as e:
            messagebox.showerror("Statistics Error", 
                               f"Failed to calculate statistics:\n{str(e)}",
                               parent=self.root)
    
    def _on_edit_metadata(self, metadata: Dict[str, str]):
        """Callback for editing test metadata
        
        Args:
            metadata: Test metadata dictionary
        """
        try:
            if not metadata:
                messagebox.showerror("Error", "Invalid test metadata.",
                                   parent=self.root)
                return
            
            # Get filepath from metadata
            filepath = metadata.get('filepath')
            if not filepath:
                messagebox.showerror("Error", "Test file path not found.",
                                   parent=self.root)
                return
            
            # Show edit dialog
            dialog = MetadataEditDialog(self.root, metadata)
            updated_metadata = dialog.get_result()
            
            if updated_metadata:
                # Update metadata in file
                self.test_manager.update_test_metadata(filepath, updated_metadata)
                messagebox.showinfo("Success", "Test metadata updated successfully.",
                                  parent=self.root)
                
                # Refresh test browser
                self.test_browser.refresh()
        
        except Exception as e:
            messagebox.showerror("Edit Error", 
                               f"Failed to update metadata:\n{str(e)}",
                               parent=self.root)
    
    def _on_load_test(self, metadata: Dict[str, str]):
        """Callback for loading a test into the main window
        
        Args:
            metadata: Test metadata dictionary
        """
        try:
            if not metadata:
                messagebox.showerror("Error", "Invalid test metadata.",
                                   parent=self.root)
                return
            
            # Get filepath from metadata
            filepath = metadata.get('filepath')
            if not filepath:
                messagebox.showerror("Error", "Test file path not found.",
                                   parent=self.root)
                return
            
            # Read test data from CSV
            test_data = self.test_manager.read_test_data(Path(filepath))
            if not test_data:
                messagebox.showerror("Error", "Failed to read test data from file.",
                                   parent=self.root)
                return
            
            timestamps, current_readings, peak_readings = test_data
            
            # Load data into plotter
            self.plotter.load_historical_data(timestamps, current_readings, peak_readings)
            
            # Update chart title with test name
            test_name = metadata.get('test_name', 'Unknown')
            self.plotter.set_title(f"Saved Tensile Test - {test_name}")
            
            # Update footer with project name if available
            project_name = metadata.get('project', '')
            if project_name:
                self.plotter.set_project_name(project_name)
            
            # Store metadata in data manager for display
            self.data_manager.set_test_metadata(metadata)
            
            # Switch to Live Test tab
            self.notebook.select(self.live_test_tab)
            
            # Show success message
            test_name = metadata.get('test_name', 'Unknown')
            messagebox.showinfo("Test Loaded", 
                              f"Successfully loaded test: {test_name}",
                              parent=self.root)
        
        except Exception as e:
            messagebox.showerror("Load Error", 
                               f"Failed to load test:\n{str(e)}",
                               parent=self.root)


def main():
    """Main entry point"""
    root = tk.Tk()
    app = TensileCompanionApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()


if __name__ == "__main__":
    main()
