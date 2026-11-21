#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zssk {

using Bytes = std::vector<uint8_t>;

enum class Algo : uint8_t { RLE = 0, HUFF = 1 };

const char* algoName(Algo algo);
Algo parseAlgo(const std::string& value);
Bytes compressBlock(const Bytes& block, Algo algo);
void decompressBlock(const uint8_t* input, size_t inputSize, uint8_t* output, size_t outputSize, Algo algo);

} // namespace zssk
