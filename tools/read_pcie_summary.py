#!/usr/bin/env python3
import json
import sys

path = '/tmp/pcie_summary.json'
try:
    with open(path,'r') as f:
        j = json.load(f)
except Exception as e:
    print('Could not read', path, '=>', e)
    sys.exit(1)

print('PCIE summary from', path)
print('descWriteCount:', j.get('descWriteCount'))
print('descWriteNsTotal:', j.get('descWriteNsTotal'))
print('descAvgMs:', j.get('descAvgMs'))
print('copyWriteCount:', j.get('copyWriteCount'))
print('copyWriteNsTotal:', j.get('copyWriteNsTotal'))
print('copyAvgMs:', j.get('copyAvgMs'))
print('totalBytesSent:', j.get('totalBytesSent'))

# if totalBytesSent and descWriteCount available, print bytes per desc
if j.get('descWriteCount') and j.get('totalBytesSent'):
    try:
        avg_bytes_per_desc = j['totalBytesSent'] / float(j['descWriteCount'])
        print('avg bytes per desc:', avg_bytes_per_desc)
    except Exception:
        pass
