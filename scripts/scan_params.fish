#!/usr/bin/env fish
# scan_params.fish - run parameter scan for FileBlockSize (KB) and IoRequestNum
# Writes results to /tmp/pcie_scan_results.csv

set BUILD_BIN $HOME/pcie_build/bin/PCIE_Demo_QT
set CONFIG $HOME/pcie_build/bin/config/config.json
set UPDATE_SCRIPT /home/gkwx/Desktop/txt/PCIE_QT_20251010(1)/PCIE_QT_20251010/tools/update_config.py
#!/usr/bin/env fish
# scan_params.fish - run parameter scan for FileBlockSize (KB) and IoRequestNum
# Writes results to /tmp/pcie_scan_results.csv

set BUILD_BIN $HOME/pcie_build/bin/PCIE_Demo_QT
set CONFIG $HOME/pcie_build/bin/config/config.json
set UPDATE_SCRIPT /home/gkwx/Desktop/txt/PCIE_QT_20251010(1)/PCIE_QT_20251010/tools/update_config.py
if test ! -x $BUILD_BIN
    echo "Executable not found: $BUILD_BIN"
    exit 1
end

# parameter lists (KB, count)
set BLOCKS 1024 2048 4096
set IOREQ 8 16 32

# results CSV
set OUT /tmp/pcie_scan_results.csv
echo "block_kb,io_req,avg_speed_mb_s,median_speed_mb_s" > $OUT

for b in $BLOCKS
    for i in $IOREQ
        echo "Running test: block=$b KB io=$i..."
        # update config (use project update_config.py)
        if test -f $UPDATE_SCRIPT
            if set -q TEST_FOLDER
                python3 $UPDATE_SCRIPT $CONFIG $b $i $TEST_FOLDER
            else
                python3 $UPDATE_SCRIPT $CONFIG $b $i
            end
        else
            echo "update_config.py not found at $UPDATE_SCRIPT"
            exit 1
        end
        # run GUI for 30s
        rm -f /tmp/pcie_gui_run.log
        timeout 30s env DISPLAY="$DISPLAY" XAUTHORITY="$XAUTHORITY" $BUILD_BIN > /tmp/pcie_gui_run.log 2>&1; or true
        # extract per-second deltaBytes values
        set vals (grep -nI 'deltaBytes=' /tmp/pcie_gui_run.log | sed -E 's/.*deltaBytes=([0-9]+).*/\1/' )
        if test (count $vals) -eq 0
            echo "no data for block=$b io=$i" >> $OUT
            continue
        end
        # compute MB/s values
        set sums 0
        for v in $vals
            set sums (math $sums + $v)
        end
        set count (count $vals)
        set avg (math $sums / $count / 1024 / 1024)
        # median (fish-friendly)
        set sorted (printf "%s\n" $vals | sort -n)
        set cnt (count $sorted)
        if test (math $cnt % 2) -eq 1
            set idx (math \( $cnt + 1 \) / 2)
            set median_val $sorted[$idx]
        else
            set i1 (math $cnt / 2)
            set i2 (math $i1 + 1)
            set a $sorted[$i1]
            set b2 $sorted[$i2]
            set median_val (math \( $a + $b2 \) / 2)
        end
        set median (math $median_val / 1024 / 1024)
        echo "$b,$i,$avg,$median" >> $OUT
        echo "Result: block=$b io=$i avg=$avg MB/s median=$median MB/s"
        sleep 1
    end
end

echo "Scan complete, results in $OUT"
