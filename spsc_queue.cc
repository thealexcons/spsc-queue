#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <algorithm>

template <typename T>
class SpscQueue {
private:
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) size_t head_cached_{0};
    alignas(64) size_t tail_cached_{0};

public:
    SpscQueue(size_t capacity) : buffer_(capacity + 1){
        if (capacity + 1 < 0) {
            throw std::invalid_argument("capacity too big");
        }
    }

    bool enqueue(const T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t next_t = (t + 1) % buffer_.size();

        // Use the cached head first instead of loading the actual head from memory.
        // If they are equal, then we know that the queue may be full, so only then load
        // the actual value of head to check if currently full.
        if (next_t == head_cached_) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if (next_t == head_cached_) {
                return false;
            }
        }

        buffer_[t] = item;
        tail_.store(next_t, std::memory_order_release);
        return true;
    }

    bool dequeue(T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);

        // Use the cached tail first instead of loading the actual tail from memory.
        // If they are equal, then we know that the queue may be empty, so only then load
        // the actual value of tail to check if currently full.
        if (h == tail_cached_) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cached_) {
                return false;
            }
        }

        item = buffer_[h];
        const size_t next_h = (h + 1) % buffer_.size();
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    inline bool is_empty() const {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }
};

void pin_thread(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == -1) {
        perror("pthread_setaffinity_np failed");
        exit(EXIT_FAILURE);
    }
}

double median(std::vector<long long> &v){
    size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin()+n, v.end());
    int vn = v[n];
    if (v.size() % 2 == 1) {
        return vn;
    }

    std::nth_element(v.begin(), v.begin()+n-1, v.end());
    return 0.5*(vn+v[n-1]);
}

void print_metrics(std::vector<long long>& results) {
    long long sum = 0;
    for (auto r : results) sum += r;

    std::cout << std::fixed;
    std::cout.imbue(std::locale("en_US.utf8"));
    std::cout << "Mean: " << sum / static_cast<double>(results.size()) << " elems/s\n"; 
    std::cout << "Median: " << median(results) << " elems/s\n"; 
    std::cout << "Min: " << *std::min_element(results.begin(), results.end())  << " elems/s\n"; 
    std::cout << "Max: " << *std::max_element(results.begin(), results.end()) << " elems/s\n"; 
}


void run_benchmark(int cpu1, int cpu2, size_t iterations) {
    size_t  iter = 0;
    std::vector<long long> results(iterations, 0);

    while (iter < iterations) {
        constexpr size_t kQueueSize = 100000;
        constexpr int64_t elems = 100000000;
        SpscQueue<int> q(kQueueSize);

        auto consumer = std::thread([&] {
            pin_thread(cpu1);

            for (int i = 0; i < elems; i++) {
                int data = 0;
                while (!q.dequeue(data)) {}

                if (data != i) {
                    std::cerr << "invalid dequeue operation: expected " << i << " but got " << data << "\n";
                    exit(EXIT_FAILURE);
                }
            }
        });

        // Producer code (use the main thread)
        pin_thread(cpu2);
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < elems; i++) {
            while (!q.enqueue(i)) {}
        }

        // Wait until the consumer dequeues all the items
        while (!q.is_empty()) {}

        auto stop = std::chrono::steady_clock::now();
        consumer.join();

        results[iter] = elems * 1000000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        iter++;
    }

    print_metrics(results);
}


int main() {
    constexpr int cpu1 = 1;
    constexpr int cpu2 = 4;
    constexpr size_t iterations = 10;

    run_benchmark(cpu1, cpu2, iterations);

    return 0;
}