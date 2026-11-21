#include "zssk/commands.hpp"

#include "zssk/archive.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zssk {

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
void cmdCompress(const std::string& in, const std::string& out, Algo algo, int k, int p) {
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

void cmdDecompress(const std::string& in, const std::string& out, int p) {
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

static std::string csvField(const std::string& value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') result += '"';
        result += c;
    }
    return result + '"';
}

std::vector<int> parseIntList(const std::string& s) {
    std::vector<int> v;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) v.push_back(std::stoi(tok));
    return v;
}

void cmdBench(const std::string& path, const std::string& algoArg, const std::vector<int>& threads,
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
                    csv << csvField(path) << "," << algoName(row.algo) << "," << N << "," << (1u << row.k) << "," << row.k
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
int cmdSelfTest() {
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

} // namespace zssk
