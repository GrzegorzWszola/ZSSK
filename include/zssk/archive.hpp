#pragma once

#include "zssk/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace zssk {

using BlockReader = std::function<void(size_t index, Bytes& block)>;

size_t nBlocksFor(uint64_t size, uint64_t blockSize);
size_t containerSize(const std::vector<Bytes>& blocks);
std::vector<Bytes> compressBlocks(const BlockReader& readBlock, size_t blockCount, Algo algo, int threads);
Bytes serialize(const std::vector<Bytes>& blocks, Algo algo, uint64_t originalSize, uint32_t blockSize);
Bytes decompressBuffer(const Bytes& container, int threads);
std::vector<Bytes> compressBuffer(const Bytes& data, Algo algo, uint32_t blockSize, int threads);

} // namespace zssk
