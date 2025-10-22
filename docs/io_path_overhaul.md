# IO Path Overhaul Plan

## Background
- File input currently handled by `CInputDataFromFileUsePOSIX` with 1 producer + `io_request_num` worker threads executing synchronous `pread`.
- Recent metrics show per-loop latency 1.5s+, excessive blocking in buffer acquisition, and heavy CPU scheduling overhead.
- Buffer pool expanded to 2048 slots (16 × io_request_num). Despite this, workers often wait >200 ms for buffers and soft-limit gating stalls the producer.
- Goal: flatten latency, reduce jitter, and sustain throughput without 100+ busy threads. Permission granted to rebuild the path if required.

## Observed Bottlenecks
1. **Excessive Threading** – 128 worker threads all issuing 8 MiB `pread` calls thrash the kernel’s scheduler and disk queue.
2. **Sequential Workload** – Files are read sequentially, so large parallelism brings little benefit but heavy contention.
3. **Synchronous wait chains** – producer waits on `capacity_cv`, workers block on buffer pool, DMA waits on send queue when descriptor bursts occur.
4. **Diagnostic feedback** – slow-loop logs show `模式=external`, so zero-copy is engaged, yet median latency grew (~1.76 s → 1.84 s).

## Design Goals
- Keep sequential reads in-order while providing just enough lookahead to mask DMA latency.
- Reduce worker count to the minimal effective set (ideally 1–4 threads).
- Preserve zero-copy handoff through `AlignedBufferPool` and descriptor queue.
- Maintain compatibility with existing UI/config; allow fallback to the legacy pipeline.
- Future-proof for io_uring integration.

## Target Architecture
```
+---------------------+
| SequentialReader    |  (new) single / small N reader threads
|  • manages file list|  
|  • acquires buffers |
|  • issues reads     |  ---> InputBlock --> OrderedDataProcessor --> DMA
+---------------------+
         ^
         |
  WaitForPendingCapacity (soft cap on outstanding blocks)

Optional extension:
  SequentialReader backend = { blocking pread | io_uring future }
```

### Components
- **`FileReadEngine` interface**: abstracts how blocks are fetched (legacy queue vs streaming reader vs io_uring later).
- **`StreamingFileReader` implementation**:
  - Single thread loops files sequentially.
  - Uses `AlignedBufferPool::Acquire` for buffers (blocking with stop flag).
  - Respects soft-limit before acquiring the next block.
  - Populates `InputBlock` and pushes to `OrderedDataProcessor` directly.
  - Updates `pending_tasks_`, `global_request_counter_`, `next_file_id_` just like legacy path.
- **Legacy worker mode** retained behind a flag for regression testing.
- **Configuration knob**: struct field `use_streaming_reader` (JSON `DiskUseStreamingReader`, default `false`). Env override `PCIE_FILE_STREAMING=1` for quick toggles.

## Migration Plan
1. **Phase 1 (this change set)**
   - Introduce the config flag and new reader class skeleton.
   - Integrate streaming reader into `CInputDataFromFileUsePOSIX` (start/stop logic).
   - Default to legacy unless `use_streaming_reader` toggle is on or `io_request_num` exceeds CPU cores by 2×.
   - Maintain existing diagnostics while adding streaming-specific counters (buffer wait, read latency).
2. **Phase 2**
   - Tune soft-limit heuristics for streaming mode (auto target = `max(4, cpu_count/2)`).
   - Add adaptive rate control when DMA back-pressure observed.
   - Expand metrics: per-block read latency histogram, queue depth snapshot.
3. **Phase 3**
   - Provide io_uring backend under feature gate (`DiskUseIoUring`).
   - Fallback gracefully if kernel <5.10 or submission fails.
4. **Phase 4**
   - Remove legacy worker mode once new path validated; simplify buffer pool interactions.

## Validation Strategy
- Build + run `perf_test.sh` with large sequential file set
- Compare latency statistics pre/post (expect >=30% reduction in median loop time)
- Monitor `/tmp/pcie_summary.json` for descriptor vs copy ratio (should remain descriptor-heavy)
- Stress test with multiple concurrent runs to ensure no starvation / memory spikes.

## Risks & Mitigations
- **Buffer starvation**: streaming thread might stall if DMA blocks release slowly. Mitigation: retain soft-limit gating and enlarge pool slack when necessary.
- **Backwards compatibility**: keep legacy path, guard new fields with defaults, ensure JSON parser tolerates missing key.
- **Platform limitations**: streaming path assumes Linux; keep IOCP path untouched.

## Next Steps
- Implement `StreamingFileReader` inside `InputDataFromFileUsePOSIX` (Phase 1).
- Wire config (JSON, MainWindow settings) + environment overrides.
- Add runtime log message confirming mode selection.
- Rebuild & benchmark.

## Status (2025-10-22)
- Phase 1 streaming reader path implemented behind `DiskUseStreamingReader` toggle with env override `PCIE_FILE_STREAMING`.
- Qt application (`PCIE_Demo_QT`) rebuilt successfully; pending performance run to collect new latency stats.
