// ZSSK - Wielowatkowa kompresja plikow z podzialem na bloki (RLE, Huffman)
// C++17, tylko STL.  Kompilacja:
//   g++ -O2 -std=c++17 -pthread zssk.cpp -o zssk            (Linux/MinGW)
//   MSVC: cl /O2 /std:c++17 /EHsc zssk.cpp
//
// Uzycie:
//   zssk compress   <wejscie> <wyjscie> [--algo rle|huff] [--block K] [--threads P]
//   zssk decompress <wejscie> <wyjscie> [--threads P]
//   zssk bench      <plik> [--algo rle|huff|both] [--threads 1,2,4,8]
//                   [--bmin 10] [--bmax 26] [--reps 3] [--csv wyniki.csv]
//                   [--memlimit MB] [--R0 x] [--T0 ms] [--w 0.5]
//   zssk selftest
// K - wykladnik rozmiaru bloku: B = 2^K (10..26).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Bytes = std::vector<uint8_t>;

enum class Algo : uint8_t { RLE = 0, HUFF = 1 };

static const char* algoName(Algo a) { return a == Algo::RLE ? "rle" : "huff"; }

static Algo parseAlgo(const std::string& s) {
    if (s == "rle") return Algo::RLE;
    if (s == "huff" || s == "huffman") return Algo::HUFF;
    throw std::runtime_error("nieznany algorytm: " + s);
}

// ============================================================================
//  RLE: pary (liczba powtorzen 1..255, wartosc)
// ============================================================================
static Bytes rleCompress(const uint8_t* d, size_t n) {
    Bytes o;
    o.reserve(n / 2 + 16);
    size_t i = 0;
    while (i < n) {
        uint8_t v = d[i];
        size_t j = i + 1;
        while (j < n && d[j] == v && j - i < 255) ++j;
        o.push_back(static_cast<uint8_t>(j - i));
        o.push_back(v);
        i = j;
    }
    return o;
}

static void rleDecompress(const uint8_t* in, size_t inN, uint8_t* out, size_t outN) {
    if (inN % 2) throw std::runtime_error("RLE: uszkodzone dane");
    size_t o = 0;
    for (size_t i = 0; i < inN; i += 2) {
        size_t c = in[i];
        if (c == 0 || o + c > outN) throw std::runtime_error("RLE: uszkodzone dane");
        std::memset(out + o, in[i + 1], c);
        o += c;
    }
    if (o != outN) throw std::runtime_error("RLE: zly rozmiar bloku");
}

// ============================================================================
//  Huffman (kod kanoniczny). Blok: 256 bajtow dlugosci kodow + strumien bitow.
// ============================================================================
static const int kMaxCodeLen = 56;

static void huffLengths(const uint64_t freq[256], uint8_t len[256]) {
    struct Node { uint64_t f; int l, r; };
    std::vector<Node> nodes;
    std::vector<int> sym;  // symbol dla lisci (indeksy 0..nleaves-1)
    using P = std::pair<uint64_t, int>;
    std::priority_queue<P, std::vector<P>, std::greater<P>> pq;
    std::memset(len, 0, 256);
    for (int s = 0; s < 256; ++s) {
        if (freq[s]) {
            nodes.push_back({freq[s], -1, -1});
            sym.push_back(s);
            pq.push({freq[s], static_cast<int>(nodes.size()) - 1});
        }
    }
    const int nleaves = static_cast<int>(nodes.size());
    if (nleaves == 0) return;
    if (nleaves == 1) { len[sym[0]] = 1; return; }
    while (pq.size() > 1) {
        P a = pq.top(); pq.pop();
        P b = pq.top(); pq.pop();
        nodes.push_back({a.first + b.first, a.second, b.second});
        pq.push({a.first + b.first, static_cast<int>(nodes.size()) - 1});
    }
    std::vector<std::pair<int, int>> st;  // (wezel, glebokosc)
    st.push_back({pq.top().second, 0});
    while (!st.empty()) {
        auto [idx, depth] = st.back();
        st.pop_back();
        if (nodes[idx].l < 0) {
            if (depth > kMaxCodeLen) throw std::runtime_error("Huffman: zbyt dlugi kod");
            len[sym[idx]] = static_cast<uint8_t>(depth);
        } else {
            st.push_back({nodes[idx].l, depth + 1});
            st.push_back({nodes[idx].r, depth + 1});
        }
    }
}

