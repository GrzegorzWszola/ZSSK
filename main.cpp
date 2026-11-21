#include "zssk/commands.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace zssk;

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