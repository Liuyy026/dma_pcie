#!/usr/bin/env python3
import sys
import json
from pathlib import Path

if len(sys.argv) < 4 or len(sys.argv) > 5:
    print("Usage: update_config.py <config_path> <file_block_size_kb> <io_request_num> [folder_path]")
    sys.exit(2)

config_path = Path(sys.argv[1])
block_kb = int(sys.argv[2])
io_num = int(sys.argv[3])
folder_path = None
if len(sys.argv) == 5:
    folder_path = sys.argv[4]

if not config_path.exists():
    print(f"Config file not found: {config_path}")
    sys.exit(1)

with config_path.open('r', encoding='utf-8') as f:
    data = json.load(f)

# Update fields
# The GUI expects FileBlockSize in KB (as observed in MainWindow)
data['FileBlockSize'] = block_kb
data['IoRequestNum'] = io_num
if folder_path:
    # store as string; MainWindow expects FolderPath to be a string
    data['FolderPath'] = folder_path

with config_path.open('w', encoding='utf-8') as f:
    json.dump(data, f, indent=4)

print(f"Updated {config_path}: FileBlockSize={block_kb} KB, IoRequestNum={io_num}")
