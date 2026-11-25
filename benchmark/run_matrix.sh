#!/usr/bin/env bash
# 按并发度矩阵重复运行 benchmark，原始输出追加到结果文件。
#
# 用法：
#   benchmark/run_matrix.sh <benchmark 可执行文件> <配置文件> <结果文件> \
#       <每线程请求数> <重复次数> <并发度...>
#
# 例：
#   benchmark/run_matrix.sh ./bin/benchmark bench.conf results/x.txt 5000 3 1 4 8 16
#
# 每组输出以 "### threads=T n=N run=R" 开头，便于后续按组统计中位数。
set -u

if [ $# -lt 6 ]; then
    sed -n '2,10p' "$0"
    exit 1
fi

BIN=$1; CONF=$2; OUT=$3; N=$4; REPEAT=$5
shift 5

{
    echo "# host: $(hostname)  kernel: $(uname -r)"
    echo "# cpu: $(lscpu | grep -m1 'Model name' | sed 's/.*: *//')  vcpu: $(nproc)"
    echo "# date: $(date '+%F %T %z')"
    echo "# config: $(grep -E 'rpcserverip|rpcserverport|zookeeperip' "$CONF" | tr '\n' ' ')"
} >> "$OUT"

for t in "$@"; do
    for r in $(seq 1 "$REPEAT"); do
        echo "### threads=$t n=$N run=$r" >> "$OUT"
        "$BIN" -i "$CONF" -t "$t" -n "$N" 2>/dev/null \
            | grep -E 'Successful|Wall-clock|QPS|min:|avg:|max:|p50:|p95:|p99:' >> "$OUT"
    done
done
