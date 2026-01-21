"""
Test Browser UI Component
Displays list of all tests with metadata and selection capabilities
"""

import tkinter as tk
from tkinter import ttk, messagebox
from typing import List, Dict, Callable, Optional
from pathlib import Path
from datetime import datetime


class TestBrowser(ttk.Frame):
    """Test browser with tree view and selection"""
    
    def __init__(self, parent, test_manager, on_calculate_stats: Callable = None,
                 on_edit_metadata: Callable = None, on_load_test: Callable = None):
        """Initialize test browser
        
        Args:
            parent: Parent widget
            test_manager: TestManager instance
            on_calculate_stats: Callback for calculate statistics button
            on_edit_metadata: Callback for edit metadata button
            on_load_test: Callback for load test button
        """
        super().__init__(parent)
        
        self.test_manager = test_manager
        self.on_calculate_stats = on_calculate_stats
        self.on_edit_metadata = on_edit_metadata
        self.on_load_test = on_load_test
        self.selected_tests: List[Dict[str, str]] = []
        
        self._create_widgets()
    
    def _create_widgets(self):
        """Create browser widgets"""
        # Top toolbar
        toolbar = ttk.Frame(self)
        toolbar.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Button(toolbar, text="Refresh", command=self.refresh).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Load Test", 
                  command=self._on_load_test_clicked).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Calculate Statistics", 
                  command=self._on_calc_stats_clicked).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Edit Metadata",
                  command=self._on_edit_clicked).pack(side=tk.LEFT, padx=2)
        
        self.selection_label = ttk.Label(toolbar, text="0 tests selected")
        self.selection_label.pack(side=tk.RIGHT, padx=5)
        
        # Main content: tree view and metadata preview
        content_paned = ttk.PanedWindow(self, orient=tk.VERTICAL)
        content_paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # Tree view frame
        tree_frame = ttk.Frame(content_paned)
        content_paned.add(tree_frame, weight=3)
        
        # Tree view with scrollbars
        tree_scroll_y = ttk.Scrollbar(tree_frame)
        tree_scroll_y.pack(side=tk.RIGHT, fill=tk.Y)
        
        tree_scroll_x = ttk.Scrollbar(tree_frame, orient=tk.HORIZONTAL)
        tree_scroll_x.pack(side=tk.BOTTOM, fill=tk.X)
        
        self.tree = ttk.Treeview(tree_frame, 
                                 columns=('test_name', 'project', 'technician', 'peak', 'date', 'time'),
                                 yscrollcommand=tree_scroll_y.set,
                                 xscrollcommand=tree_scroll_x.set,
                                 selectmode='extended')
        self.tree.pack(fill=tk.BOTH, expand=True)
        
        tree_scroll_y.config(command=self.tree.yview)
        tree_scroll_x.config(command=self.tree.xview)
        
        # Configure columns
        self.tree.heading('#0', text='☐ Select')
        self.tree.heading('test_name', text='Test Name')
        self.tree.heading('project', text='Project')
        self.tree.heading('technician', text='Technician')
        self.tree.heading('peak', text='Peak (kN)')
        self.tree.heading('date', text='Date')
        self.tree.heading('time', text='Time')
        
        self.tree.column('#0', width=80)
        self.tree.column('test_name', width=120)
        self.tree.column('project', width=240)
        self.tree.column('technician', width=150)
        self.tree.column('peak', width=100)
        self.tree.column('date', width=110)
        self.tree.column('time', width=80)
        
        # Center align all columns
        self.tree.column('#0', anchor='center')
        self.tree.column('test_name', anchor='center')
        self.tree.column('project', anchor='center')
        self.tree.column('technician', anchor='center')
        self.tree.column('peak', anchor='center')
        self.tree.column('date', anchor='center')
        self.tree.column('time', anchor='center')
        
        # Bind events
        self.tree.bind('<Button-1>', self._on_tree_click)
        self.tree.bind('<<TreeviewSelect>>', self._on_tree_select)
        
        # Bind header click for "Select All"
        self.tree.bind('<Button-1>', self._on_header_or_tree_click, add='+')
        
        # Metadata preview frame
        preview_frame = ttk.LabelFrame(content_paned, text="Test Details", padding="10")
        content_paned.add(preview_frame, weight=1)
        
        # Metadata text widget
        preview_scroll = ttk.Scrollbar(preview_frame)
        preview_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.preview_text = tk.Text(preview_frame, height=8, wrap=tk.WORD,
                                    yscrollcommand=preview_scroll.set, font=("", 9))
        self.preview_text.pack(fill=tk.BOTH, expand=True)
        preview_scroll.config(command=self.preview_text.yview)
        
        # Track checked items
        self.checked_items = set()
        self.all_selected = False  # Track if all tests are selected
    
    def refresh(self):
        """Refresh the test list"""
        # Clear tree
        for item in self.tree.get_children():
            self.tree.delete(item)
        
        self.checked_items.clear()
        self.all_selected = False
        
        # Update header
        self.tree.heading('#0', text='☐ Select All')
        
        # Get all tests organized by date
        all_tests = self.test_manager.list_all_tests()
        
        # Group by date
        tests_by_date = {}
        for test in all_tests:
            date_folder = test.get('date_folder', 'Unknown')
            if date_folder not in tests_by_date:
                tests_by_date[date_folder] = []
            tests_by_date[date_folder].append(test)
        
        # Add to tree
        for date_folder in sorted(tests_by_date.keys(), reverse=True):
            # Add date folder as parent
            date_id = self.tree.insert('', 'end', text=f'☐ {date_folder}', 
                                      values=('', '', '', '', '', ''), tags=('date',))
            
            # Add tests under date folder
            for test in tests_by_date[date_folder]:
                # Parse datetime and format date and time
                datetime_str = test.get('datetime', '')
                date_formatted = ''
                time_only = ''
                
                if datetime_str:
                    try:
                        dt = datetime.strptime(datetime_str, '%Y-%m-%d %H:%M:%S')
                        date_formatted = dt.strftime('%b %d, %Y')  # e.g., "Jan 20, 2026"
                        time_only = dt.strftime('%H:%M:%S')
                    except ValueError:
                        # Fallback if parsing fails
                        parts = datetime_str.split()
                        date_formatted = parts[0] if len(parts) > 0 else ''
                        time_only = parts[-1] if len(parts) > 1 else ''
                
                test_id = self.tree.insert(date_id, 'end', text='☐',
                                          values=(test.get('test_name', ''),
                                                 test.get('project', ''),
                                                 test.get('technician', ''),
                                                 test.get('peak_force', ''),
                                                 date_formatted,
                                                 time_only),
                                          tags=('test', str(test)))
        
        self._update_selection_label()
    
    def _on_header_or_tree_click(self, event):
        """Handle clicks on header or tree items"""
        region = self.tree.identify_region(event.x, event.y)
        
        # Check if clicked on header
        if region == 'heading':
            column = self.tree.identify_column(event.x)
            if column == '#0':  # Clicked on Select All header
                self._toggle_select_all()
                return
        
        # Handle tree item clicks
        if region == 'tree':
            item = self.tree.identify_row(event.y)
            if item:
                tags = self.tree.item(item, 'tags')
                
                # Check if it's a date folder
                if 'date' in tags:
                    self._toggle_date_folder(item)
                # Check if it's a test
                elif 'test' in tags:
                    self._toggle_test(item)
    
    def _toggle_select_all(self):
        """Toggle selection of all tests"""
        self.all_selected = not self.all_selected
        
        if self.all_selected:
            # Select all tests
            self.checked_items.clear()
            for date_item in self.tree.get_children():
                self.tree.item(date_item, text='☑ ' + self.tree.item(date_item, 'text').lstrip('☐☑ '))
                for test_item in self.tree.get_children(date_item):
                    self.checked_items.add(test_item)
                    self.tree.item(test_item, text='☑')
            self.tree.heading('#0', text='☑ Select All')
        else:
            # Deselect all tests
            self.checked_items.clear()
            for date_item in self.tree.get_children():
                self.tree.item(date_item, text='☐ ' + self.tree.item(date_item, 'text').lstrip('☐☑ '))
                for test_item in self.tree.get_children(date_item):
                    self.tree.item(test_item, text='☐')
            self.tree.heading('#0', text='☐ Select All')
        
        self._update_selection_label()
    
    def _toggle_date_folder(self, date_item):
        """Toggle selection of all tests in a date folder"""
        children = self.tree.get_children(date_item)
        if not children:
            return
        
        # Check if all children are currently selected
        all_checked = all(child in self.checked_items for child in children)
        
        if all_checked:
            # Deselect all in this folder
            for child in children:
                if child in self.checked_items:
                    self.checked_items.remove(child)
                self.tree.item(child, text='☐')
            self.tree.item(date_item, text='☐ ' + self.tree.item(date_item, 'text').lstrip('☐☑ '))
        else:
            # Select all in this folder
            for child in children:
                self.checked_items.add(child)
                self.tree.item(child, text='☑')
            self.tree.item(date_item, text='☑ ' + self.tree.item(date_item, 'text').lstrip('☐☑ '))
        
        self._update_all_selected_state()
        self._update_selection_label()
    
    def _toggle_test(self, test_item):
        """Toggle selection of a single test"""
        if test_item in self.checked_items:
            self.checked_items.remove(test_item)
            self.tree.item(test_item, text='☐')
        else:
            self.checked_items.add(test_item)
            self.tree.item(test_item, text='☑')
        
        # Update parent date folder checkbox
        parent = self.tree.parent(test_item)
        if parent:
            children = self.tree.get_children(parent)
            all_checked = all(child in self.checked_items for child in children)
            any_checked = any(child in self.checked_items for child in children)
            
            if all_checked:
                self.tree.item(parent, text='☑ ' + self.tree.item(parent, 'text').lstrip('☐☑ '))
            elif any_checked:
                self.tree.item(parent, text='☑ ' + self.tree.item(parent, 'text').lstrip('☐☑ '))
            else:
                self.tree.item(parent, text='☐ ' + self.tree.item(parent, 'text').lstrip('☐☑ '))
        
        self._update_all_selected_state()
        self._update_selection_label()
    
    def _update_all_selected_state(self):
        """Update the 'Select All' header checkbox state"""
        # Get all test items
        all_test_items = []
        for date_item in self.tree.get_children():
            all_test_items.extend(self.tree.get_children(date_item))
        
        if all_test_items and len(self.checked_items) == len(all_test_items):
            self.all_selected = True
            self.tree.heading('#0', text='☑ Select All')
        else:
            self.all_selected = False
            self.tree.heading('#0', text='☐ Select All')
    
    def _on_tree_click(self, event):
        """Handle tree click for checkbox toggle (legacy, now handled by _on_header_or_tree_click)"""
        pass
    
    def _on_tree_select(self, event):
        """Handle tree selection for preview"""
        selected = self.tree.selection()
        if selected:
            item = selected[0]
            tags = self.tree.item(item, 'tags')
            
            if 'test' in tags and len(tags) > 1:
                # Extract metadata from tags
                metadata_str = tags[1]
                try:
                    metadata = eval(metadata_str)
                    self._show_metadata_preview(metadata)
                except:
                    pass
    
    def _show_metadata_preview(self, metadata: Dict[str, str]):
        """Show metadata in preview pane"""
        self.preview_text.delete('1.0', tk.END)
        
        preview = f"Test Name: {metadata.get('test_name', 'N/A')}\n"
        preview += f"Date/Time: {metadata.get('datetime', 'N/A')}\n"
        preview += f"Technician: {metadata.get('technician', 'N/A')}\n"
        preview += f"Peak Force: {metadata.get('peak_force', 'N/A')}\n"
        preview += f"\nNotes:\n{metadata.get('notes', 'None')}\n"
        preview += f"\nFile: {metadata.get('filepath', 'N/A')}"
        
        self.preview_text.insert('1.0', preview)
    
    def _update_selection_label(self):
        """Update selection count label"""
        count = len(self.checked_items)
        self.selection_label.config(text=f"{count} test{'s' if count != 1 else ''} selected")
    
    def _on_calc_stats_clicked(self):
        """Handle calculate statistics button"""
        if not self.checked_items:
            messagebox.showwarning("No Selection", 
                                 "Please select at least one test for statistical analysis.",
                                 parent=self)
            return
        
        # Get metadata for selected tests
        selected_metadata = []
        for item in self.checked_items:
            tags = self.tree.item(item, 'tags')
            if 'test' in tags and len(tags) > 1:
                try:
                    metadata = eval(tags[1])
                    selected_metadata.append(metadata)
                except:
                    pass
        
        if self.on_calculate_stats:
            self.on_calculate_stats(selected_metadata)
    
    def _on_load_test_clicked(self):
        """Handle load test button"""
        # Get the currently selected item (not checked items)
        selected = self.tree.selection()
        if not selected:
            messagebox.showwarning("No Selection",
                                 "Please select a single test to load.",
                                 parent=self)
            return
        
        item = selected[0]
        tags = self.tree.item(item, 'tags')
        
        if 'test' not in tags:
            messagebox.showwarning("Invalid Selection",
                                 "Please select a test (not a date folder) to load.",
                                 parent=self)
            return
        
        if len(tags) > 1:
            try:
                metadata = eval(tags[1])
                if self.on_load_test:
                    self.on_load_test(metadata)
            except:
                messagebox.showerror("Error", "Failed to load test data.",
                                   parent=self)
    
    def _on_edit_clicked(self):
        """Handle edit metadata button"""
        selected = self.tree.selection()
        if not selected:
            messagebox.showwarning("No Selection",
                                 "Please select a test to edit.",
                                 parent=self)
            return
        
        item = selected[0]
        tags = self.tree.item(item, 'tags')
        
        if 'test' not in tags:
            messagebox.showwarning("Invalid Selection",
                                 "Please select a test (not a date folder).",
                                 parent=self)
            return
        
        if len(tags) > 1:
            try:
                metadata = eval(tags[1])
                if self.on_edit_metadata:
                    self.on_edit_metadata(metadata)
            except:
                messagebox.showerror("Error", "Failed to load test metadata.",
                                   parent=self)
