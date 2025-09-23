#!/usr/bin/env python3
"""
Log parser and grapher for Tenstorrent Blackhole telemetry data
"""

import re
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from datetime import datetime, timedelta
import numpy as np

def parse_log_file(filename):
    """
    Parse the log file to extract telemetry data
    Returns: dict with lists of time values and power values
    """
    data = {
        'time_values': [],
        'tdp_power': [],
        'board_power': []
    }
    
    # Regular expressions to match the different log entries
    time_pattern = r'Time: ([\d.]+) W'
    tdp_power_pattern = r'TDP Power: ([\d.]+) W'
    board_power_pattern = r'Board Power: ([\d.]+) W'
    
    with open(filename, 'r') as file:
        lines = file.readlines()
    
    current_time = None
    current_data = {}
    
    for line in lines:
        # Extract time value
        if 'Time:' in line:
            time_match = re.search(time_pattern, line)
            if time_match:
                current_time = float(time_match.group(1))
        
        # Extract power values
        elif 'TDP Power:' in line:
            match = re.search(tdp_power_pattern, line)
            if match and current_time is not None:
                current_data['tdp_power'] = float(match.group(1))
        
        elif 'Board Power:' in line:
            match = re.search(board_power_pattern, line)
            if match and current_time is not None:
                current_data['board_power'] = float(match.group(1))
                
                # We have all three values, add to data arrays
                if len(current_data) == 2:  # tdp_power and board_power
                    data['time_values'].append(current_time)
                    data['tdp_power'].append(current_data['tdp_power'])
                    data['board_power'].append(current_data['board_power'])
                    current_data = {}
                    current_time = None  # Reset for next set
    
    return data

def create_power_graph(data):
    """
    Create a comprehensive power monitoring graph
    """
    # Convert time values to relative time from start
    if not data['time_values']:
        print("No data found in log file!")
        return
    
    # Convert time values to relative time from start (values are already in seconds)
    start_time = data['time_values'][0]
    time_relative = [t - start_time for t in data['time_values']]  # Already in seconds
    
    # Create the plot with both power values on same graph
    plt.figure(figsize=(12, 8))
    
    # Plot both power values
    plt.plot(time_relative, data['tdp_power'], 'b-', linewidth=1, label='TDP Power', alpha=0.8, marker='o', markersize=0.1)
    plt.plot(time_relative, data['board_power'], 'g-', linewidth=1, label='Board Power', alpha=0.8, marker='o', markersize=0.1)
    
    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('Power (W)', fontsize=12)
    plt.title('Tenstorrent Blackhole Power Telemetry Over Time', fontsize=14, fontweight='bold')
    plt.legend(loc='upper right')
    plt.grid(True, alpha=0.3)
    
    # Add some statistics
    if data['tdp_power']:
        avg_tdp = np.mean(data['tdp_power'])
        max_tdp = np.max(data['tdp_power'])
        min_tdp = np.min(data['tdp_power'])
        avg_board = np.mean(data['board_power'])
        max_board = np.max(data['board_power'])
        min_board = np.min(data['board_power'])
        
        stats_text = f'TDP: avg={avg_tdp:.1f}W, min={min_tdp:.1f}W, max={max_tdp:.1f}W | Board: avg={avg_board:.1f}W, min={min_board:.1f}W, max={max_board:.1f}W'
        plt.figtext(0.5, 0.02, stats_text, ha='center', fontsize=10, style='italic')
    
    plt.tight_layout()
    plt.subplots_adjust(bottom=0.1)
    
    # Save the plot
    plt.savefig('power_telemetry_graph.png', dpi=300, bbox_inches='tight')
    print("Graph saved as 'power_telemetry_graph.png'")
    
    # Show the plot
    plt.show()


def main():
    """
    Main function to parse log and create graphs
    """
    print("Parsing log.txt file...")
    
    try:
        data = parse_log_file('log.txt')
        
        if not data['time_values']:
            print("No telemetry data found in log.txt")
            return
        
        print(f"Found {len(data['time_values'])} telemetry data points")
        print(f"Time range: {data['time_values'][0]} to {data['time_values'][-1]}")
        
        # Create power telemetry graph
        print("\nCreating power telemetry graph...")
        create_power_graph(data)
        
        # Print some basic statistics
        if data['tdp_power']:
            print(f"\n=== Power Statistics ===")
            print(f"TDP Power: avg={np.mean(data['tdp_power']):.2f}W, min={np.min(data['tdp_power']):.2f}W, max={np.max(data['tdp_power']):.2f}W")
            print(f"Board Power: avg={np.mean(data['board_power']):.2f}W, min={np.min(data['board_power']):.2f}W, max={np.max(data['board_power']):.2f}W")
        
    except FileNotFoundError:
        print("Error: log.txt file not found!")
    except Exception as e:
        print(f"Error parsing log file: {e}")

if __name__ == "__main__":
    main()