static Bytes huffCompress(const uint8_t* d, size_t n) {
    uint64_t freq[256] = {0};
    for (size_t i = 0; i < n; ++i) ++freq[d[i]];
    uint8_t len[256];
    huffLengths(freq, len);

    uint32_t cnt[65] = {0};
    for (int s = 0; s < 256; ++s) if (len[s]) ++cnt[len[s]];
    uint64_t next[65], code = 0;
    for (int l = 1; l <= 64; ++l) {
        next[l] = code;
        code = (code + cnt[l]) << 1;
    }
    uint64_t codes[256] = {0};
    for (int s = 0; s < 256; ++s) if (len[s]) codes[s] = next[len[s]]++;

    Bytes o;
    o.reserve(256 + n / 2 + 16);
    o.insert(o.end(), len, len + 256);

    uint64_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < n; ++i) {
        int l = len[d[i]];
        acc = (acc << l) | codes[d[i]];
        nbits += l;
        while (nbits >= 8) {
            nbits -= 8;
            o.push_back(static_cast<uint8_t>((acc >> nbits) & 0xFF));
        }
    }
    if (nbits > 0) o.push_back(static_cast<uint8_t>((acc << (8 - nbits)) & 0xFF));
    return o;
}

static void huffDecompress(const uint8_t* in, size_t inN, uint8_t* out, size_t outN) {
    if (outN == 0) return;
    if (inN < 256) throw std::runtime_error("Huffman: uszkodzony naglowek");
    const uint8_t* len = in;
    uint32_t cnt[65] = {0};
    int maxLen = 0;
    for (int s = 0; s < 256; ++s) {
        if (len[s] > kMaxCodeLen) throw std::runtime_error("Huffman: zla dlugosc kodu");
        if (len[s]) { ++cnt[len[s]]; maxLen = std::max<int>(maxLen, len[s]); }
    }
    if (maxLen == 0) throw std::runtime_error("Huffman: pusty slownik");
    uint64_t first[65];
    uint32_t offset[65], pos[65];
    uint64_t code = 0;
    uint32_t off = 0;
    for (int l = 1; l <= 64; ++l) {
        first[l] = code;
        offset[l] = pos[l] = off;
        off += cnt[l];
        code = (code + cnt[l]) << 1;
    }
    uint8_t sorted[256];
    for (int s = 0; s < 256; ++s) if (len[s]) sorted[pos[len[s]]++] = static_cast<uint8_t>(s);

    const uint8_t* data = in + 256;
    const size_t totalBits = (inN - 256) * 8;
    size_t bp = 0;
    for (size_t i = 0; i < outN; ++i) {
        uint64_t c = 0;
        int l = 0;
        for (;;) {
            if (bp >= totalBits) throw std::runtime_error("Huffman: koniec danych");
            c = (c << 1) | ((data[bp >> 3] >> (7 - (bp & 7))) & 1);
            ++bp;
            ++l;
            if (l > maxLen) throw std::runtime_error("Huffman: uszkodzone dane");
            if (cnt[l] && c >= first[l] && c - first[l] < cnt[l]) {
                out[i] = sorted[offset[l] + (c - first[l])];
                break;
            }
        }
    }
}

static Bytes compressBlock(const Bytes& b, Algo a) {
    return a == Algo::RLE ? rleCompress(b.data(), b.size()) : huffCompress(b.data(), b.size());
}

static void decompressBlock(const uint8_t* in, size_t inN, uint8_t* out, size_t outN, Algo a) {
    if (a == Algo::RLE) rleDecompress(in, inN, out, outN);
    else huffDecompress(in, inN, out, outN);
}

// ============================================================================
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
using BlockReader = std::function<void(size_t idx, Bytes& block)>;

