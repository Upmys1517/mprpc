# mprpc

基于 muduo + protobuf + ZooKeeper 的 C++ RPC 框架。

- 服务端基于 muduo `TcpServer` / `EventLoop`，业务只写 protobuf service，不感知网络层；
- 客户端通过 protobuf `Stub` / `Channel` 调用，框架负责序列化、服务发现和收发；
- ZooKeeper 做服务注册与发现，provider 上线注册临时节点，客户端按 `service/method` 解析地址；
- 提供多线程压测客户端与服务端分段计时，用来定位延迟分布。

## 目录结构

```
src/
  mprpcapplication.*   框架初始化（读取配置文件）
  mprpcconfig.*        ini 配置解析
  mprpcchannel.*       客户端：序列化、服务发现、连接池、收发
  mprpccontroller.*    RpcController 实现
  rpcprovider.*        服务端：服务注册、消息分发、响应
  zookeeperutil.*      ZooKeeper 客户端封装
  logger.* / lockqueue.h   异步日志（无锁队列 + 独立写线程）
example/
  callee/              服务提供方示例
  caller/              调用方示例 + benchmark 压测客户端
benchmark/results/     压测原始输出
```

## 线上协议

请求：

```
+----------------+-------------------+-------------------+
| header_size(4) | RpcHeader(args...) | args_str(args_size)|
+----------------+-------------------+-------------------+
```

`RpcHeader` 为 protobuf 结构：`service_name`、`method_name`、`args_size`。

响应：

```
+--------------+------------------+
| size(4)      | response_body     |
+--------------+------------------+
```

响应带 4 字节长度前缀，长连接下客户端可以精确读满一条响应，不会因为一次 `recv`
只拿到半个包而和后续响应错位。

## 构建

依赖：protobuf、muduo、zookeeper C client。

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DPROTOBUF_ROOT=/path/to/protobuf \
         -DCMAKE_PREFIX_PATH=/path/to/deps \
         -DCMAKE_CXX_FLAGS="-I/path/to/deps/include" \
         -DCMAKE_EXE_LINKER_FLAGS="-L/path/to/deps/lib -Wl,-rpath,/path/to/deps/lib"
