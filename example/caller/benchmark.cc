#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <chrono>
#include <cstdlib>

#include "mprpcapplication.h"
#include "mprpcchannel.h"
#include "mprpccontroller.h"
#include "user.pb.h"

struct BenchmarkConfig {
    int threads = 1;
    int requests_per_thread = 10;
    std::string service = "UserServiceRpc";
    std::string method = "Login";
};

struct CallMetrics {
    int64_t e2e_us;       // end-to-end latency
    int64_t connect_us;   // socket connect time (hard to measure separately here)
};

std::mutex g_metrics_mutex;
std::vector<CallMetrics> g_all_metrics;

void workerThread(const BenchmarkConfig& cfg, int thread_id) {
    for (int i = 0; i < cfg.requests_per_thread; ++i) {
        MprpcChannel channel(false);
        fixbug::UserServiceRpc_Stub stub(&channel);
        MprpcController controller;

        fixbug::LoginRequest request;
        request.set_username("benchmark_user");
        request.set_password("benchmark_pass");

        fixbug::LoginResponse response;

        auto t_start = std::chrono::high_resolution_clock::now();

        stub.Login(&controller, &request, &response, nullptr);

        auto t_end = std::chrono::high_resolution_clock::now();
        int64_t e2e_us = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();

        CallMetrics metrics;
        metrics.e2e_us = e2e_us;
        metrics.connect_us = 0;

        if (controller.Failed()) {
            std::cerr << "[thread " << thread_id << " req " << i << "] FAILED: "
                      << controller.ErrorText() << std::endl;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_metrics_mutex);
            g_all_metrics.push_back(metrics);
        }
    }
}

void printStats() {
    if (g_all_metrics.empty()) {
        std::cout << "No successful requests to report." << std::endl;
        return;
    }

    std::vector<int64_t> latencies;
    latencies.reserve(g_all_metrics.size());
    for (const auto& m : g_all_metrics) {
        latencies.push_back(m.e2e_us);
    }

    std::sort(latencies.begin(), latencies.end());

    size_t n = latencies.size();
    int64_t total_us = std::accumulate(latencies.begin(), latencies.end(), 0LL);
    int64_t min_us = latencies.front();
    int64_t max_us = latencies.back();
    double avg_us = static_cast<double>(total_us) / n;

    auto percentile = [&](double p) -> int64_t {
        size_t idx = static_cast<size_t>(p * n / 100.0);
        if (idx >= n) idx = n - 1;
        return latencies[idx];
    };

    double total_sec = static_cast<double>(total_us) / 1e6;
    double qps = static_cast<double>(n) / total_sec;

    std::cout << std::endl;
    std::cout << "========== Benchmark Results ==========" << std::endl;
    std::cout << "Successful requests: " << n << std::endl;
    std::cout << "Total time:           " << std::fixed << std::setprecision(3) << total_sec << " s" << std::endl;
    std::cout << "Throughput (QPS):     " << std::fixed << std::setprecision(1) << qps << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "End-to-End Latency (microseconds):" << std::endl;
    std::cout << "  min:  " << min_us << " us" << std::endl;
    std::cout << "  avg:  " << std::fixed << std::setprecision(1) << avg_us << " us" << std::endl;
    std::cout << "  max:  " << max_us << " us" << std::endl;
    std::cout << "  p50:  " << percentile(50) << " us" << std::endl;
    std::cout << "  p95:  " << percentile(95) << " us" << std::endl;
    std::cout << "  p99:  " << percentile(99) << " us" << std::endl;
    std::cout << "========================================" << std::endl;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " -i <config> [-t threads] [-n requests_per_thread] [-s service] [-m method]" << std::endl;
    std::cout << "  -i   Path to config file (required)" << std::endl;
    std::cout << "  -t   Number of concurrent threads (default: 1)" << std::endl;
    std::cout << "  -n   Number of requests per thread (default: 10)" << std::endl;
    std::cout << "  -s   Service name (default: UserServiceRpc)" << std::endl;
    std::cout << "  -m   Method name (default: Login)" << std::endl;
}

int main(int argc, char** argv) {
    BenchmarkConfig cfg;
    std::string config_file;

    // 手工解析参数，避免 MprpcApplication::Init 的 getopt 遇到未知参数直接 exit
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "-t" && i + 1 < argc) {
            cfg.threads = std::stoi(argv[++i]);
        } else if (arg == "-n" && i + 1 < argc) {
            cfg.requests_per_thread = std::stoi(argv[++i]);
        } else if (arg == "-s" && i + 1 < argc) {
            cfg.service = argv[++i];
        } else if (arg == "-m" && i + 1 < argc) {
            cfg.method = argv[++i];
        } else if (arg == "-h") {
            printUsage(argv[0]);
            _Exit(0);
        }
    }

    if (config_file.empty()) {
        printUsage(argv[0]);
        _Exit(1);
    }

    // 用只含 -i 的参数调用 MprpcApplication::Init
    {
        char fake_arg0[] = "benchmark";
        char fake_arg1[] = "-i";
        char* fake_argv[] = {fake_arg0, fake_arg1, &config_file[0], nullptr};
        int fake_argc = 3;
        MprpcApplication::Init(fake_argc, fake_argv);
    }

    std::cout << "Benchmark Configuration:" << std::endl;
    std::cout << "  Threads:       " << cfg.threads << std::endl;
    std::cout << "  Req/thread:    " << cfg.requests_per_thread << std::endl;
    std::cout << "  Total requests: " << cfg.threads * cfg.requests_per_thread << std::endl;
    std::cout << "  Service:       " << cfg.service << std::endl;
    std::cout << "  Method:        " << cfg.method << std::endl;
    std::cout << std::endl << "Running benchmark..." << std::endl;

    auto bench_start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(cfg.threads);
    for (int t = 0; t < cfg.threads; ++t) {
        threads.emplace_back(workerThread, std::cref(cfg), t);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    int64_t wall_us = std::chrono::duration_cast<std::chrono::microseconds>(bench_end - bench_start).count();
    double wall_sec = static_cast<double>(wall_us) / 1e6;

    std::cout << "Wall-clock time: " << std::fixed << std::setprecision(3) << wall_sec << " s" << std::endl;

    printStats();

    // 使用 _Exit 避免 Logger 守护线程在静态析构时阻塞退出
    std::cout << std::flush;
    _Exit(0);
}