static std::vector<Bytes> compressBlocks(const BlockReader& next, size_t nblocks, Algo algo, int p) {
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

static size_t nBlocksFor(uint64_t N, uint64_t B) { return static_cast<size_t>((N + B - 1) / B); }

static size_t containerSize(const std::vector<Bytes>& blocks) {
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

static Bytes serialize(const std::vector<Bytes>& blocks, Algo algo, uint64_t N, uint32_t B) {
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
static Bytes decompressBuffer(const Bytes& c, int p) {
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
static std::vector<Bytes> compressBuffer(const Bytes& data, Algo algo, uint32_t B, int p) {
    const size_t N = data.size();
    BlockReader rd = [&](size_t i, Bytes& blk) {
        size_t o = i * B;
        size_t l = std::min<size_t>(B, N - o);
        blk.assign(data.begin() + o, data.begin() + o + l);
    };
    return compressBlocks(rd, nBlocksFor(N, B), algo, p);
}

// ============================================================================
//  Narzedzia: I/O, suma kontrolna, czas
// ============================================================================
static uint64_t fnv1a64(const Bytes& d) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : d) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

static Bytes readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("nie mozna otworzyc: " + path);
    std::streamsize n = f.tellg();
    f.seekg(0);
    Bytes b(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(b.data()), n)) throw std::runtime_error("blad odczytu: " + path);
    return b;
}

static void writeWholeFile(const std::string& path, const Bytes& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("nie mozna zapisac: " + path);
    if (!b.empty()) f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("blad zapisu: " + path);
}

using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ============================================================================
//  Polecenia: compress / decompress (z sekwencyjnym odczytem pliku)
// ============================================================================
static void cmdCompress(const std::string& in, const std::string& out, Algo algo, int k, int p) {
    if (k < 10 || k > 26) throw std::runtime_error("--block musi byc w zakresie 10..26");
    const uint32_t B = 1u << k;
    std::ifstream f(in, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("nie mozna otworzyc: " + in);
    const uint64_t N = static_cast<uint64_t>(f.tellg());
    f.seekg(0);

    auto t0 = Clock::now();
    BlockReader rd = [&](size_t i, Bytes& blk) {
        size_t l = static_cast<size_t>(std::min<uint64_t>(B, N - i * static_cast<uint64_t>(B)));
        blk.resize(l);
        if (!f.read(reinterpret_cast<char*>(blk.data()), static_cast<std::streamsize>(l)))
            throw std::runtime_error("blad odczytu pliku");
    };
    auto blocks = compressBlocks(rd, nBlocksFor(N, B), algo, p);
    Bytes c = serialize(blocks, algo, N, B);
    writeWholeFile(out, c);
    double ms = msSince(t0);
    std::cout << "algorytm=" << algoName(algo) << " B=2^" << k << " p=" << p << "\n"
              << "N=" << N << " -> " << c.size() << " bajtow, R="
              << (N ? static_cast<double>(c.size()) / N : 0.0) << ", czas=" << ms << " ms\n";
}

static void cmdDecompress(const std::string& in, const std::string& out, int p) {
    auto t0 = Clock::now();
    Bytes c = readWholeFile(in);
    Bytes d = decompressBuffer(c, p);
    writeWholeFile(out, d);
    std::cout << "rozpakowano " << d.size() << " bajtow, czas=" << msSince(t0) << " ms, FNV-1a="
              << std::hex << fnv1a64(d) << std::dec << "\n";
}

// ============================================================================
//  Tryb badawczy (przeglad zupelny po B i p)
// ============================================================================
struct Row {
    Algo algo;
    int k;
    int p;
    double R, T, S;
    bool ok;
    size_t memBytes;
};

static std::vector<int> parseIntList(const std::string& s) {
    std::vector<int> v;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) v.push_back(std::stoi(tok));
    return v;
}

static void cmdBench(const std::string& path, const std::string& algoArg, const std::vector<int>& threads,
                     int bmin, int bmax, int reps, const std::string& csvPath, double memLimitMB,
                     double R0, double T0, double w) {
    Bytes data = readWholeFile(path);
    const size_t N = data.size();
    if (N == 0) throw std::runtime_error("plik pusty - nie ma czego mierzyc");
    const uint64_t refSum = fnv1a64(data);
    std::vector<Algo> algos;
    if (algoArg == "both") algos = {Algo::RLE, Algo::HUFF};
    else algos = {parseAlgo(algoArg)};

    unsigned hw = std::thread::hardware_concurrency();
    for (int p : threads)
        if (hw && p > static_cast<int>(hw))
            std::cerr << "Uwaga: p=" << p << " > liczba rdzeni (" << hw << ")\n";

    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        if (!csv) throw std::runtime_error("nie mozna zapisac CSV: " + csvPath);
        csv << "plik,algorytm,N,B,k,p,R,T_ms,S,pamiec_bajty,poprawne\n";
    }
    std::cout << std::left << std::setw(6) << "algo" << std::setw(5) << "k" << std::setw(4) << "p"
              << std::setw(10) << "R" << std::setw(12) << "T[ms]" << std::setw(8) << "S" << "OK\n";

    std::vector<Row> rows;
    for (Algo algo : algos) {
        for (int k = bmin; k <= bmax; ++k) {
            const uint32_t B = 1u << k;
            if (k > bmin && (1ull << (k - 1)) >= N) break;  // wiecej niz jeden blok juz niemozliwy
            std::map<int, double> tOf;
            bool verified = false, okFlag = true;
            std::vector<Row> local;
            for (int p : threads) {
                size_t mem = static_cast<size_t>(p) * B;
                if (memLimitMB > 0 && mem > memLimitMB * 1024 * 1024) continue;
                double sum = 0;
                std::vector<Bytes> blocks;
                for (int r = 0; r < reps; ++r) {
                    auto t0 = Clock::now();
                    blocks = compressBuffer(data, algo, B, p);
                    sum += msSince(t0);
                }
                if (!verified) {  // poprawnosc: dekompresja == oryginal
                    Bytes back = decompressBuffer(serialize(blocks, algo, N, B), p);
                    okFlag = (back.size() == N && fnv1a64(back) == refSum);
                    verified = true;
                }
                Row row{algo, k, p, static_cast<double>(containerSize(blocks)) / N, sum / reps, 0.0, okFlag, mem};
                tOf[p] = row.T;
                local.push_back(row);
            }
            for (auto& row : local) {
                row.S = tOf.count(1) ? tOf[1] / row.T : 0.0;
                std::cout << std::left << std::setw(6) << algoName(row.algo) << std::setw(5) << row.k
                          << std::setw(4) << row.p << std::setw(10) << std::fixed << std::setprecision(4) << row.R
                          << std::setw(12) << std::setprecision(2) << row.T << std::setw(8) << row.S
                          << (row.ok ? "tak" : "NIE") << "\n";
                if (csv)
                    csv << path << "," << algoName(row.algo) << "," << N << "," << (1u << row.k) << "," << row.k
                        << "," << row.p << "," << std::setprecision(6) << row.R << "," << row.T << "," << row.S
                        << "," << row.memBytes << "," << (row.ok ? 1 : 0) << "\n";
                rows.push_back(row);
            }
        }
    }
    if (rows.empty()) { std::cout << "Brak wynikow (limit pamieci?)\n"; return; }

    // Problem decyzyjny: istnieja (B,p) z R<=R0 i T<=T0 ?
    if (R0 > 0 && T0 > 0) {
        int found = 0;
        for (auto& r : rows)
            if (r.R <= R0 && r.T <= T0) {
                if (!found) std::cout << "\nRozwiazania spelniajace R<=" << R0 << " i T<=" << T0 << " ms:\n";
                std::cout << "  " << algoName(r.algo) << " k=" << r.k << " p=" << r.p << " R=" << r.R
                          << " T=" << r.T << "\n";
                ++found;
            }
        if (!found) std::cout << "\nProblem decyzyjny: NIE (brak (B,p) z R<=" << R0 << " i T<=" << T0 << ")\n";
        else std::cout << "Problem decyzyjny: TAK (" << found << " rozwiazan)\n";
    }

    // Problem optymalizacyjny: wazona suma znormalizowanych R i T
    double Rmin = 1e300, Tmin = 1e300;
    for (auto& r : rows) { Rmin = std::min(Rmin, r.R); Tmin = std::min(Tmin, r.T); }
    const Row* best = nullptr;
    double bestScore = 1e300;
    for (auto& r : rows) {
        if (!r.ok) continue;
        double s = w * r.R / Rmin + (1 - w) * r.T / Tmin;
        if (s < bestScore) { bestScore = s; best = &r; }
    }
    if (best)
        std::cout << "\nNajlepszy kompromis (waga R=" << w << ", T=" << (1 - w) << "): "
                  << algoName(best->algo) << " B=2^" << best->k << " p=" << best->p
                  << " R=" << best->R << " T=" << best->T << " ms\n";
}

