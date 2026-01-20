"""
Metadata Edit Dialog
Edit existing test metadata
"""

import tkinter as tk
from tkinter import ttk, messagebox
from typing import Optional, Dict


class MetadataEditDialog:
    """Dialog for editing existing test metadata"""
    
    def __init__(self, parent, metadata: Dict[str, str]):
        """Initialize edit dialog
        
        Args:
            parent: Parent window
            metadata: Existing metadata dictionary
        """
        self.result: Optional[Dict[str, str]] = None
        self.original_metadata = metadata
        
        # Create modal dialog
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Edit Test Metadata")
        self.dialog.geometry("500x400")
        self.dialog.transient(parent)
        self.dialog.grab_set()
        
        # Center on parent
        self.dialog.update_idletasks()
        x = parent.winfo_x() + (parent.winfo_width() // 2) - (self.dialog.winfo_width() // 2)
        y = parent.winfo_y() + (parent.winfo_height() // 2) - (self.dialog.winfo_height() // 2)
        self.dialog.geometry(f"+{x}+{y}")
        
        self._create_widgets()
        
        # Make dialog modal
        self.dialog.protocol("WM_DELETE_WINDOW", self._on_cancel)
        self.dialog.wait_window()
    
    def _create_widgets(self):
        """Create dialog widgets"""
        # Main frame
        main_frame = ttk.Frame(self.dialog, padding="20")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Title
        title_label = ttk.Label(main_frame, text="Edit Test Metadata", 
                               font=("", 12, "bold"))
        title_label.grid(row=0, column=0, columnspan=2, pady=(0, 20))
        
        # Test Name (Required)
        ttk.Label(main_frame, text="Test Name:*", font=("", 10, "bold")).grid(
            row=1, column=0, sticky=tk.W, pady=5)
        self.test_name_var = tk.StringVar(value=self.original_metadata.get('test_name', ''))
        test_name_entry = ttk.Entry(main_frame, textvariable=self.test_name_var, width=40)
        test_name_entry.grid(row=1, column=1, sticky=(tk.W, tk.E), pady=5)
        test_name_entry.focus()
        
        # Technician Name (Required)
        ttk.Label(main_frame, text="Technician:*", font=("", 10, "bold")).grid(
            row=2, column=0, sticky=tk.W, pady=5)
        self.technician_var = tk.StringVar(value=self.original_metadata.get('technician', ''))
        technician_entry = ttk.Entry(main_frame, textvariable=self.technician_var, width=40)
        technician_entry.grid(row=2, column=1, sticky=(tk.W, tk.E), pady=5)
        
        # Date/Time (Display only - cannot edit)
        ttk.Label(main_frame, text="Date/Time:", font=("", 10)).grid(
            row=3, column=0, sticky=tk.W, pady=5)
        datetime_label = ttk.Label(main_frame, 
                                   text=self.original_metadata.get('datetime', 'Unknown'),
                                   foreground="gray")
        datetime_label.grid(row=3, column=1, sticky=tk.W, pady=5)
        
        # Peak Force (Display only - cannot edit)
        ttk.Label(main_frame, text="Peak Force:", font=("", 10)).grid(
            row=4, column=0, sticky=tk.W, pady=5)
        peak_label = ttk.Label(main_frame,
                              text=self.original_metadata.get('peak_force', 'Unknown'),
                              foreground="gray")
        peak_label.grid(row=4, column=1, sticky=tk.W, pady=5)
        
        # Notes (Optional)
        ttk.Label(main_frame, text="Notes:", font=("", 10)).grid(
            row=5, column=0, sticky=(tk.W, tk.N), pady=5)
        self.notes_text = tk.Text(main_frame, height=8, width=40, wrap=tk.WORD)
        self.notes_text.grid(row=5, column=1, sticky=(tk.W, tk.E), pady=5)
        self.notes_text.insert("1.0", self.original_metadata.get('notes', ''))
        
        # Scrollbar for notes
        notes_scroll = ttk.Scrollbar(main_frame, command=self.notes_text.yview)
        notes_scroll.grid(row=5, column=2, sticky=(tk.N, tk.S))
        self.notes_text.config(yscrollcommand=notes_scroll.set)
        
        # Required fields notice
        required_label = ttk.Label(main_frame, text="* Required fields", 
                                   font=("", 8), foreground="red")
        required_label.grid(row=6, column=0, columnspan=2, sticky=tk.W, pady=(10, 5))
        
        # Buttons frame
        button_frame = ttk.Frame(main_frame)
        button_frame.grid(row=7, column=0, columnspan=2, pady=(20, 0))
        
        ttk.Button(button_frame, text="Save Changes", command=self._on_ok, 
                  width=15).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Cancel", command=self._on_cancel, 
                  width=15).pack(side=tk.LEFT, padx=5)
        
        # Configure grid weights
        main_frame.columnconfigure(1, weight=1)
        
        # Bind keys
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
        
        # Store result (preserve original datetime and peak_force)
        self.result = {
            "test_name": test_name,
            "technician": technician,
            "datetime": self.original_metadata.get('datetime', ''),
            "peak_force": self.original_metadata.get('peak_force', ''),
            "notes": notes,
            "filepath": self.original_metadata.get('filepath', '')
        }
        
        self.dialog.destroy()
    
    def _on_cancel(self):
        """Handle Cancel button click"""
        self.result = None
        self.dialog.destroy()
    
    def get_result(self) -> Optional[Dict[str, str]]:
        """Get dialog result
        
        Returns:
            Dictionary with updated metadata if OK clicked, None if cancelled
        """
        return self.result
