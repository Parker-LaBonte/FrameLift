#pragma once
#include "common.h"

bool LoadPngBGRA(const std::wstring& path, int& w, int& h, std::vector<uint8_t>& out);
int RunBench(const std::wstring& clipsDir, const std::wstring& configsPath, const std::wstring& outPath, int stride = 2);
