# Terminal performance measurements

## Method

Windows 11 Enterprise 10.0.26200; Intel Core i9-13900H (20 logical
processors); Intel Iris Xe Graphics (driver 32.0.101.7084) and NVIDIA RTX
2000 Ada Generation Laptop GPU (driver 32.0.15.9641). The test ran locally
with the windows visible, one terminal at a time. Windows Terminal Preview
was 1.26.3133.0; WezTerm was `20261026-082628-e68a8ec2`. termica used the
Release llvm-mingw build and libghostty-vt 1.0.1. The v0.2 baseline was
`1584302a1356`. Each figure is a **single run**, not a statistical summary.

`tools/perf-workload.c` measures the time spent by a child process writing
exactly 100 MiB (104,857,600 bytes) to its terminal's ConPTY. The `cat`
workload reads a warm 100 MiB file containing repeated ASCII text and CRLF;
the `yes` workload generates `y\n` repeatedly. The child waits two seconds
for startup, measures with `QueryPerformanceCounter`, writes the byte count
and elapsed seconds to a result file, then waits four seconds before exiting.
Throughput is 100 divided by elapsed seconds (MiB/s).

These are **writer completion times**, not time until the terminal displays
the last byte. ConPTY and terminal buffers differ between implementations;
the results include backpressure but cannot establish full-display latency.
The terminal windows were not configured to identical fonts, geometry,
scrollback limits or compositing, so compare them as observations of these
settings, not controlled rankings. Background system activity and other user
sessions were not controlled.

## Throughput results

| Terminal | `cat` elapsed | `cat` MiB/s | `yes` elapsed | `yes` MiB/s |
|---|---:|---:|---:|---:|
| termica v0.2 | 133.18 s | 0.75 | 199.02 s | 0.50 |
| termica v0.3 development build | 7.83 s | 12.78 | 146.14 s | 0.68 |
| Windows Terminal Preview | 0.48 s | 207.15 | 8.99 s | 11.12 |
| WezTerm | 3.01 s | 33.27 | 13.42 s | 7.45 |

The v0.3 file-write run reflects the bounded-PTY-drain wake-up fix. The two
termica `yes` runs were repeated at the same 916 × 519 window size, 11-point
font and 10,000-line scrollback; they remain single samples. The `cat` and
other terminals' measurements were not size-matched.

## Reproduction

Build the workload from the repository root with llvm-mingw:

```powershell
x86_64-w64-mingw32-clang -O2 tools/perf-workload.c -o perf-workload.exe
```

Create the same 100 MiB ASCII payload, then run each command in each
terminal, one at a time, with distinct result files and the same input file:

```powershell
python -c "from pathlib import Path; s=b'Terminal performance measurement - 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ test\r\n'; b=(s*(65536//len(s)+1))[:65536]; Path('100MiB.txt').write_bytes(b*1600)"
```

```powershell
.\perf-workload.exe cat C:\path\to\100MiB.txt C:\path\to\cat-result.txt
.\perf-workload.exe yes C:\path\to\100MiB.txt C:\path\to\yes-result.txt
```

The `yes` mode ignores the file path but retains the same argument layout.
Read the result only after the child has finished: its two fields are bytes
and elapsed seconds. Run repeated trials with consistent terminal settings and
an idle system for defensible comparisons.

## Scrolling frame time

PresentMon 2.6.0 captured each target process separately with administrator
approval. Three newly created windows were resized to 916 × 519 pixels on a
1920 × 1080, 60 Hz display. Each received the same 2,400 numbered text lines;
120 wheel messages were posted to its own window, at approximately 30 ms
intervals, alternating 20 up and 20 down. Screenshots confirmed that all three
viewports scrolled. Termica and WezTerm were configured at 11 points; Windows
Terminal used its existing profile. Font faces, window chrome and renderer
settings were not identical.

For each single run, the table uses the dominant swapchain's PresentMon samples
whose `CPUStartQPCTimeInMs` fell inside the input interval, excluding the
first sample. Percentiles use the nearest-rank method. Frame interval is
`MsBetweenPresents`; GPU time is `MsGPUTime`, both in milliseconds.

| Terminal | Frames | Interval p50 | Interval p95 | Interval max | GPU p50 | GPU p95 |
|---|---:|---:|---:|---:|---:|---:|
| termica v0.3 | 123 | 31.14 | 32.60 | 350.06 | 1.83 | 3.90 |
| Windows Terminal Preview | 117 | 31.26 | 32.69 | 46.12 | 0.77 | 0.99 |
| WezTerm | 102 | 31.18 | 32.60 | 45.74 | 1.71 | 4.13 |

The interval is largely determined by the 30 ms input cadence, not intrinsic
rendering time; messages may coalesce and unrelated redraws can be counted.
Termica had one 350 ms interval in this run. GPU and presentation modes also
differed: Windows Terminal used DXGI flip; termica used DXGI/GDI composition;
WezTerm reported an `Other` present runtime with GDI composition. These are
single samples, not controlled rankings. Repeat with matched fonts and
settings before drawing broader conclusions.

To reproduce, use PresentMon with ETW permission, capture only the new
window's process (`--process_id <PID> --qpc_time_ms --timed 8
--terminate_after_timed --output_file <CSV>`), and filter the recorded
samples to the wheel-input interval using the same performance-counter clock.
Do not grant permanent administrator privileges merely to run the test.