// ============================================================================
//  Autotest: przypadki brzegowe
// ============================================================================
static int cmdSelfTest() {
    std::mt19937 rng(12345);
    auto rnd = [&](size_t n) { Bytes b(n); for (auto& x : b) x = static_cast<uint8_t>(rng()); return b; };
    struct Case { std::string name; Bytes data; };
    std::vector<Case> cases;
    cases.push_back({"pusty", {}});
    cases.push_back({"1 bajt", Bytes(1, 7)});
    cases.push_back({"mniejszy niz blok (100 B)", rnd(100)});
    cases.push_back({"dokladnie 1 blok (1024)", rnd(1024)});
    cases.push_back({"niepelny ostatni blok", rnd(1024 * 5 + 321)});
    cases.push_back({"jeden powtarzany bajt", Bytes(300000, 'A')});
    cases.push_back({"jeden bajt, dlugosc 255/256/511", Bytes(255 + 256 + 511, 0)});
    cases.push_back({"dane losowe", rnd(500000)});
    {
        Bytes t;
        const std::string s = "Ala ma kota, a kot ma Ale. Kompresja Huffmana i RLE. aaaaabbbbbcccc ";
        while (t.size() < 400000) t.insert(t.end(), s.begin(), s.end());
        cases.push_back({"tekst powtarzalny", t});
    }
    {   // rozklad zblizony do Fibonacciego -> dlugie kody
        Bytes t;
        uint64_t a = 1, b = 1;
        for (int s = 0; s < 30; ++s) { t.insert(t.end(), static_cast<size_t>(a), static_cast<uint8_t>(s)); uint64_t c = a + b; a = b; b = c; }
        std::shuffle(t.begin(), t.end(), rng);
        cases.push_back({"rozklad Fibonacciego", t});
    }

    int fails = 0;
    for (auto& c : cases) {
        for (Algo algo : {Algo::RLE, Algo::HUFF}) {
            for (int k : {10, 12, 16}) {
                Bytes ref;
                bool first = true, ok = true;
                for (int p : {1, 2, 4, 8}) {
                    try {
                        auto blocks = compressBuffer(c.data, algo, 1u << k, p);
                        Bytes ser = serialize(blocks, algo, c.data.size(), 1u << k);
                        Bytes back = decompressBuffer(ser, p);
                        if (back != c.data) ok = false;
                        if (first) { ref = ser; first = false; }
                        else if (ser != ref) ok = false;  // rownolegle == sekwencyjne
                    } catch (const std::exception& e) {
                        std::cerr << "  wyjatek: " << e.what() << "\n";
                        ok = false;
                    }
                }
                if (!ok) {
                    ++fails;
                    std::cout << "BLAD: " << c.name << ", " << algoName(algo) << ", k=" << k << "\n";
                }
            }
        }
        std::cout << "ok: " << c.name << " (" << c.data.size() << " B)\n";
    }
    std::cout << (fails ? "TESTY NIEZALICZONE\n" : "Wszystkie testy zaliczone\n");
    return fails ? 1 : 0;
}

