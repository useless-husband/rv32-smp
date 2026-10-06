# Benchmark results

Simulated machine cycles on this machine (Apple M5, shared), from the cores' own
`mcycle` counter.  Speed-up is cycles(1 hart) / cycles(n harts) for the same total
work; the checksum is identical across hart counts, so the work is really the same.


## Mandelbrot (80x48, 50 iterations), rows split across harts

| harts | cycles | speed-up | checksum | BusRd | BusRdX | BusUpgr | supplied by a cache |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1,692,930 | 1.00x | 70288 | 9 | 37 | 0 | 0 |
| 2 | 852,072 | 1.99x | 70288 | 26 | 2233 | 10 | 2209 |
| 4 | 429,560 | 3.94x | 70288 | 52 | 3093 | 21 | 3077 |

## Shared histogram (4096 samples, 16 atomically-updated buckets)

A memory-sharing-heavy kernel: every bucket ping-pongs between caches.

| harts | cycles | speed-up | bucket total |
|---:|---:|---:|---:|
| 1 | 32,988 | 1.00x | 4096 |
| 2 | 22,667 | 1.46x | 4096 |
| 4 | 16,346 | 2.02x | 4096 |

## False sharing and its fix (4 harts, 20000 increments each)

```
false sharing, 4 harts, 20000 increments each:
  packed (one line)   : 361051 cycles
  padded (per line)   : 120484 cycles
  speed-up from the fix: 2.99x
```


