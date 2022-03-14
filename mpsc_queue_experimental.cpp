#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <algorithm>
#include <mutex>


template <typename T>
class MpscQueue {
private:
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<size_t> count_{0};
    // alignas(64) size_t head_cached_{0};
    // alignas(64) size_t tail_cached_{0};

public:
    MpscQueue(size_t capacity) : buffer_(capacity + 1) {
        if (capacity + 1 < 0 && (buffer_.size() & (buffer_.size() - 1)) == 0) {
            throw std::invalid_argument("capacity too big");
        }
        for (size_t i = 0; i < buffer_.size(); i++) {
            volatile T r = buffer_[i]; // force early page allocation to avoid page faults
            (void) r;
        }
    }

    // This may be called by multiple producer threads
    bool enqueue(const T& item) {
        size_t t;
        size_t next_t;

        do {
            t = tail_.load(std::memory_order_acquire);
            next_t = (t + 1) % buffer_.size();

            // check if full
            if (next_t == head_.load(std::memory_order_acquire)) {
                return false;
            }

        // verify that tail_ was not updated first by another thread
        } while (!tail_.compare_exchange_strong(t, next_t, std::memory_order_acq_rel));

        buffer_[t] = item;
        return true;
    }

    // This will be called only by a single consumer thread
    bool dequeue(T& item) {
        // This can be a relaxed load because the producers do not modify head_
        const size_t h = head_.load(std::memory_order_relaxed);

        // Check if empty
        if (h == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[h];
        const size_t next_h = (h + 1) % buffer_.size();
        head_.store(next_h, std::memory_order_seq_cst); // need an mfence here
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


void run_benchmark(size_t iterations) {
    size_t iter = 0;
    std::vector<long long> results(iterations, 0);

    while (iter < iterations) {
        constexpr size_t kQueueSize = (1 << 17) - 1;
        constexpr int64_t elems = 1'150'000;
        constexpr int64_t consumers = 3;
        // static_assert(elems*consumers < kQueueSize);

        MpscQueue<int> q(kQueueSize);
        std::atomic<int> signal(0);

        std::vector<int> consumed_elems(elems, 0);
        
        auto start = std::chrono::steady_clock::now();
        
        auto consumer = std::thread([&] {
            pin_thread(0);

            // wait at least 2 of the producers have added their data
            // while (signal.load() != 3) {}

            std::cout << "Start consuming, here is the queue...\n";

            for (int i = 0; i < consumers*elems; i++) {
                int data = 99;
                while (!q.dequeue(data)) {}
                consumed_elems[data] += 1;
                // std::cout << "Dequeued " << data << '\n';

                // if (data != i) {
                    // std::cerr << "invalid dequeue operation: expected " << i << " but got " << data << "\n";
                    // exit(EXIT_FAILURE);
                // }
            }

            signal.fetch_add(1);
        });

        auto producer1 = std::thread([&] {
            pin_thread(1);

            for (int i = 0; i < elems; i++) {
                while (!q.enqueue(i)) {}
                // std::cout << "T1 enqueue("<<i<<")\n";
            }
            std::cout << "Done producer 1...\n";
            signal.fetch_add(1);
        });

        auto producer2 = std::thread([&] {
            pin_thread(2);

            for (int i = 0; i < elems; i++) {
                while (!q.enqueue(i)) {}
                // std::cout << "T2 enqueue("<<i<<")\n";
            }
            std::cout << "Done producer 2...\n";
            signal.fetch_add(1);
        });

        auto producer3 = std::thread([&] {
            pin_thread(3);

            for (int i = 0; i < elems; i++) {
                while (!q.enqueue(i)) {}
                // std::cout << "T3 enqueue("<<i<<")\n";
            }
            std::cout << "Done producer 3...\n";
            signal.fetch_add(1);
        });

        // Wait until the consumer dequeues all the items
        while (!q.is_empty() && signal.load() == 4) {}

        consumer.join();
        producer1.join();
        producer2.join();
        producer3.join();

        auto stop = std::chrono::steady_clock::now();


        for (size_t i = 0; i < consumed_elems.size(); i++) {
            if (consumed_elems[i] != consumers) {
                std::cerr << "invalid dequeue operation: expected a count of 3 at idx " << i << " but got count " << consumed_elems[i] << "\n";
                // for (size_t j = 0; j < consumed_elems.size(); j++) {
                //     std::cout << consumed_elems[j] << " ";
                // }
                std::cout << '\n';
                exit(EXIT_FAILURE);
            }
        }

        results[iter] = 3 * elems * 1000000000 / std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        iter++;
    }

    print_metrics(results);
}


int main() {
    // constexpr int cpu1 = 1;
    // constexpr int cpu2 = 4;
    constexpr size_t iterations = 10;

    run_benchmark(iterations);

    return 0;
}
