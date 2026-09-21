## Apple M5 Pro — examples/canny-flower.jpg, 10000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 7.18 | --backend cpu |
| single thread | 58.27 | --backend cpu --threads 1 |
| float canny | 7.40 | --backend cpu --canny float |
| jump flooding voronoi | 10.35 | --backend cpu --voronoi jfa |
| brute force voronoi | 447.93 | --backend cpu --voronoi brute |
| sort+unique extraction | 8.09 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 7.11 | --backend cpu --raster owner |
| with hull overlay | 7.51 | --backend cpu --hull |
| everything off, single thread | 101.97 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 9.77 | --backend gpu |
| gpu, single host thread | 16.48 | --backend gpu --threads 1 |
| gpu with hull overlay | 10.44 | --backend gpu --hull |

## Apple M5 Pro — examples/flower.jpg, 10000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 9.05 | --backend cpu |
| single thread | 76.97 | --backend cpu --threads 1 |
| float canny | 9.86 | --backend cpu --canny float |
| jump flooding voronoi | 13.92 | --backend cpu --voronoi jfa |
| brute force voronoi | 640.42 | --backend cpu --voronoi brute |
| sort+unique extraction | 9.89 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 9.13 | --backend cpu --raster owner |
| with hull overlay | 9.48 | --backend cpu --hull |
| everything off, single thread | 138.95 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 11.03 | --backend gpu |
| gpu, single host thread | 18.83 | --backend gpu --threads 1 |
| gpu with hull overlay | 11.77 | --backend gpu --hull |

## Apple M5 Pro — examples/monarch.jpg, 10000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 15.45 | --backend cpu |
| single thread | 125.79 | --backend cpu --threads 1 |
| float canny | 17.16 | --backend cpu --canny float |
| jump flooding voronoi | 24.95 | --backend cpu --voronoi jfa |
| brute force voronoi | 1279.03 | --backend cpu --voronoi brute |
| sort+unique extraction | 16.70 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 16.12 | --backend cpu --raster owner |
| with hull overlay | 16.19 | --backend cpu --hull |
| everything off, single thread | 246.67 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 16.74 | --backend gpu |
| gpu, single host thread | 28.30 | --backend gpu --threads 1 |
| gpu with hull overlay | 17.39 | --backend gpu --hull |

## Apple M5 Pro — examples/moto-butterfly.jpg, 30000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 38.16 | --backend cpu |
| single thread | 400.25 | --backend cpu --threads 1 |
| float canny | 44.86 | --backend cpu --canny float |
| jump flooding voronoi | 73.69 | --backend cpu --voronoi jfa |
| brute force voronoi | 15690.23 | --backend cpu --voronoi brute |
| sort+unique extraction | 42.60 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 39.54 | --backend cpu --raster owner |
| with hull overlay | 41.55 | --backend cpu --hull |
| everything off, single thread | 915.01 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 44.90 | --backend gpu |
| gpu, single host thread | 60.13 | --backend gpu --threads 1 |
| gpu with hull overlay | 37.36 | --backend gpu --hull |

