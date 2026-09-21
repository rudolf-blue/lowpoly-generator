#!/bin/sh
# ablation: run the pipeline with each optimisation switched off in turn.
# usage: scripts/ablate.sh <lowpoly binary> <image> <points> [label]
# prints one markdown row per configuration: median pipeline ms of 5 runs.
B=$1; IMG=$2; PTS=$3; LABEL=${4:-}
run() {
    name=$1; shift
    ms=""
    for i in 1 2 3 4 5; do
        t=$("$B" "$IMG" --no-stages -o /dev/null --points "$PTS" "$@" 2>/dev/null | awk '/^  total/ {print $2}')
        ms="$ms $t"
    done
    med=$(echo $ms | tr ' ' '\n' | sort -n | sed -n 3p)
    printf '| %s | %s | %s |\n' "$name" "$med" "$*"
}
echo "| configuration | ms | flags |"
echo "|---|---|---|"
run "baseline (all on, cpu)"            --backend cpu
run "single thread"                     --backend cpu --threads 1
run "float canny"                       --backend cpu --canny float
run "jump flooding voronoi"             --backend cpu --voronoi jfa
run "brute force voronoi"               --backend cpu --voronoi brute
run "sort+unique extraction"            --backend cpu --extract sort
run "owner-lookup raster (cpu)"         --backend cpu --raster owner
run "with hull overlay"                 --backend cpu --hull
run "everything off, single thread"     --backend cpu --threads 1 --canny float --voronoi jfa --extract sort
run "gpu"                               --backend gpu
run "gpu, single host thread"           --backend gpu --threads 1
run "gpu with hull overlay"             --backend gpu --hull
