#include "zssk/codec.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace zssk {

const char* algoName(Algo a) { return a == Algo::RLE ? "rle" : "huff"; }

Algo parseAlgo(const std::string& s) {
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

Bytes compressBlock(const Bytes& b, Algo a) {
    return a == Algo::RLE ? rleCompress(b.data(), b.size()) : huffCompress(b.data(), b.size());
}

void decompressBlock(const uint8_t* in, size_t inN, uint8_t* out, size_t outN, Algo a) {
    if (a == Algo::RLE) rleDecompress(in, inN, out, outN);
    else huffDecompress(in, inN, out, outN);
}

// ============================================================================

} // namespace zssk
