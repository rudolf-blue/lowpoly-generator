canny-flower.jpg
flower.jpg
monarch.jpg
moto-butterfly.jpg
## RTX 3060 Ti host — examples/canny-flower.jpg, 10000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 20.21 | --backend cpu |
| single thread | 66.23 | --backend cpu --threads 1 |
| float canny | 31.08 | --backend cpu --canny float |
| jump flooding voronoi | 37.32 | --backend cpu --voronoi jfa |
| brute force voronoi | 890.50 | --backend cpu --voronoi brute |
| sort+unique extraction | 22.83 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 22.54 | --backend cpu --raster owner |
| with hull overlay | 21.40 | --backend cpu --hull |
| everything off, single thread | 175.49 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 18.88 | --backend gpu |
| gpu, single host thread | 23.56 | --backend gpu --threads 1 |
| gpu with hull overlay | 20.29 | --backend gpu --hull |

## RTX 3060 Ti host — examples/flower.jpg, 10000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 25.81 | --backend cpu |
| single thread | 94.75 | --backend cpu --threads 1 |
| float canny | 42.02 | --backend cpu --canny float |
| jump flooding voronoi | 50.53 | --backend cpu --voronoi jfa |
| brute force voronoi | 1354.36 | --backend cpu --voronoi brute |
| sort+unique extraction | 27.35 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 28.04 | --backend cpu --raster owner |
| with hull overlay | 26.50 | --backend cpu --hull |
| everything off, single thread | 233.00 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 23.34 | --backend gpu |
| gpu, single host thread | 31.62 | --backend gpu --threads 1 |
| gpu with hull overlay | 23.95 | --backend gpu --hull |

## RTX 3060 Ti host — examples/monarch.jpg, 10000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 43.02 | --backend cpu |
| single thread | 173.98 | --backend cpu --threads 1 |
| float canny | 72.52 | --backend cpu --canny float |
| jump flooding voronoi | 89.98 | --backend cpu --voronoi jfa |
| brute force voronoi | 2337.13 | --backend cpu --voronoi brute |
| sort+unique extraction | 47.65 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 53.97 | --backend cpu --raster owner |
| with hull overlay | 49.63 | --backend cpu --hull |
| everything off, single thread | 451.50 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 41.22 | --backend gpu |
| gpu, single host thread | 59.69 | --backend gpu --threads 1 |
| gpu with hull overlay | 37.47 | --backend gpu --hull |

## RTX 3060 Ti host — examples/moto-butterfly.jpg, 30000 points
| configuration | ms | flags |
|---|---|---|
| baseline (all on, cpu) | 134.14 | --backend cpu |
| single thread | 580.13 | --backend cpu --threads 1 |
| float canny | 255.67 | --backend cpu --canny float |
| jump flooding voronoi | 322.03 | --backend cpu --voronoi jfa |
| brute force voronoi | 24982.57 | --backend cpu --voronoi brute |
| sort+unique extraction | 146.79 | --backend cpu --extract sort |
| owner-lookup raster (cpu) | 154.40 | --backend cpu --raster owner |
| with hull overlay | 134.99 | --backend cpu --hull |
| everything off, single thread | 1646.13 | --backend cpu --threads 1 --canny float --voronoi jfa --extract sort |
| gpu | 94.91 | --backend gpu |
| gpu, single host thread | 126.30 | --backend gpu --threads 1 |
| gpu with hull overlay | 97.88 | --backend gpu --hull |

