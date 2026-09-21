## Apple M5 Pro (18 threads, Metal) — aspen 600x450, 10k points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 5.87 | --backend cpu |
| single thread | 41.99 | --backend cpu --threads 1 |
| float canny | 5.91 | --backend cpu --canny float |
| jump flooding voronoi | 7.83 | --backend cpu --voronoi jfa |
| brute force voronoi | 230.34 | --backend cpu --voronoi brute |
| sort+unique extraction | 6.80 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 5.88 | --backend cpu --raster owner |
| with hull overlay | 6.34 | --backend cpu --hull |
| everything off, single thread | 62.70 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 8.70 | --backend gpu |
| gpu, single host thread | 16.81 | --backend gpu --threads 1 |
| gpu with hull overlay | 9.17 | --backend gpu --hull |

## Apple M5 Pro — alto 2000x1125, 30k points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 21.26 | --backend cpu |
| single thread | 212.18 | --backend cpu --threads 1 |
| float canny | 24.85 | --backend cpu --canny float |
| jump flooding voronoi | 35.74 | --backend cpu --voronoi jfa |
| brute force voronoi | 5956.74 | --backend cpu --voronoi brute |
| sort+unique extraction | 23.97 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 20.65 | --backend cpu --raster owner |
| with hull overlay | 23.36 | --backend cpu --hull |
| everything off, single thread | 431.32 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 20.39 | --backend gpu |
| gpu, single host thread | 33.38 | --backend gpu --threads 1 |
| gpu with hull overlay | 23.4 | --backend gpu --hull |