// ============================================================================
//  main / parsowanie argumentow
// ============================================================================
static std::string opt(const std::vector<std::string>& a, const std::string& name, const std::string& def) {
    for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == name) return a[i + 1];
    return def;
}

static void usage() {
    std::cout <<
        "Uzycie:\n"
        "  zssk compress   <in> <out> [--algo rle|huff] [--block K(10..26)] [--threads P]\n"
        "  zssk decompress <in> <out> [--threads P]\n"
        "  zssk bench      <plik> [--algo rle|huff|both] [--threads 1,2,4,8] [--bmin 10] [--bmax 26]\n"
        "                  [--reps 3] [--csv out.csv] [--memlimit MB] [--R0 x --T0 ms] [--w 0.5]\n"
        "  zssk selftest\n";
}

int main(int argc, char** argv) {
    try {
        std::vector<std::string> a(argv + 1, argv + argc);
        if (a.empty()) { usage(); return 1; }
        const std::string cmd = a[0];
        unsigned hw = std::max(1u, std::thread::hardware_concurrency());

        if (cmd == "selftest") return cmdSelfTest();
        if (cmd == "compress" && a.size() >= 3) {
            cmdCompress(a[1], a[2], parseAlgo(opt(a, "--algo", "huff")), std::stoi(opt(a, "--block", "16")),
                        std::stoi(opt(a, "--threads", std::to_string(hw))));
            return 0;
        }
        if (cmd == "decompress" && a.size() >= 3) {
            cmdDecompress(a[1], a[2], std::stoi(opt(a, "--threads", std::to_string(hw))));
            return 0;
        }
        if (cmd == "bench" && a.size() >= 2) {
            cmdBench(a[1], opt(a, "--algo", "both"), parseIntList(opt(a, "--threads", "1,2,4,8")),
                     std::stoi(opt(a, "--bmin", "10")), std::stoi(opt(a, "--bmax", "26")),
                     std::stoi(opt(a, "--reps", "3")), opt(a, "--csv", ""),
                     std::stod(opt(a, "--memlimit", "0")), std::stod(opt(a, "--R0", "0")),
                     std::stod(opt(a, "--T0", "0")), std::stod(opt(a, "--w", "0.5")));
            return 0;
        }
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Blad: " << e.what() << "\n";
        return 2;
    }
}