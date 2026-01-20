"""
Statistical analysis utilities for test data
Calculates statistics across multiple tests
"""

import statistics
from typing import List, Dict, Tuple
from pathlib import Path


class TestStatistics:
    """Calculate statistics for multiple tests"""
    
    def __init__(self, test_metadata_list: List[Dict[str, str]]):
        """Initialize with list of test metadata
        
        Args:
            test_metadata_list: List of metadata dictionaries from tests
        """
        self.tests = test_metadata_list
        self.peak_forces = []
        
        # Extract peak forces
        for test in self.tests:
            peak_str = test.get('peak_force', '').replace(' kN', '').strip()
            try:
                peak = float(peak_str)
                self.peak_forces.append(peak)
            except (ValueError, AttributeError):
                # Skip tests with invalid peak force
                pass
    
    def calculate_average_peak(self) -> float:
        """Calculate average peak force
        
        Returns:
            Average peak force in kN
        """
        if not self.peak_forces:
            return 0.0
        return statistics.mean(self.peak_forces)
    
    def calculate_std_dev(self) -> float:
        """Calculate standard deviation of peak forces
        
        Returns:
            Standard deviation in kN
        """
        if len(self.peak_forces) < 2:
            return 0.0
        return statistics.stdev(self.peak_forces)
    
    def calculate_3sigma(self) -> Tuple[float, float, float]:
        """Calculate 3-sigma range (mean ± 3*std_dev)
        
        Returns:
            Tuple of (mean, lower_bound, upper_bound)
        """
        mean = self.calculate_average_peak()
        std_dev = self.calculate_std_dev()
        
        lower = mean - (3 * std_dev)
        upper = mean + (3 * std_dev)
        
        return (mean, lower, upper)
    
    def get_min_peak(self) -> float:
        """Get minimum peak force
        
        Returns:
            Minimum peak force in kN
        """
        return min(self.peak_forces) if self.peak_forces else 0.0
    
    def get_max_peak(self) -> float:
        """Get maximum peak force
        
        Returns:
            Maximum peak force in kN
        """
        return max(self.peak_forces) if self.peak_forces else 0.0
    
    def get_median_peak(self) -> float:
        """Get median peak force
        
        Returns:
            Median peak force in kN
        """
        if not self.peak_forces:
            return 0.0
        return statistics.median(self.peak_forces)
    
    def get_test_count(self) -> int:
        """Get number of tests
        
        Returns:
            Count of tests
        """
        return len(self.peak_forces)
    
    def get_summary(self) -> Dict[str, float]:
        """Get complete statistical summary
        
        Returns:
            Dictionary with all statistics
        """
        mean, lower_3sigma, upper_3sigma = self.calculate_3sigma()
        
        return {
            'count': self.get_test_count(),
            'mean': mean,
            'std_dev': self.calculate_std_dev(),
            'lower_3sigma': lower_3sigma,
            'upper_3sigma': upper_3sigma,
            'min': self.get_min_peak(),
            'max': self.get_max_peak(),
            'median': self.get_median_peak()
        }
    
    def get_deviations(self) -> List[Tuple[str, float, float]]:
        """Get deviation from mean for each test
        
        Returns:
            List of tuples (test_name, peak_force, deviation_from_mean)
        """
        mean = self.calculate_average_peak()
        deviations = []
        
        for i, test in enumerate(self.tests):
            if i < len(self.peak_forces):
                test_name = test.get('test_name', 'Unknown')
                peak = self.peak_forces[i]
                deviation = peak - mean
                deviations.append((test_name, peak, deviation))
        
        return deviations
