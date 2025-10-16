#!/usr/bin/env python3
import sys
import csv
import statistics
from collections import defaultdict

def analyze(path):
    stats = defaultdict(list)
    total = 0
    with open(path, 'r') as f:
        rdr = csv.DictReader(f)
        for r in rdr:
            total += 1
            t = r['write_type']
            try:
                latency = int(r['latency_ns'])
            except:
                latency = 0
            stats[t].append(latency)
    print(f"Total writes: {total}")
    for t, lst in stats.items():
        if not lst:
            continue
        print(f"Type: {t}")
        print(f"  Count: {len(lst)}")
        print(f"  Avg latency (us): {statistics.mean(lst)/1000:.3f}")
        print(f"  Median latency (us): {statistics.median(lst)/1000:.3f}")
        print(f"  90p latency (us): {sorted(lst)[int(0.9*len(lst))]/1000:.3f}")
        print(f"  Max latency (us): {max(lst)/1000:.3f}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: pcie_csv_analysis.py /path/to/pcie_write_log.csv')
        sys.exit(1)
    analyze(sys.argv[1])
