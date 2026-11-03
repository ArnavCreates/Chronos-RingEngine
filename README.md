# Chronos RingEngine

Chronos RingEngine is a high-performance C++20 limit-order-book (LOB) matching engine and lock-free multi-producer/single-consumer (MPSC) event-processing pipeline. It models exchange workloads such as price-time priority limit-order matching, O(1) order cancellation, and deterministic FIFO execution with zero heap allocations on the critical path.

The architecture decouples order ingestion from matching via a bounded, sequence-numbered MPSC ring buffer, processing incoming `EventCommand` requests with mechanical-sympathy memory layouts, intrusive slab pooling, and deterministic resting-book state hashing.

## Architecture

- `MpscRingBuffer<T>` is a bounded, multi-producer/single-consumer queue with per-slot sequence numbers and CAS ticket reservation. `try_push()` reports a full queue without waiting; `push()` spins until a slot is available.
- Four interchangeable books show a progression from map/list storage to slab-backed intrusive queues, a bounded flat price array with an occupancy bitmap, and the V4 cached-best-tick book with a flat cancellation index.
- V4 maps `OrderId` directly to an 8-byte location record. Its bounded tick index and side are packed into one 32-bit field alongside the 32-bit slab-node index.
- `SlabNode` stores the complete 32-byte `Order` payload and is `alignas(32)`. Link and state metadata lives in a compact 12-byte sidecar entry, preserving full-width IDs, prices, and quantities while keeping two payloads within a 64-byte cache line.
- The engine reserves its trade tape to the configured maximum command/resting-order capacity. It reports validation and capacity errors through the generic API and does not allocate a new trade vector per operation.
- Snapshot hashes use FNV-1a 64 over canonical little-endian fields. Snapshots omit cancelled ghosts and include each live level's price, quantity, and active-order count.

## Cancellation and reclamation

V4 cancellation uses the flat ID index and performs the logical `Active -> GhostCancelled` transition in O(1). It clears the ID index immediately. If the cancelled order is the FIFO head, V4 unlinks and reclaims it immediately in O(1); an interior ghost remains in the chain until it reaches the matching head. When a level loses its last live order, the whole ghost-only chain is reclaimed immediately because the empty level is no longer guaranteed to be visited by a matching cursor. The slot lifecycle is `Free -> Active -> GhostCancelled -> Free` for cancelled orders.

## Provenance and clean-room engineering

The architecture adapts mechanical-sympathy patterns from the LMAX Disruptor (cache-line-aligned ring buffers with acquire-release semantics) and standard electronic exchange limit-order-book design (intrusive slab memory pools, O(1) lazy ghost cancellation, and flat tick-indexed price levels). Chronos RingEngine is a clean-room implementation; the following constraints and correctness gaps are addressed here:

- Replaced a non-wrap-safe boolean slot flag with a Vyukov-style, sequence-numbered `alignas(64)` MPSC ring. The sequence value distinguishes slot generations across wraparound.
- Added the O(1) lazy ghost-cancellation state machine, immediate head-of-line reclamation, and immediate cleanup of ghost-only levels.
- Removed per-operation trade-vector allocation from the engine hot path through a pre-reserved trade tape and a preallocated `SlabPool`.
- Extended differential verification to compare execution ledgers and 64-bit FNV-1a resting-book snapshot hashes across all four variants. Deterministic replay checks repeatability for identical input streams.

## Build and verification

The project requires CMake 3.20 or newer, a C++20 compiler, Ninja, and GoogleTest. CMake uses `find_package(GTest REQUIRED)` and does not download dependencies. GCC/Clang Release builds enable `-O3 -march=native -DNDEBUG` and retain `-Wall -Wextra -Werror -Wpedantic`.

### Linux (Ubuntu and Debian)

Install the required packages with:

```sh
sudo apt-get update
sudo apt-get install --yes cmake ninja-build g++ libgtest-dev
```

From the repository directory, configure, build, and run the Release tests:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Build and run the AddressSanitizer and UndefinedBehaviorSanitizer test target with:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCHRONOS_ENABLE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Run the executables directly when collecting benchmark results:

```sh
./build/chronos_mpsc_bench
./build/chronos_variant_bench
```

### Windows (MSYS2 UCRT64)

