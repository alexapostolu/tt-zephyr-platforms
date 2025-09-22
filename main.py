import matplotlib.pyplot as plt
import numpy as np
import re

def read_throttler_data(file_path):
    data = []
    with open(file_path, 'r') as file:
        for line in file:
            match = re.search(r'named_event: \{ name = "thr", arg0 = (\d+), arg1 = (\d+) \}', line)
            if match:
                arg0 = int(match.group(1))
                arg1 = int(match.group(2))
                data.append((arg0, arg1))
    return data

file_path = 'update_throttler.log'

data = read_throttler_data(file_path)
print("data points read:", len(data))

if data:
    # Separate arg0 and arg1 values
    arg0_values = [point[0] for point in data]
    arg1_values = [point[1] for point in data]
    
    # Create x-axis (sample index)
    x = np.arange(len(data))
    
    # Create a single plot
    plt.figure(figsize=(12, 8))
    
    # Plot both arg0 and arg1 values as points only
    plt.plot(x, arg0_values, label='tdp', marker='o', markersize=1)#, linestyle='None')
    plt.plot(x, arg1_values, label='power', marker='o', markersize=1)#, linestyle='None')
    
    plt.xlabel('Sample Index')
    plt.ylabel('Value')
    plt.title('Throttler "thr" Event Data')
    plt.legend()
    plt.grid(True)
    plt.savefig('throttler_plot.png', dpi=300, bbox_inches='tight')
    plt.show()
else:
    print("No 'thr' named events found in the data")
