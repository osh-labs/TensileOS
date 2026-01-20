"""
Statistics Results Window
Displays statistical analysis with visualization
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import matplotlib
matplotlib.use('TkAgg')
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from typing import List, Dict
import csv
from datetime import datetime
from reportlab.lib.pagesizes import letter
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import inch
from reportlab.platypus import SimpleDocTemplate, Table, TableStyle, Paragraph, Spacer, PageBreak, Image
from reportlab.lib.enums import TA_CENTER, TA_LEFT, TA_RIGHT
import io


class StatisticsWindow:
    """Window displaying statistical analysis results"""
    
    def __init__(self, parent, test_metadata_list: List[Dict[str, str]], statistics_calc):
        """Initialize statistics window
        
        Args:
            parent: Parent window
            test_metadata_list: List of test metadata dictionaries
            statistics_calc: TestStatistics instance with calculated values
        """
        self.test_metadata_list = test_metadata_list
        self.stats = statistics_calc
        self.chart_figure = None  # Store figure for PDF export
        
        # Create window
        self.window = tk.Toplevel(parent)
        self.window.title("Statistical Analysis Results")
        self.window.geometry("1000x1200")
        self.window.transient(parent)
        
        # Center on parent
        self.window.update_idletasks()
        x = parent.winfo_x() + (parent.winfo_width() // 2) - (self.window.winfo_width() // 2)
        y = parent.winfo_y() + (parent.winfo_height() // 2) - (self.window.winfo_height() // 2)
        self.window.geometry(f"+{x}+{y}")
        
        self._create_widgets()
    
    def _create_widgets(self):
        """Create window widgets"""
        # Main container
        main_frame = ttk.Frame(self.window, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Title
        title_label = ttk.Label(main_frame, text="Statistical Analysis", 
                               font=("", 14, "bold"))
        title_label.pack(pady=(0, 10))
        
        # Summary statistics frame
        summary_frame = ttk.LabelFrame(main_frame, text="Summary Statistics", padding="10")
        summary_frame.pack(fill=tk.X, pady=(0, 10))
        
        summary = self.stats.get_summary()
        mean, lower_3sigma, upper_3sigma = self.stats.calculate_3sigma()
        three_sigma_avg = mean - (3 * summary['std_dev'])
        
        # Create summary grid
        stats_grid = ttk.Frame(summary_frame)
        stats_grid.pack(fill=tk.X)
        
        row = 0
        # Number of Tests
        ttk.Label(stats_grid, text="Number of Tests:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{summary['count']}", font=("", 10)).grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        row += 1
        # Average Peak Force
        ttk.Label(stats_grid, text="Average Peak Force:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{summary['mean']:.3f} kN", 
                 font=("", 10), foreground="#2196F3").grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        row += 1
        # Median Peak Force
        ttk.Label(stats_grid, text="Median Peak Force:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{summary['median']:.3f} kN", font=("", 10)).grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        row += 1
        # Min / Max Force
        ttk.Label(stats_grid, text="Min / Max Force:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{summary['min']:.3f} / {summary['max']:.3f} kN", font=("", 10)).grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        row += 1
        # Horizontal divider
        ttk.Separator(stats_grid, orient=tk.HORIZONTAL).grid(
            row=row, column=0, columnspan=2, sticky=tk.EW, pady=10)
        
        row += 1
        # Standard Deviation
        ttk.Label(stats_grid, text="Standard Deviation:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{summary['std_dev']:.3f} kN", font=("", 10)).grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        row += 1
        # 3-Sigma Range
        ttk.Label(stats_grid, text="3-Sigma Range:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{lower_3sigma:.3f} to {upper_3sigma:.3f} kN",
                 font=("", 10), foreground="#FF5722").grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        row += 1
        # 3-Sigma Average Peak Force (mean - 3*std_dev)
        ttk.Label(stats_grid, text="3-Sigma Average Peak Force:", font=("", 10, "bold")).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        ttk.Label(stats_grid, text=f"{three_sigma_avg:.3f} kN", 
                 font=("", 10), foreground="#FF5722").grid(
            row=row, column=1, sticky=tk.W, padx=5, pady=2)
        
        # Chart frame
        chart_frame = ttk.LabelFrame(main_frame, text="Peak Force Distribution", padding="10")
        chart_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 10))
        
        self._create_chart(chart_frame)
        
        # Individual tests table
        table_frame = ttk.LabelFrame(main_frame, text="Individual Test Results", padding="10")
        table_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 10))
        
        self._create_table(table_frame)
        
        # Buttons
        button_frame = ttk.Frame(main_frame)
        button_frame.pack(fill=tk.X, pady=(5, 0))
        
        ttk.Button(button_frame, text="Export Report", 
                  command=self._export_report).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Close", 
                  command=self.window.destroy).pack(side=tk.RIGHT, padx=5)
    
    def _create_chart(self, parent):
        """Create bar chart with mean and 3-sigma lines"""
        # Create matplotlib figure with reasonable size for GUI
        figure = Figure(figsize=(9, 3.5), dpi=100)
        self.chart_figure = figure  # Store for PDF export
        ax = figure.add_subplot(111)
        
        # Get data
        deviations = self.stats.get_deviations()
        test_names = [d[0][:20] for d in deviations]  # Truncate names
        peak_forces = [d[1] for d in deviations]
        
        mean, lower_3sigma, upper_3sigma = self.stats.calculate_3sigma()
        
        # Create bar chart
        bars = ax.bar(range(len(peak_forces)), peak_forces, color='#2196F3', alpha=0.7)
        
        # Add mean line
        ax.axhline(y=mean, color='#4CAF50', linestyle='--', linewidth=2, label='Mean')
        
        # Add 3-sigma bounds
        ax.axhline(y=upper_3sigma, color='#FF5722', linestyle=':', linewidth=2, label='+3\u03c3')
        ax.axhline(y=lower_3sigma, color='#FF5722', linestyle=':', linewidth=2, label='-3\u03c3')
        
        # Formatting
        ax.set_xlabel('Test', fontsize=10)
        ax.set_ylabel('Peak Force (kN)', fontsize=10)
        ax.set_title('Peak Forces with Mean and 3-Sigma Bounds', fontsize=12, fontweight='bold')
        ax.set_xticks(range(len(test_names)))
        ax.set_xticklabels(test_names, rotation=45, ha='right', fontsize=8)
        # Add legend with padding
        ax.legend(loc='center left', bbox_to_anchor=(1.02, 0.5), borderaxespad=0, frameon=True)
        ax.grid(True, alpha=0.3)
        
        # Adjust layout to prevent legend cutoff with more right margin
        figure.subplots_adjust(left=0.08, right=0.78, top=0.92, bottom=0.12)
        
        # Embed in tkinter
        canvas = FigureCanvasTkAgg(figure, master=parent)
        canvas.draw()
        canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
    
    def _create_table(self, parent):
        """Create table of individual test results"""
        # Scrollbars
        scroll_y = ttk.Scrollbar(parent)
        scroll_y.pack(side=tk.RIGHT, fill=tk.Y)
        
        scroll_x = ttk.Scrollbar(parent, orient=tk.HORIZONTAL)
        scroll_x.pack(side=tk.BOTTOM, fill=tk.X)
        
        # Treeview
        tree = ttk.Treeview(parent, 
                           columns=('test_name', 'technician', 'peak', 'deviation'),
                           yscrollcommand=scroll_y.set,
                           xscrollcommand=scroll_x.set,
                           height=8)
        tree.pack(fill=tk.BOTH, expand=True)
        
        scroll_y.config(command=tree.yview)
        scroll_x.config(command=tree.xview)
        
        # Configure columns
        tree.heading('#0', text='#')
        tree.heading('test_name', text='Test Name')
        tree.heading('technician', text='Technician')
        tree.heading('peak', text='Peak (kN)')
        tree.heading('deviation', text='Deviation from Mean')
        
        tree.column('#0', width=40)
        tree.column('test_name', width=200)
        tree.column('technician', width=150)
        tree.column('peak', width=100)
        tree.column('deviation', width=150)
        
        # Populate with data
        deviations = self.stats.get_deviations()
        for i, (test_name, peak, deviation) in enumerate(deviations, 1):
            # Get technician from metadata
            technician = ''
            for test in self.test_metadata_list:
                if test.get('test_name') == test_name:
                    technician = test.get('technician', '')
                    break
            
            deviation_str = f"{deviation:+.3f} kN"
            tree.insert('', 'end', text=str(i),
                       values=(test_name, technician, f"{peak:.3f}", deviation_str))
    
    def _export_report(self):
        """Export statistical report to PDF"""
        filename = filedialog.asksaveasfilename(
            parent=self.window,
            title="Export Statistics Report",
            defaultextension=".pdf",
            filetypes=[("PDF files", "*.pdf"), ("All files", "*.*")],
            initialfile=f"statistics_report_{datetime.now().strftime('%Y%m%d_%H%M%S')}.pdf"
        )
        
        if not filename:
            return
        
        try:
            # Create PDF
            doc = SimpleDocTemplate(filename, pagesize=letter,
                                  rightMargin=72, leftMargin=72,
                                  topMargin=72, bottomMargin=72)
            
            # Container for elements
            elements = []
            styles = getSampleStyleSheet()
            
            # Custom styles
            title_style = ParagraphStyle(
                'CustomTitle',
                parent=styles['Heading1'],
                fontSize=18,
                textColor=colors.HexColor('#2196F3'),
                spaceAfter=10,
                alignment=TA_CENTER
            )
            
            heading_style = ParagraphStyle(
                'CustomHeading',
                parent=styles['Heading2'],
                fontSize=12,
                textColor=colors.HexColor('#424242'),
                spaceAfter=6,
                spaceBefore=10
            )
            
            # Title
            now = datetime.now()
            elements.append(Paragraph("Tensitle Testing Statistical Analysis Report", title_style))
            elements.append(Paragraph(f"Generated: {now.strftime('%Y-%m-%d %H:%M:%S')}", 
                                    styles['Normal']))
            elements.append(Spacer(1, 0.15*inch))
            
            # Summary Statistics
            elements.append(Paragraph("Summary Statistics", heading_style))
            
            summary = self.stats.get_summary()
            mean, lower_3sigma, upper_3sigma = self.stats.calculate_3sigma()
            three_sigma_avg = mean - (3 * summary['std_dev'])
            
            summary_data = [
                ['Number of Tests:', f"{summary['count']}"],
                ['Average Peak Force:', f"{summary['mean']:.3f} kN"],
                ['Median Peak Force:', f"{summary['median']:.3f} kN"],
                ['Min / Max Force:', f"{summary['min']:.3f} / {summary['max']:.3f} kN"],
                ['', ''],  # Divider row
                ['Standard Deviation:', f"{summary['std_dev']:.3f} kN"],
                ['3-Sigma Range:', f"{lower_3sigma:.3f} to {upper_3sigma:.3f} kN"],
                ['3-Sigma Average Peak Force:', f"{three_sigma_avg:.3f} kN"],
            ]
            
            summary_table = Table(summary_data, colWidths=[2.5*inch, 2.5*inch])
            summary_table.setStyle(TableStyle([
                ('ALIGN', (0, 0), (-1, -1), 'LEFT'),
                ('FONTNAME', (0, 0), (0, -1), 'Helvetica-Bold'),
                ('FONTNAME', (1, 0), (1, -1), 'Helvetica'),
                ('FONTSIZE', (0, 0), (-1, -1), 10),
                ('BOTTOMPADDING', (0, 0), (-1, -1), 4),
                ('TOPPADDING', (0, 0), (-1, -1), 4),
                ('LINEBELOW', (0, 4), (-1, 4), 1, colors.grey),  # Divider line
                ('TEXTCOLOR', (0, 1), (-1, 1), colors.HexColor('#2196F3')),  # Average in blue
                ('TEXTCOLOR', (0, 6), (-1, 7), colors.HexColor('#FF5722')),  # 3-sigma in red
            ]))
            elements.append(summary_table)
            elements.append(Spacer(1, 0.15*inch))
            
            # Add chart image - create new chart specifically for PDF export
            elements.append(Paragraph("Peak Force Distribution", heading_style))
            
            # Create new figure with exact PDF dimensions at 150 DPI
            pdf_figure = Figure(figsize=(6, 2.6), dpi=150)
            pdf_ax = pdf_figure.add_subplot(111)
            
            # Get data
            deviations = self.stats.get_deviations()
            test_names = [d[0][:20] for d in deviations]
            peak_forces = [d[1] for d in deviations]
            
            mean, lower_3sigma, upper_3sigma = self.stats.calculate_3sigma()
            
            # Create bar chart
            pdf_ax.bar(range(len(peak_forces)), peak_forces, color='#2196F3', alpha=0.7)
            
            # Add mean line
            pdf_ax.axhline(y=mean, color='#4CAF50', linestyle='--', linewidth=2, label='Mean')
            
            # Add 3-sigma bounds
            pdf_ax.axhline(y=upper_3sigma, color='#FF5722', linestyle=':', linewidth=2, label='+3σ')
            pdf_ax.axhline(y=lower_3sigma, color='#FF5722', linestyle=':', linewidth=2, label='-3σ')
            
            # Formatting
            pdf_ax.set_xlabel('Test', fontsize=9)
            pdf_ax.set_ylabel('Peak Force (kN)', fontsize=9)
            pdf_ax.set_title('Peak Forces with Mean and 3-Sigma Bounds', fontsize=10, fontweight='bold')
            pdf_ax.set_xticks(range(len(test_names)))
            pdf_ax.set_xticklabels(test_names, rotation=45, ha='right', fontsize=7)
            pdf_ax.legend(loc='center left', bbox_to_anchor=(1.02, 0.5), borderaxespad=0, frameon=True, fontsize=8)
            pdf_ax.grid(True, alpha=0.3)
            
            # Adjust layout
            pdf_figure.subplots_adjust(left=0.08, right=0.78, top=0.88, bottom=0.18)
            
            # Save to BytesIO
            img_buffer = io.BytesIO()
            pdf_figure.savefig(img_buffer, format='png', dpi=150)
            img_buffer.seek(0)
            
            # Add to PDF with exact dimensions
            img = Image(img_buffer, width=6*inch, height=2.6*inch)
            elements.append(img)
            elements.append(Spacer(1, 0.15*inch))
            
            # Individual Test Results
            elements.append(Paragraph("Individual Test Results", heading_style))
            
            test_data = [['#', 'Test Name', 'Technician', 'Peak (kN)', 'Deviation']]
            
            deviations = self.stats.get_deviations()
            for i, (test_name, peak, deviation) in enumerate(deviations, 1):
                technician = ''
                for test in self.test_metadata_list:
                    if test.get('test_name') == test_name:
                        technician = test.get('technician', '')
                        break
                
                test_data.append([
                    str(i),
                    test_name[:30],  # Truncate if too long
                    technician,
                    f"{peak:.3f}",
                    f"{deviation:+.3f}"
                ])
            
            test_table = Table(test_data, colWidths=[0.3*inch, 2*inch, 1.4*inch, 0.9*inch, 0.9*inch])
            test_table.setStyle(TableStyle([
                ('ALIGN', (0, 0), (-1, -1), 'LEFT'),
                ('ALIGN', (0, 0), (0, -1), 'CENTER'),  # Center # column
                ('ALIGN', (3, 0), (4, -1), 'RIGHT'),  # Right-align numbers
                ('FONTNAME', (0, 0), (-1, 0), 'Helvetica-Bold'),
                ('FONTNAME', (0, 1), (-1, -1), 'Helvetica'),
                ('FONTSIZE', (0, 0), (-1, -1), 9),
                ('BOTTOMPADDING', (0, 0), (-1, -1), 4),
                ('TOPPADDING', (0, 0), (-1, -1), 4),
                ('GRID', (0, 0), (-1, -1), 0.5, colors.grey),
                ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#E3F2FD')),
                ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#F5F5F5')]),
            ]))
            elements.append(test_table)
            
            # Build PDF with custom footer
            def add_footer(canvas, doc):
                """Add footer to each page"""
                canvas.saveState()
                footer_text = "Created via TensileOS"
                canvas.setFont('Helvetica', 9)
                canvas.setFillColor(colors.grey)
                canvas.drawCentredString(letter[0]/2, 0.5*inch, footer_text)
                canvas.restoreState()
            
            doc.build(elements, onFirstPage=add_footer, onLaterPages=add_footer)
            
            messagebox.showinfo("Export Successful", 
                              f"Statistics report exported to:\n{filename}",
                              parent=self.window)
        
        except Exception as e:
            messagebox.showerror("Export Error", 
                               f"Failed to export report:\n{str(e)}",
                               parent=self.window)
