#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // ★SEH(__try/__except) 用
#endif

#include "OSP_Logic.h"
#include "Globals.h"
#include "SlotDictionary.hpp"
#include "tinyxml2.h"
#include <NifFile.hpp>
#include <iostream>
#include <algorithm>
#include <mutex>

// tinyxml2 名前空間等
using namespace tinyxml2;
extern void SaveSessionChangesToFile();

// ★バッチ堅牢化: nifly Load/Save を SEH でラップ（C++ try/catch では捕捉できないネイティブ例外で
//   OSP エクスポート全体が落ちるのを防ぎ、その NIF をスキップして継続する）。Main.cpp の
//   SafeNifLoad/SafeNifSave と同方針。__try フレームにはデストラクタを持つ C++ ローカルを置かない。
// ★B6: 破損 NIF の巨大確保（bad_alloc）対策。読込前にサイズ上限で弾く。
static const uintmax_t kOspMaxNifBytes = 512ull * 1024 * 1024;
#ifdef _WIN32
static int OspSafeNifLoadSEH(nifly::NifFile& nif, const fs::path& p) {
    __try { return nif.Load(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}
static bool OspSafeNifSaveSEH(nifly::NifFile& nif, const fs::path& p) {
    __try { nif.Save(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
static int OspSafeNifLoadSEH(nifly::NifFile& nif, const fs::path& p) { return nif.Load(p); }
static bool OspSafeNifSaveSEH(nifly::NifFile& nif, const fs::path& p) { nif.Save(p); return true; }
#endif
// ★B2: SEH に加え C++ 例外（bad_alloc 等）も捕捉する外側ラッパ（/EHsc では __except がすり抜ける）。
static int OspSafeNifLoad(nifly::NifFile& nif, const fs::path& p) {
    std::error_code ec;
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > kOspMaxNifBytes) {
        AddLog("OSP: NIF too large, refusing to load (" + std::to_string(sz) + " bytes): " + p.string(), LogType::Error);
        return -9998;
    }
    try { return OspSafeNifLoadSEH(nif, p); }
    catch (const std::exception& e) { AddLog(std::string("OSP NIF load exception: ") + e.what() + " (" + p.string() + ")", LogType::Error); return -9997; }
    catch (...) { AddLog("OSP NIF load unknown exception (" + p.string() + ")", LogType::Error); return -9996; }
}
static bool OspSafeNifSave(nifly::NifFile& nif, const fs::path& p) {
    try { return OspSafeNifSaveSEH(nif, p); }
    catch (const std::exception& e) { AddLog(std::string("OSP NIF save exception: ") + e.what() + " (" + p.string() + ")", LogType::Error); return false; }
    catch (...) { AddLog("OSP NIF save unknown exception (" + p.string() + ")", LogType::Error); return false; }
}

std::string NormalizePath(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// ParseOSPFile の実装（変更なし：必要時に呼び出す）
void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets) {
    // ★B6: 巨大 .osp（外部ファイル）による OOM / 深ネストでの再帰を一次防御。64MB 上限。
    if (!FileSizeOk(ospPath, 64ull * 1024 * 1024, "OSP XML")) return;
    XMLDocument doc;
    if (doc.LoadFile(ospPath.string().c_str()) != XML_SUCCESS) return;

    XMLElement* root = doc.FirstChildElement("SliderSetInfo");
    if (!root) return;

    fs::path shapeDataBase = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "ShapeData";
    if (!fs::exists(shapeDataBase) && strlen(g_InputRootPath) > 0) {
        shapeDataBase = fs::path(g_InputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData";
    }

    for (XMLElement* setElem = root->FirstChildElement("SliderSet"); setElem; setElem = setElem->NextSiblingElement("SliderSet")) {
        const char* nameAttr = setElem->Attribute("name");
        if (!nameAttr) continue;

        BodySlideSet setInfo;
        setInfo.setName = nameAttr;

        auto srcElem = setElem->FirstChildElement("SourceFile");
        auto dataFolderElem = setElem->FirstChildElement("DataFolder");
        auto outPathElem = setElem->FirstChildElement("OutputPath");
        auto outFileElem = setElem->FirstChildElement("OutputFile");

        if (srcElem && outPathElem && outFileElem) {
            std::string srcFile = srcElem->GetText() ? srcElem->GetText() : "";
            std::string dataFolder = "";
            if (dataFolderElem && dataFolderElem->GetText()) dataFolder = dataFolderElem->GetText();

            fs::path sourceFull;
            if (!dataFolder.empty()) sourceFull = shapeDataBase / dataFolder / srcFile;
            else sourceFull = shapeDataBase / srcFile;

            setInfo.sourceNifPath = sourceFull.string();
            setInfo.outputPath = outPathElem->GetText() ? outPathElem->GetText() : "";
            setInfo.outputName = outFileElem->GetText() ? outFileElem->GetText() : "";

            fs::path outBase = fs::path(g_GameDataPath) / setInfo.outputPath / setInfo.outputName;
            setInfo.fullOutputPath = outBase.string();

            outSets.push_back(setInfo);
        }
    }
}
fs::path FindFileInBodySlide(const std::string& filename) {
    // GameDataPath と InputRootPath の両方を考慮して ShapeData を探索します
    if (strlen(g_GameDataPath) == 0 && strlen(g_InputRootPath) == 0) return fs::path();

    fs::path searchRoot = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "ShapeData";
    if (!fs::exists(searchRoot) && strlen(g_InputRootPath) > 0) {
        fs::path try1 = fs::path(g_InputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData";
        if (fs::exists(try1)) searchRoot = try1;
        else {
            fs::path try2 = fs::path(g_InputRootPath).parent_path() / "CalienteTools" / "BodySlide" / "ShapeData";
            if (fs::exists(try2)) searchRoot = try2;
        }
    }

    if (!fs::exists(searchRoot)) return fs::path();

    for (const auto& entry : fs::recursive_directory_iterator(searchRoot)) {
        try {
            if (entry.is_regular_file()) {
                if (entry.path().filename().string() == filename) {
                    return entry.path();
                }
            }
        }
        catch (...) { continue; }
    }
    return fs::path();
}

// 新規: 遅延読み込み関数
void LoadOSPDetails(const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    auto it = g_OspFiles.find(filename);
    if (it == g_OspFiles.end()) return;

    OSPFile& osp = it->second;
    if (!osp.sets.empty()) return; // 既にロード済み

    // Parse を実行してセットを埋める
    std::vector<BodySlideSet> sets;
    ParseOSPFile(osp.fullPath, sets);
    osp.sets = std::move(sets);
}

// ScanOSPWorker: ファイルパスのみ登録する（遅延読み込み対応）
void ScanOSPWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;
    // ★A1: ワーカー全体を例外境界で保護（detach スレッドから例外が抜けると terminate）。
    struct Guard { ~Guard() { g_IsProcessing = false; } } _seGuard;
    try {

    {
        std::lock_guard<std::mutex> lock(g_ProgressMutex);
        g_CurrentProcessItem = "Searching for SliderSets folder...";
    }

    // ★#1: g_OspFiles をその場で clear/insert すると UI 反復と競合する。ローカルに構築して
    //   最後に g_DataMutex 下で一括 swap する（UI には常に一貫したスナップショットが見える）。
    {
        std::lock_guard<std::recursive_mutex> dlock(g_DataMutex);
        g_OspFiles.clear();
        g_SelectedOspName = "";
    }

    if (strlen(g_GameDataPath) == 0 && strlen(g_InputRootPath) == 0) {
        AddLog("Game Data Path / Input Root is not set.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    fs::path sliderSetsPath = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "SliderSets";
    if (!fs::exists(sliderSetsPath) && strlen(g_InputRootPath) > 0) {
        sliderSetsPath = fs::path(g_InputRootPath) / "CalienteTools" / "BodySlide" / "SliderSets";
        if (!fs::exists(sliderSetsPath)) {
            sliderSetsPath = fs::path(g_InputRootPath).parent_path() / "CalienteTools" / "BodySlide" / "SliderSets";
        }
    }

    if (!fs::exists(sliderSetsPath)) {
        AddLog("SliderSets folder not found.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    AddLog("Scanning OSP files in: " + sliderSetsPath.string(), LogType::Info);

    try {
        std::vector<fs::path> ospPaths;
        for (const auto& entry : fs::recursive_directory_iterator(sliderSetsPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".osp") {
                ospPaths.push_back(entry.path());
            }
        }

        int total = (int)ospPaths.size();
        int count = 0;

        std::map<std::string, OSPFile> localOsp; // ★ローカル構築（UI と非競合）
        for (const auto& path : ospPaths) {
            if (g_CancelRequested) break;
            {
                std::lock_guard<std::mutex> lock(g_ProgressMutex);
                g_CurrentProcessItem = "Found: " + path.filename().string();
            }

            OSPFile osp;
            osp.filename = path.filename().string();
            osp.fullPath = path.string();
            // 重要: ここでは ParseOSPFile を呼ばない（遅延読み込み）。osp.sets は空のまま登録。
            localOsp[osp.filename] = osp;

            count++;
            g_Progress = (float)count / (float)(total > 0 ? total : 1);
        }

        if (!g_CancelRequested) {
            // ★#1: 完成したマップを一括反映（途中状態を UI に見せない）。
            size_t n;
            { std::lock_guard<std::recursive_mutex> dlock(g_DataMutex); g_OspFiles.swap(localOsp); n = g_OspFiles.size(); }
            AddLog("Found " + std::to_string(n) + " OSP files (registered, details lazy-loaded).", LogType::Success);
        }
        else {
            AddLog("OSP Scan Cancelled.", LogType::Warning);
        }
    }
    catch (...) {
        AddLog("Error scanning OSP files.", LogType::Error);
    }

    g_IsProcessing = false;
    }
    catch (const std::exception& e) { AddLog(std::string("OSP scan worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("OSP scan worker unknown error.", LogType::Error); }
}

// ExportOSPWorker: 既存ロジックだが、必要なら LoadOSPDetails を使って遅延ロードを行う
void ExportOSPWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;
    // ★A1: ワーカー全体を例外境界で保護。
    struct Guard { ~Guard() { g_IsProcessing = false; } } _seGuard;
    try {

    if (strlen(g_OutputRootPath) == 0) {
        AddLog("Output Root is not set!", LogType::Error);
        g_IsProcessing = false;
        return;
    }

    int success = 0;
    int failed = 0;
    int modified = 0;

    // ★#1: 選択セットを g_DataMutex 下でスナップショット（遅延ロードも実施）。以降 g_OspFiles は触らず
    //   ローカルの selectedSets だけを処理する（重い NIF I/O 中に UI と競合しない）。
    std::vector<BodySlideSet> selectedSets;
    {
        // ★A4: まずロック下では「名前・fullPath・ロード済みか」のスナップショットと、ロード済み
        //   エントリの選択セット収集だけを行う（重い ParseOSPFile はロック外へ）。これにより同じ
        //   g_DataMutex を取る UI パネル描画が長時間待たされない。
        std::vector<std::pair<std::string, std::string>> toParse; // 未ロード（name, fullPath）
        {
            std::lock_guard<std::recursive_mutex> dlock(g_DataMutex);
            for (const auto& [name, osp] : g_OspFiles) {
                if (osp.sets.empty()) toParse.push_back({ name, osp.fullPath });
                else for (const auto& set : osp.sets) if (set.selected) selectedSets.push_back(set);
            }
        }
        // ★A4: 未ロード OSP の XML 解析はロック外で実行。結果をロック下でキャッシュへ反映し、
        //   選択分があれば収集する（通常は未ロード＝UI 未展開＝選択なしだが、整合性のため収集する）。
        for (auto& np : toParse) {
            std::vector<BodySlideSet> sets;
            ParseOSPFile(fs::path(np.second), sets);
            std::lock_guard<std::recursive_mutex> dlock(g_DataMutex);
            auto it = g_OspFiles.find(np.first);
            if (it != g_OspFiles.end() && it->second.sets.empty()) it->second.sets = sets;
            for (const auto& set : sets) if (set.selected) selectedSets.push_back(set);
        }
    }
    int total = (int)selectedSets.size();

    if (total == 0) {
        AddLog("No sets selected for export.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    int processed = 0;

    for (const auto& set : selectedSets) {
        if (g_CancelRequested) break;

        processed++;
        g_Progress = (float)processed / (float)total;

        {
            std::lock_guard<std::mutex> lock(g_ProgressMutex);
            g_CurrentProcessItem = "Exporting: " + set.setName;
        }

            fs::path inPath(set.sourceNifPath);
            fs::path outPath = inPath;

            if (strlen(g_OutputRootPath) > 0) {
                std::string fullInStr = inPath.string();
                std::string lowerIn = fullInStr;
                AsciiLowerInplace(lowerIn); // ★B5: "shapedata" 検索用（パス）

                size_t shapeDataPos = lowerIn.find("shapedata");
                std::string relPath;

                if (shapeDataPos != std::string::npos) {
                    size_t startPos = shapeDataPos + 9;
                    while (startPos < fullInStr.length() && (fullInStr[startPos] == '\\' || fullInStr[startPos] == '/')) {
                        startPos++;
                    }
                    relPath = fullInStr.substr(startPos);
                }
                else {
                    relPath = inPath.filename().string();
                }

                fs::path outDir = fs::path(g_OutputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData";
                outPath = (outDir / relPath).lexically_normal();

                // ★B3: relPath は .osp 由来（外部データ）。".." を含むと outDir 外へ書き出し得る。
                //   正規化後に outDir 配下へ収まるか検証し、外れるなら当該セットをスキップ。
                fs::path outDirN = outDir.lexically_normal();
                auto mm = std::mismatch(outDirN.begin(), outDirN.end(), outPath.begin(), outPath.end());
                if (mm.first != outDirN.end()) {
                    AddLog("OSP: refusing output outside root (traversal?): " + outPath.string(), LogType::Error);
                    failed++;
                    continue;
                }

                if (outPath.has_parent_path()) {
                    fs::create_directories(outPath.parent_path());
                }
            }
            if (fs::exists(inPath)) {
                try {
                    fs::create_directories(outPath.parent_path());

                    // もともとは nifly でロードして書き換えていたが、
                    // 新しい実装では LoadOSPDetails / ParseOSPFile により g_SessionChanges に反映され、
                    // ここでは元の処理を維持する（必要なら LoadOSPDetails で details を取得済み）
                    nifly::NifFile nif;
                    if (OspSafeNifLoad(nif, inPath) == 0) {
                        fs::path rawOutPath = fs::path(set.outputPath) / (set.outputName + "_1.nif");
                        std::string dbKey = rawOutPath.string();
                        std::string lowerKey = dbKey;
                        AsciiLowerInplace(lowerKey); // ★B5: "meshes" 前置判定用（パス）

                        if (lowerKey.find("meshes\\") == 0) dbKey = dbKey.substr(7);
                        else if (lowerKey.find("meshes/") == 0) dbKey = dbKey.substr(7);

                        // ★#1: g_AllRecords / g_SessionChanges アクセスをロックで保護し、対象レコードを
                        //   ローカルへコピーしてから（ロック外で）NIF に適用する（重い NIF 処理中はロックを持たない）。
                        SlotRecord recCopy; bool haveRec = false;
                        {
                            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                            for (auto& r : g_AllRecords) {
                                if (_stricmp(r.femalePath.c_str(), dbKey.c_str()) == 0) { recCopy = r; haveRec = true; break; }
                            }
                            if (haveRec) {
                                bool foundInSession = false;
                                for (auto& change : g_SessionChanges) {
                                    if (change.second.sourceFile == recCopy.sourceFile &&
                                        change.second.armaFormID == recCopy.armaFormID) {
                                        change.second = recCopy; foundInSession = true; break;
                                    }
                                }
                                if (!foundInSession) {
                                    std::string newKey = recCopy.sourceFile + "_" + recCopy.armaFormID;
                                    if (g_SessionChanges.count(newKey) > 0) newKey += "_OSP";
                                    g_SessionChanges[newKey] = recCopy;
                                }
                            }
                        }

                        // ★#4: per-mesh / union 適用は共有関数（SEH 保護・per-mesh / NiSkin↔BSD 対応）に統一。
                        //   旧コードは union を位置適用するだけで per-mesh を無視し、SEH 非保護だった。
                        bool osMismatch = false;
                        if (haveRec) {
                            if (ApplyPerMeshSlotsToNif(nif, recCopy, osMismatch)) modified++;
                        }
                        if (osMismatch) {
                            AddLog("OSP SLOT MISMATCH (not written, manual check): " + set.setName, LogType::Error);
                            failed++;
                        }
                        else if (OspSafeNifSave(nif, outPath)) {
                            success++;
                        }
                        else {
                            AddLog("Crash/exception during OSP NIF save (skipped): " + outPath.string(), LogType::Error);
                            failed++;
                        }
                    }
                    else {
                        AddLog("Load Error: " + set.setName, LogType::Error);
                        failed++;
                    }
                }
                catch (const std::exception& e) {
                    AddLog("Export Exception: " + std::string(e.what()), LogType::Error);
                    failed++;
                }
            }
            else {
                AddLog("Source missing: " + inPath.string(), LogType::Error);
                failed++;
            }
    }

    if (!g_CancelRequested) {
        std::string msg = "OSP Export: " + std::to_string(success) + " files. (" + std::to_string(modified) + " slot-modified)";
        AddLog(msg, (failed == 0 ? LogType::Success : LogType::Warning));
        AddLog("Auto-saving to slotdata-output.txt...", LogType::Info);
        SaveSessionChangesToFile();
    }
    else {
        AddLog("Export Cancelled.", LogType::Warning);
    }

    g_IsProcessing = false;
    }
    catch (const std::exception& e) { AddLog(std::string("OSP export worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("OSP export worker unknown error.", LogType::Error); }
}
