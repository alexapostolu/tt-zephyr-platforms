import matplotlib.pyplot as plt
import numpy as np
import re

def read_throttler_data(file_path):
    data = []
    with open(file_path, 'r') as file:
        for line in file:
            # Look for named_event lines with names "1" or "2"
            match = re.search(r'named_event: \{ name = "([12])", arg0 = (\d+), arg1 = (\d+) \}', line)
            if match:
                name = match.group(1)
                arg0 = int(match.group(2))
                arg1 = int(match.group(3))
                data.append((name, arg0, arg1))
    return data

# File path (replace with your actual file path)
file_path = 'update_throttler.log'

data = read_throttler_data(file_path)
print("data points read:", len(data))

# Map named event names to descriptive labels
event_map = {
    "1": "Limit vs Value",
    "2": "Alpha Filter vs Raw Value"
}

# Organize data by event name, tracking both arg0 and arg1
event_names = ["1", "2"]
data_by_event = {name: {"arg0": [], "arg1": []} for name in event_names}

for name, arg0, arg1 in data:
    if name in event_names:
        data_by_event[name]["arg0"].append(arg0)
        data_by_event[name]["arg1"].append(arg1)

# Create a single plot for both events
plt.figure(figsize=(12, 8))

max_length = max(len(data_by_event[name]["arg0"]) for name in event_names if data_by_event[name]["arg0"])
if max_length > 0:
    x = np.arange(max_length)

    # Plot both arg0 and arg1 values for each event
    for name in event_names:
        if data_by_event[name]["arg0"]:
            # Pad shorter sequences with NaN for consistent plotting
            padded_data_arg0 = data_by_event[name]["arg0"] + [np.nan] * (max_length - len(data_by_event[name]["arg0"]))
            # Specific labels based on what each event measures
            if name == "1":
                arg0_label = "limit"
            else:  # name == "2"
                arg0_label = "limit - value"
            plt.plot(x, padded_data_arg0, label=arg0_label, marker='o', markersize=1, linestyle='None')

        if data_by_event[name]["arg1"]:
            # Pad shorter sequences with NaN for consistent plotting
            padded_data_arg1 = data_by_event[name]["arg1"] + [np.nan] * (max_length - len(data_by_event[name]["arg1"]))
            # Specific labels based on what each event measures
            if name == "1":
                arg1_label = "value"
            else:  # name == "2"
                arg1_label = "error (x100)"
            plt.plot(x, padded_data_arg1, label=arg1_label, marker='o', markersize=1, linestyle='None')

plt.xlabel('Sample Index')
plt.ylabel('Value')
plt.title('TDC Throttler Control System Data')
plt.legend()
plt.grid(True)
plt.savefig('throttler_plot.png', dpi=300, bbox_inches='tight')
plt.show()
