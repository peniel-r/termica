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

Not measured. PresentMon 2.6.0 was available but ETW session creation failed
with `access denied`: the current account lacks administrator or Performance
Log Users privileges. Do not compare screenshots or timer periods as frame
presentation times. To complete this measurement, capture each terminal's
scrolling presents with an appropriately authorized PresentMon account,
record the scenario and p50/p95 frame times, and compare the same geometry,
font, scrollback contents, refresh rate and renderer.
