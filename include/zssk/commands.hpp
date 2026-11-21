#pragma once

#include "zssk/codec.hpp"

#include <string>
#include <vector>

namespace zssk {

std::vector<int> parseIntList(const std::string& value);
void cmdCompress(const std::string& input, const std::string& output, Algo algo, int blockExponent, int threads);
void cmdDecompress(const std::string& input, const std::string& output, int threads);
void cmdBench(const std::string& path, const std::string& algo, const std::vector<int>& threads,
              int blockMin, int blockMax, int repetitions, const std::string& csvPath,
              double memoryLimitMB, double ratioLimit, double timeLimit, double weight);
int cmdSelfTest();

} // namespace zssk
