# TensileCompanion Multi-Test Feature Testing Guide

## Overview
This document describes how to test the multi-test management feature that includes metadata capture, date-organized storage, test browsing, and statistical analysis.

## Test Prerequisites
1. TensileSIM simulator running (or actual TensileOS hardware)
2. TensileCompanion application installed with all dependencies
3. Serial port available for connection

## Test Workflow

### 1. Initial Setup
- [ ] Launch TensileCompanion
- [ ] Verify two tabs present: "Live Test" and "Test Browser"
- [ ] Connect to TensileSIM or TensileOS device
- [ ] Verify connection status shows "Connected" in green

### 2. First Test with Metadata
- [ ] Click "New Test (Auto-Export)" button
- [ ] Verify metadata dialog appears with fields:
  - Test Name (required)
  - Technician (required, with dropdown if recent technicians exist)
  - Date/Time (auto-filled, read-only)
  - Notes (optional)
- [ ] Enter metadata:
  - Test Name: "Sample_Test_1"
  - Technician: "John Doe"
  - Notes: "First test with new system"
- [ ] Click OK
- [ ] Verify test starts immediately
- [ ] Watch data streaming on plot
- [ ] Note peak force value

### 3. Second Test with Metadata
- [ ] After test completes, click "New Test (Auto-Export)" again
- [ ] Verify previous test auto-saves
- [ ] Verify save confirmation dialog appears
- [ ] Verify metadata dialog shows last technician in dropdown
- [ ] Enter new metadata:
  - Test Name: "Sample_Test_2"
  - Technician: Select "John Doe" from dropdown
  - Notes: "Second test"
- [ ] Run test to completion

### 4. Third Test with Different Technician
- [ ] Click "New Test (Auto-Export)"
- [ ] Enter metadata:
  - Test Name: "Sample_Test_3"
  - Technician: "Jane Smith"
  - Notes: "Different technician"
- [ ] Run test
- [ ] Verify recent technicians list now includes both names

### 5. Test File Organization
- [ ] Navigate to Tests directory (default: ./Tests)
- [ ] Verify date folder exists (YYYY-MM-DD format)
- [ ] Verify three CSV files present with naming:
  - test_name_HHMMSS.csv
- [ ] Open one CSV file and verify:
  - Metadata headers start with #
  - Contains test_name, technician, datetime, notes, peak_force
  - Data columns: Timestamp (s), Current Force (kN), Peak Force (kN)

### 6. Test Browser Functionality
- [ ] Switch to "Test Browser" tab
- [ ] Click "Refresh" button
- [ ] Verify date folder(s) appear in tree view
- [ ] Expand date folder
- [ ] Verify all three tests listed
- [ ] Click on each test
- [ ] Verify metadata preview pane shows:
  - Test name
  - Technician
  - Date/time
  - Peak force
  - Notes

### 7. Multiple Test Selection
- [ ] In Test Browser, select checkbox for all three tests
- [ ] Verify "Calculate Statistics" button enabled
- [ ] Click "Calculate Statistics"
- [ ] Verify Statistics Results window opens

### 8. Statistics Window Verification
- [ ] In Statistics Results window, verify Summary Statistics section shows:
  - Number of Tests: 3
  - Average Peak Force (kN)
  - Standard Deviation
  - 3-Sigma Range (lower to upper bounds)
  - Min/Max values
  - Median
- [ ] Verify bar chart displays:
  - Individual test peak forces as blue bars
  - Mean line (green dashed)
  - +3σ and -3σ bounds (red dotted)
  - Test names on x-axis
- [ ] Verify Individual Test Results table shows:
  - Test Name
  - Technician
  - Peak Force (kN)
  - Deviation from Mean
- [ ] Click "Export Report"
- [ ] Save report CSV
- [ ] Open exported file and verify format

### 9. Edit Metadata
- [ ] In Test Browser, select one test
- [ ] Click "Edit Metadata" button
- [ ] Verify MetadataEditDialog opens showing:
  - Test Name (editable)
  - Technician (editable)
  - Date/Time (read-only, grayed out)
  - Peak Force (read-only, grayed out)
  - Notes (editable)
- [ ] Modify technician name to "John D."
- [ ] Add text to notes
- [ ] Click OK
- [ ] Verify success message
- [ ] Click Refresh in Test Browser
- [ ] Verify changes reflected in metadata preview

### 10. Cancel Operations
- [ ] Click "New Test (Auto-Export)"
- [ ] In metadata dialog, click Cancel
- [ ] Verify test does NOT start
- [ ] Verify plot remains unchanged

### 11. Settings Persistence
- [ ] Close TensileCompanion
- [ ] Reopen TensileCompanion
- [ ] Click "New Test (Auto-Export)"
- [ ] Verify metadata dialog shows last technician in dropdown
- [ ] Verify recent technicians list preserved (up to 10)

### 12. Error Handling
- [ ] Try to calculate statistics with only 1 test selected
- [ ] Verify warning message: "Please select at least 2 tests"
- [ ] Try to edit metadata on a test file that doesn't exist
- [ ] Verify appropriate error message

## Success Criteria
✅ All checkboxes above completed without errors
✅ Metadata captures correctly at test start
✅ Files organized in date folders
✅ CSV format includes metadata headers
✅ Test browser displays all tests with metadata
✅ Statistics calculated correctly for multiple tests
✅ Statistics window shows chart and summary
✅ Metadata editing works and persists
✅ Recent technicians dropdown functions
✅ Export functionality works for statistics

## Known Issues
Document any issues found during testing here:
- 

## Notes
- Default tests directory: ./Tests
- Date folders format: YYYY-MM-DD
- File naming: test_name_HHMMSS.csv
- Recent technicians limited to last 10
- Minimum 2 tests required for statistics
