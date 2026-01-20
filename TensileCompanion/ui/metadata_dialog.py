"""
Metadata Dialog for Test Information
Captures required test metadata before starting a test
"""

import tkinter as tk
from tkinter import ttk, messagebox
from datetime import datetime
from typing import Optional, Dict, List


class MetadataDialog:
    """Dialog for capturing test metadata"""
    
    def __init__(self, parent, last_technician: str = "", recent_technicians: List[str] = None):
        """Initialize metadata dialog
        
        Args:
            parent: Parent window
            last_technician: Last used technician name for auto-fill
            recent_technicians: List of recent technician names
        """
        self.result: Optional[Dict[str, str]] = None
        self.recent_technicians = recent_technicians or []
        
        # Create modal dialog
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Test Metadata")
        self.dialog.geometry("500x400")
        self.dialog.transient(parent)
        self.dialog.grab_set()
        
        # Center on parent
        self.dialog.update_idletasks()
        x = parent.winfo_x() + (parent.winfo_width() // 2) - (self.dialog.winfo_width() // 2)
        y = parent.winfo_y() + (parent.winfo_height() // 2) - (self.dialog.winfo_height() // 2)
        self.dialog.geometry(f"+{x}+{y}")
        
        self._create_widgets(last_technician)
        
        # Make dialog modal
        self.dialog.protocol("WM_DELETE_WINDOW", self._on_cancel)
        self.dialog.wait_window()
    
    def _create_widgets(self, last_technician: str):
        """Create dialog widgets"""
        # Main frame
        main_frame = ttk.Frame(self.dialog, padding="20")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Title
        title_label = ttk.Label(main_frame, text="Enter Test Information", 
                               font=("", 12, "bold"))
        title_label.grid(row=0, column=0, columnspan=2, pady=(0, 20))
        
        # Test Name (Required)
        ttk.Label(main_frame, text="Test Name:*", font=("", 10, "bold")).grid(
            row=1, column=0, sticky=tk.W, pady=5)
        self.test_name_var = tk.StringVar()
        test_name_entry = ttk.Entry(main_frame, textvariable=self.test_name_var, width=40)
        test_name_entry.grid(row=1, column=1, sticky=(tk.W, tk.E), pady=5)
        test_name_entry.focus()
        
        # Technician Name (Required)
        ttk.Label(main_frame, text="Technician:*", font=("", 10, "bold")).grid(
            row=2, column=0, sticky=tk.W, pady=5)
        self.technician_var = tk.StringVar(value=last_technician)
        
        if self.recent_technicians:
            technician_combo = ttk.Combobox(main_frame, textvariable=self.technician_var, 
                                           width=37, values=self.recent_technicians)
            technician_combo.grid(row=2, column=1, sticky=(tk.W, tk.E), pady=5)
        else:
            technician_entry = ttk.Entry(main_frame, textvariable=self.technician_var, width=40)
            technician_entry.grid(row=2, column=1, sticky=(tk.W, tk.E), pady=5)
        
        # Date/Time (Auto-filled, display only)
        ttk.Label(main_frame, text="Date/Time:", font=("", 10)).grid(
            row=3, column=0, sticky=tk.W, pady=5)
        self.datetime_var = tk.StringVar(value=datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        datetime_label = ttk.Label(main_frame, textvariable=self.datetime_var, 
                                   foreground="gray")
        datetime_label.grid(row=3, column=1, sticky=tk.W, pady=5)
        
        # Notes (Optional)
        ttk.Label(main_frame, text="Notes:", font=("", 10)).grid(
            row=4, column=0, sticky=(tk.W, tk.N), pady=5)
        self.notes_text = tk.Text(main_frame, height=8, width=40, wrap=tk.WORD)
        self.notes_text.grid(row=4, column=1, sticky=(tk.W, tk.E), pady=5)
        
        # Scrollbar for notes
        notes_scroll = ttk.Scrollbar(main_frame, command=self.notes_text.yview)
        notes_scroll.grid(row=4, column=2, sticky=(tk.N, tk.S))
        self.notes_text.config(yscrollcommand=notes_scroll.set)
        
        # Required fields notice
        required_label = ttk.Label(main_frame, text="* Required fields", 
                                   font=("", 8), foreground="red")
        required_label.grid(row=5, column=0, columnspan=2, sticky=tk.W, pady=(10, 5))
        
        # Buttons frame
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=6, column=0, columnspan=2, pady=(20, 0))
        
        ttk.Button(button_frame, text="Start Test", command=self._on_ok, 
                  width=15).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Cancel", command=self._on_cancel, 
                  width=15).pack(side=tk.LEFT, padx=5)
        
        # Configure grid weights
        main_frame.columnconfigure(1, weight=1)
        
        # Bind Enter key to OK button
        self.dialog.bind('<Return>', lambda e: self._on_ok())
        self.dialog.bind('<Escape>', lambda e: self._on_cancel())
    
    def _on_ok(self):
        """Handle OK button click"""
        test_name = self.test_name_var.get().strip()
        technician = self.technician_var.get().strip()
        notes = self.notes_text.get("1.0", tk.END).strip()
        
        # Validate required fields
        if not test_name:
            messagebox.showerror("Missing Information", 
                               "Test Name is required.", parent=self.dialog)
            return
        
        if not technician:
            messagebox.showerror("Missing Information", 
                               "Technician name is required.", parent=self.dialog)
            return
        
        # Store result
        self.result = {
            "test_name": test_name,
            "technician": technician,
            "datetime": self.datetime_var.get(),
            "notes": notes
        }
        
        self.dialog.destroy()
    
    def _on_cancel(self):
        """Handle Cancel button click"""
        self.result = None
        self.dialog.destroy()
    
    def get_result(self) -> Optional[Dict[str, str]]:
        """Get dialog result
        
        Returns:
            Dictionary with metadata if OK clicked, None if cancelled
        """
        return self.result
