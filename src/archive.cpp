#include "zssk/archive.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zssk {

//  Kolejka ograniczona (watek glowny -> watki robocze)
// ============================================================================
struct Task {
    size_t idx;
    Bytes data;
};

class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : cap_(cap) {}
    void push(Task t) {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [&] { return q_.size() < cap_; });
        q_.push_back(std::move(t));
        notEmpty_.notify_one();
    }
    bool pop(Task& t) {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return false;
        t = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        notEmpty_.notify_all();
    }
private:
    std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<Task> q_;
    size_t cap_;
    bool closed_ = false;
};

// ============================================================================
//  Kompresja blokowa na puli watkow
// ============================================================================
std::vector<Bytes> compressBlocks(const BlockReader& next, size_t nblocks, Algo algo, int p) {
    std::vector<Bytes> results(nblocks);
    if (nblocks == 0) return results;
    p = std::max(1, p);
    BoundedQueue q(static_cast<size_t>(p) * 2);
    std::exception_ptr err;
    std::mutex errMu;

    auto worker = [&] {
        Task t;
        try {
            while (q.pop(t)) results[t.idx] = compressBlock(t.data, algo);  // rozne sloty - bez blokady
        } catch (...) {
            std::lock_guard<std::mutex> lk(errMu);
            if (!err) err = std::current_exception();
            // oproznij kolejke, aby producent sie nie zablokowal
            while (q.pop(t)) {}
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < p; ++i) pool.emplace_back(worker);

    try {
        for (size_t i = 0; i < nblocks; ++i) {
            Task t;
            t.idx = i;
            next(i, t.data);
            q.push(std::move(t));
        }
    } catch (...) {
        q.close();
        for (auto& th : pool) th.join();
        throw;
    }
    q.close();
    for (auto& th : pool) th.join();
    if (err) std::rethrow_exception(err);
    return results;
}

// ============================================================================
//  Format kontenera:
//  "ZSSK" | algo(1) | N(8) | B(4) | nblocks(8) | nblocks x rozmiar(4) | dane
// ============================================================================
static const size_t kHeaderFixed = 4 + 1 + 8 + 4 + 8;

size_t nBlocksFor(uint64_t N, uint64_t B) { return static_cast<size_t>((N + B - 1) / B); }

size_t containerSize(const std::vector<Bytes>& blocks) {
    size_t s = kHeaderFixed + 4 * blocks.size();
    for (auto& b : blocks) s += b.size();
    return s;
}

static void putLE(Bytes& o, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
static uint64_t getLE(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

Bytes serialize(const std::vector<Bytes>& blocks, Algo algo, uint64_t N, uint32_t B) {
    Bytes o;
    o.reserve(containerSize(blocks));
    const char magic[4] = {'Z', 'S', 'S', 'K'};
    o.insert(o.end(), magic, magic + 4);
    o.push_back(static_cast<uint8_t>(algo));
    putLE(o, N, 8);
    putLE(o, B, 4);
    putLE(o, blocks.size(), 8);
    for (auto& b : blocks) putLE(o, b.size(), 4);
    for (auto& b : blocks) o.insert(o.end(), b.begin(), b.end());
    return o;
}

// Dekompresja calego kontenera zaladowanego do pamieci
Bytes decompressBuffer(const Bytes& c, int p) {
    if (c.size() < kHeaderFixed || std::memcmp(c.data(), "ZSSK", 4) != 0)
        throw std::runtime_error("to nie jest plik ZSSK");
    const Algo algo = static_cast<Algo>(c[4]);
    if (c[4] > 1) throw std::runtime_error("nieznany algorytm w pliku");
    const uint64_t N = getLE(&c[5], 8);
    const uint64_t B = getLE(&c[13], 4);
    const uint64_t nb = getLE(&c[17], 8);
    if (B == 0 || nb != nBlocksFor(N, B)) throw std::runtime_error("uszkodzony naglowek");
    if (c.size() < kHeaderFixed + 4 * nb) throw std::runtime_error("uszkodzony indeks");

    std::vector<size_t> off(nb + 1);
    off[0] = kHeaderFixed + 4 * nb;
    for (size_t i = 0; i < nb; ++i) off[i + 1] = off[i] + getLE(&c[kHeaderFixed + 4 * i], 4);
    if (off[nb] != c.size()) throw std::runtime_error("zly rozmiar pliku");

    Bytes out(N);
    p = std::max(1, p);
    std::atomic<size_t> nextIdx{0};
    std::exception_ptr err;
    std::mutex errMu;
    auto worker = [&] {
        try {
            for (;;) {
                size_t i = nextIdx.fetch_add(1);
                if (i >= nb) break;
                size_t outOff = i * B;
                size_t outLen = std::min<uint64_t>(B, N - outOff);
                decompressBlock(&c[off[i]], off[i + 1] - off[i], out.data() + outOff, outLen, algo);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lk(errMu);
            if (!err) err = std::current_exception();
            nextIdx = nb;
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < p; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (err) std::rethrow_exception(err);
    return out;
}

// Kompresja bufora z pamieci -> zestaw skompresowanych blokow
std::vector<Bytes> compressBuffer(const Bytes& data, Algo algo, uint32_t B, int p) {
    const size_t N = data.size();
    BlockReader rd = [&](size_t i, Bytes& blk) {
        size_t o = i * B;
        size_t l = std::min<size_t>(B, N - o);
        blk.assign(data.begin() + o, data.begin() + o + l);
    };
    return compressBlocks(rd, nBlocksFor(N, B), algo, p);
}

// ============================================================================

} // namespace zssk