make -j8
```

`PROTOBUF_ROOT` 用于指定非系统安装的 protobuf。修改 `.proto` 后重新生成：

```bash
cd src   && protoc --cpp_out=. rpcheader.proto
cd example && protoc --cpp_out=. user.proto
```

## 运行

```bash
# bench.conf: rpcserverip / rpcserverport / zookeeperip / zookeeperport
./bin/provider  -i bench.conf     # 服务端
./bin/consumer  -i bench.conf     # 单次调用示例
./bin/benchmark -i bench.conf -t 8 -n 200   # 压测：8 线程 × 200 次
```

## 压测结果

压测客户端为同步调用：每个线程发出请求、收到响应后才发下一个。

### 跨机压测（主要结果）

| 角色 | 机器 |
|---|---|
| 服务端（provider + ZooKeeper） | 2 × Intel Xeon Gold 6430（64 核 128 线程）、503 GiB 内存，Ubuntu 20.04.6 |
| 客户端（benchmark） | VMware 虚拟机，16 vCPU（Xeon Silver 4114）、31 GiB 内存，Ubuntu 22.04.2 |
| 网络 | 跨网段经路由转发，10 GbE，ping RTT 平均 1.53 ms |

每组重复 3 次，表中为中位数；运行脚本见 `benchmark/run_matrix.sh`。

**优化前**（客户端每次调用都新建 ZooKeeper 会话、新建 TCP 连接，服务端响应后断开）：

| 并发 | 总请求数 | 吞吐 | e2e p50 | e2e p95 | e2e p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 500 | 68 req/s | 14.11 ms | 17.79 ms | 20.35 ms |
| 4 | 2 000 | 183 req/s | 20.66 ms | 31.29 ms | 37.98 ms |
| 8 | 4 000 | 167 req/s | 47.99 ms | 84.02 ms | 115.16 ms |
| 16 | 8 000 | 170 req/s | 93.07 ms | 182.82 ms | 263.16 ms |

**优化后**（进程级复用 ZooKeeper 会话 + 地址缓存 + 连接池 + 长连接）：

| 并发 | 总请求数 | 吞吐 | e2e p50 | e2e p95 | e2e p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 5 000 | 650 req/s | 1.51 ms | 1.64 ms | 2.00 ms |
| 4 | 20 000 | 2 621 req/s | 1.48 ms | 1.65 ms | 2.10 ms |
| 8 | 40 000 | 5 218 req/s | 1.48 ms | 1.75 ms | 2.28 ms |
| 16 | 80 000 | 10 438 req/s | 1.48 ms | 1.73 ms | 2.32 ms |
| 32 | 160 000 | 19 201 req/s | 1.54 ms | 2.14 ms | 3.44 ms |
| 64 | 128 000 | 37 296 req/s | 1.58 ms | 1.95 ms | 2.60 ms |
| 128 | 256 000 | 56 914 req/s | 1.84 ms | 3.50 ms | 5.42 ms |

结论：

- 优化后单次调用约等于一次网络往返（p50 1.48 ms，ping RTT 1.53 ms），框架本身的开销
  已经不是延迟的主要来源；
- 优化前吞吐不随并发增长，稳定在约 170 req/s，并发越高排队越久（16 线程 p50 93 ms）。
  瓶颈是串行的，和每次调用新建 / 关闭 ZooKeeper 会话直接相关，不是网络带宽或服务端处理；
- 优化后吞吐随并发线性增长，到 128 线程时 p50 / p99 开始上升，此时 provider 进程 CPU 约
  280%（4 个 IO 线程 + 日志线程），接近服务端处理上限。

原始输出见 `benchmark/results/cross-host-before.txt` 与 `benchmark/results/cross-host-after.txt`。

### 同机回环压测

客户端与服务端同在服务端机器上，经 127.0.0.1 通信，每组单次运行。

| 并发 | 优化前吞吐 | 优化后吞吐 | 优化前 p50 | 优化后 p50 | 优化后 p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 128 req/s | 6 807 req/s | 7.12 ms | 66 µs | 84 µs |
| 4 | 283 req/s | 28 846 req/s | 13.59 ms | 93 µs | 290 µs |
| 8 | 286 req/s | 34 855 req/s | 26.40 ms | 177 µs | 510 µs |
| 16 | — | 46 377 req/s | — | 248 µs | 826 µs |

**服务端分段计时**（parse / process / response，8 403 次采样）：

| 阶段 | p50 | p90 | p99 | avg |
|---|---:|---:|---:|---:|
| parse | 0 µs | 1 µs | 2 µs | 0 µs |
| process | 48 µs | 118 µs | 217 µs | 60 µs |
| response | 9 µs | 11 µs | 16 µs | 9 µs |
| total | 58 µs | 130 µs | 230 µs | 70 µs |

服务端处理本身只有约 50 µs；优化前单次调用 7.12 ms，99% 以上耗在客户端重建
ZooKeeper 会话与 TCP 连接上。原始输出见 `benchmark/results/before-conn-reuse.txt` 与
`benchmark/results/after-conn-reuse.txt`。

## 服务注册

provider 启动时为每个方法注册临时节点 `/<Service>/<Method>`，内容为 `ip:port`。
节点已存在时按归属处理：

| 情况 | 处理 |
|---|---|
| 节点归属当前会话 | 已注册，幂等返回 |
| 归属其它会话，地址与本进程相同 | 上一次运行遗留（进程被 kill 后会话 30 s 内未超时）。provider 注册前已 bind 该端口，旧进程不可能仍在监听，带版本号删除后重建 |
| 归属其它会话，地址不同 | 另一个在线 provider 占用了该方法，拒绝注册，provider 退出 |
| 同名永久节点 | 无法注册临时节点，provider 退出 |

exists / create / delete 之间与会话超时或其它客户端并发时，按 `ZNODEEXISTS` /
`ZNONODE` / `ZBADVERSION` 重新检查，最多 5 次。

## 已知限制

- 连接池按进程共享，连接在单次调用期间被一个线程独占，暂不支持多路复用（协议没有
  request id，无法在一条连接上并发发多个请求）；
- 连接空闲被对端关闭时依赖调用失败后的重连重试，没有心跳保活；
- 服务地址缓存没有失效机制，provider 迁移地址后需要重启客户端；
- 每个方法只登记一个 provider 地址，同一方法的多实例负载均衡尚未支持。
