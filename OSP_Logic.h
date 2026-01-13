#pragma once
#include "Globals.h" // 構造体定義 (BodySlideSet, OSPFile) を参照するため

// ヘルパー関数
std::string NormalizePath(const std::string& path);

// OSPファイルの解析
void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets);

// ワーカー関数 (スレッドで実行される処理)
void ScanOSPWorker();
void ExportOSPWorker();
//void ExecuteSourceNifExport();
void ScanBodySlideWorker();
void ScanBodySlideOSPs();
void ParseOSPFile();
//void ExportBodySlideWorker();