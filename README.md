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

**环境**：2 × Intel Xeon Gold 6430（32 核/路，2 线程/核，共 128 vCPU）、503 GiB 内存、
Ubuntu 20.04.6 / kernel 5.4.0，客户端与服务端同机回环（127.0.0.1）。

**优化前**（客户端每次调用都重建 ZooKeeper 会话、重新 TCP 连接）：

| 并发 | 请求数 | 墙钟 | 吞吐 | e2e p50 | e2e p95 | e2e p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 200 | 1.565 s | 128 req/s | 7.12 ms | 10.58 ms | 19.17 ms |
| 4 | 800 | 2.831 s | 283 req/s | 13.59 ms | 19.62 ms | 34.95 ms |
| 8 | 1600 | 5.601 s | 286 req/s | 26.40 ms | 33.14 ms | 35.31 ms |

**优化后**（进程级复用 ZooKeeper 会话 + 地址缓存 + 连接池 + 长连接）：

| 并发 | 请求数 | 墙钟 | 吞吐 | e2e p50 | e2e p95 | e2e p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 200 | 0.029 s | 6 807 req/s | 66 µs | 71 µs | 84 µs |
| 4 | 800 | 0.028 s | 28 846 req/s | 93 µs | 172 µs | 290 µs |
| 8 | 1600 | 0.046 s | 34 855 req/s | 177 µs | 359 µs | 510 µs |
| 16 | 3200 | 0.069 s | 46 377 req/s | 248 µs | 625 µs | 826 µs |

**服务端分段计时**（parse / process / response，8 403 次采样）：

| 阶段 | p50 | p90 | p99 | avg |
|---|---:|---:|---:|---:|
| parse | 0 µs | 1 µs | 2 µs | 0 µs |
| process | 48 µs | 118 µs | 217 µs | 60 µs |
| response | 9 µs | 11 µs | 16 µs | 9 µs |
| total | 58 µs | 130 µs | 230 µs | 70 µs |

单线程端到端从 7.12 ms 降到 66 µs：优化前 99% 以上的时间耗在客户端每次调用重建
ZooKeeper 会话与 TCP 连接上，服务端处理本身只有约 50 µs。

原始输出见 `benchmark/results/before-conn-reuse.txt` 与
`benchmark/results/after-conn-reuse.txt`。

## 已知限制

- 连接池按进程共享，连接在单次调用期间被一个线程独占，暂不支持多路复用（协议没有
  request id，无法在一条连接上并发发多个请求）；
- 连接空闲被对端关闭时依赖调用失败后的重连重试，没有心跳保活；
- 服务地址缓存没有失效机制，provider 迁移地址后需要重启客户端；
- `RpcProvider` 注册 znode 时未检查返回值，若上次 provider 的临时节点尚未过期，
  本次注册会静默失败。
