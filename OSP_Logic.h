#pragma once
#include "Globals.h" // 型定義 (BodySlideSet, OSPFile) を利用

// プロトタイプ
std::string NormalizePath(const std::string& path);

// OSP 解析（実際のパース処理）
void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets);

// スキャン / エクスポート ワーカー群
void ScanOSPWorker();
void ExportOSPWorker();
void ScanBodySlideWorker();
void ScanBodySlideOSPs();

// 新規: 遅延読み込み用 API
void LoadOSPDetails(const std::string& filename);