Install GCC, CMake, Ninja, and GoogleTest in the MSYS2 UCRT64 environment. Ensure `C:\msys64\ucrt64\bin` is on `PATH`, then run these commands from PowerShell in the repository directory:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
.\build\chronos_tests.exe
.\build\chronos_mpsc_bench.exe
.\build\chronos_variant_bench.exe
```

The current test run passed all 17 GoogleTest cases. Coverage includes repeated queue wraparound with ten producers, malformed CSV rejection, deterministic replay, matching and cancellation behavior, zero hot-path allocations for V4, and ledger/snapshot parity across all variants.

## MPSC throughput

The run used 10 producers, 1,000,000 pre-generated commands per producer, and a 1,048,576-slot queue. Command generation is outside the timed interval.

| Producers | Commands | Queue capacity | Elapsed seconds | Throughput ops/s | Average ns/op | Matched trades | Ghosts reclaimed | Target ops/s | Target |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---|
| 10 | 10,000,000 | 1,048,576 | 1.684 | 5,938,185.860 | 168.402 | 6,205,308 | 5,068 | 3,100,000.000 | Pass |

Producer interleaving can change trade totals between runs; every run is expected to process the full command count. The run data is in [`assets/mpsc_throughput_results.json`](assets/mpsc_throughput_results.json).

## Variant benchmark results

Each workload used 100,000 warm-up commands followed by 1,000,000 measured commands. Batch `amortized_ns_per_op` uses `steady_clock`. Per-operation percentiles use TSC reads with the median overhead from 100,000 back-to-back probe pairs subtracted from each sample; reported integer nanoseconds are rounded up and never report a positive sample as zero. The benchmark thread was pinned to logical CPU 0 and set to high process/thread priority on Windows. The measured TSC frequency was 2,495,986,196.269 Hz; median probe overhead was 44 cycles (17.628 ns).

| Workload | Variant | Throughput ops/s | Amortized ns/op | Cycles/op | Mean / P50 / P90 / P99 / P99.9 ns | Trades | Hot-path `new` calls |
|:---|:---|---:|---:|---:|:---|---:|---:|
| Balanced_OrderFlow | V1_MapList | 6,679,058.547 | 149.722 | 373.701 | 156.868 / 142 / 274 / 497 / 795 | 581,454 | 1,231,948 |
| Balanced_OrderFlow | V2_MapSlab | 8,024,999.478 | 124.611 | 311.023 | 134.367 / 119 / 237 / 452 / 791 | 581,454 | 762,886 |
| Balanced_OrderFlow | V3_FlatBitmap | 10,067,107.338 | 99.333 | 247.931 | 111.433 / 89 / 200 / 381 / 636 | 581,454 | 469,065 |
| Balanced_OrderFlow | V4_ChronosFull | 31,765,998.945 | 31.480 | 78.568 | 30.498 / 27 / 59 / 125 / 222 | 581,454 | 0 |
| Cancel_Heavy_Burst | V1_MapList | 9,417,357.508 | 106.187 | 265.037 | 116.580 / 103 / 234 / 400 / 660 | 425,938 | 899,216 |
| Cancel_Heavy_Burst | V2_MapSlab | 10,947,782.362 | 91.343 | 227.986 | 95.897 / 87 / 193 / 339 / 548 | 425,938 | 555,066 |
| Cancel_Heavy_Burst | V3_FlatBitmap | 13,681,369.122 | 73.092 | 182.432 | 80.013 / 72 / 158 / 280 / 589 | 425,938 | 344,153 |
| Cancel_Heavy_Burst | V4_ChronosFull | 40,767,738.043 | 24.529 | 61.218 | 21.827 / 20 / 46 / 105 / 187 | 425,938 | 0 |
| Single_Tick_Contention | V1_MapList | 5,416,361.094 | 184.626 | 460.822 | 171.926 / 67 / 390 / 604 / 2,703 | 148,518 | 1,350,103 |
| Single_Tick_Contention | V2_MapSlab | 7,678,280.065 | 130.238 | 325.068 | 142.724 / 49 / 373 / 571 / 1,419 | 148,518 | 675,053 |
| Single_Tick_Contention | V3_FlatBitmap | 6,766,943.750 | 147.777 | 368.846 | 151.293 / 60 / 369 / 601 / 1,781 | 148,518 | 675,053 |
| Single_Tick_Contention | V4_ChronosFull | 50,995,430.809 | 19.610 | 48.938 | 16.241 / 8 / 29 / 119 / 206 | 148,518 | 0 |

The benchmark replays each measured workload outside the timed pass and hashes every trade's maker ID, taker ID, price, and quantity with FNV-1a 64 using canonical little-endian fields, followed by the ledger length. All four variants produced identical matched-trade totals, trade-ledger hashes, and final resting-book hashes:

| Workload | Matched trades | Trade ledger hash | Final snapshot hash |
|:---|---:|---:|---:|
| Balanced_OrderFlow | 581454 | 14993857306644745382 | 1142100614329776776 |
| Cancel_Heavy_Burst | 425938 | 7235044370710537332 | 4185448699708678808 |
| Single_Tick_Contention | 148518 | 16042332863231913596 | 5176015459438953539 |

V4 recorded zero hot-path heap allocations in all three workloads. Its batch latency and corrected P50 were below 60 ns in each workload. The run reported `parity_passed: true` and `v4_targets_passed: true`; the full measurements are in [`assets/variant_latency_comparison.json`](assets/variant_latency_comparison.json).

Linux `perf_event_open` counters are implemented for Linux runs. This Windows run did not have those counters available, so the JSON marks them unavailable and reports zero counter values.
