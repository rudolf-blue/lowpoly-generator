## RTX 3060 Ti host (16 threads, CUDA) — aspen 600x450, 10k points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 18.10 | --backend cpu |
| single thread | 55.28 | --backend cpu --threads 1 |
| float canny | 22.72 | --backend cpu --canny float |
| jump flooding voronoi | 30.23 | --backend cpu --voronoi jfa |
| brute force voronoi | 524.79 | --backend cpu --voronoi brute |
| sort+unique extraction | 19.89 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 18.37 | --backend cpu --raster owner |
| with hull overlay | 19.00 | --backend cpu --hull |
| everything off, single thread | 106.35 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 16.84 | --backend gpu |
| gpu, single host thread | 21.41 | --backend gpu --threads 1 |
| gpu with hull overlay | 16.94 | --backend gpu --hull |

## RTX 3060 Ti host — alto 2000x1125, 30k points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 77.19 | --backend cpu |
| single thread | 327.30 | --backend cpu --threads 1 |
| float canny | 145.07 | --backend cpu --canny float |
| jump flooding voronoi | 158.42 | --backend cpu --voronoi jfa |
| brute force voronoi | 12269.38 | --backend cpu --voronoi brute |
| sort+unique extraction | 84.72 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 90.02 | --backend cpu --raster owner |
| with hull overlay | 80.78 | --backend cpu --hull |
| everything off, single thread | 807.17 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 58.81 | --backend gpu |
| gpu, single host thread | 72.92 | --backend gpu --threads 1 |
| gpu with hull overlay | 57.20 | --backend gpu --hull |
