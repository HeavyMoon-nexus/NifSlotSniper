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

std::string NormalizePath(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// ParseOSPFile の実装（変更なし：必要時に呼び出す）
void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets) {
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
    std::lock_guard<std::mutex> lock(g_DataMutex);
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

    {
        std::lock_guard<std::mutex> lock(g_ProgressMutex);
        g_CurrentProcessItem = "Searching for SliderSets folder...";
    }

    g_OspFiles.clear();
    g_SelectedOspName = "";

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

        for (const auto& path : ospPaths) {
            if (g_CancelRequested) break;
            {
                std::lock_guard<std::mutex> lock(g_ProgressMutex);
                g_CurrentProcessItem = "Found: " + path.filename().string();
            }

            OSPFile osp;
            osp.filename = path.filename().string();
            osp.fullPath = path.string();
            // 重要: ここでは ParseOSPFile を呼ばない（遅延読み込み）
            // osp.sets は空のまま登録する

            {
                std::lock_guard<std::mutex> dlock(g_DataMutex);
                g_OspFiles[osp.filename] = osp;
            }

            count++;
            g_Progress = (float)count / (float)(total > 0 ? total : 1);
        }

        if (!g_CancelRequested) {
            AddLog("Found " + std::to_string(g_OspFiles.size()) + " OSP files (registered, details lazy-loaded).", LogType::Success);
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

// ExportOSPWorker: 既存ロジックだが、必要なら LoadOSPDetails を使って遅延ロードを行う
void ExportOSPWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;

    if (strlen(g_OutputRootPath) == 0) {
        AddLog("Output Root is not set!", LogType::Error);
        g_IsProcessing = false;
        return;
    }

    int total = 0;
    int success = 0;
    int failed = 0;
    int modified = 0;

    for (const auto& [name, osp] : g_OspFiles) {
        // sets は遅延ロードされている可能性があるため、チェック時にロードする
        // 名前だけ見て選択済みをカウントする実装のままにするため、ここではセットをロードしてからカウント
        LoadOSPDetails(name);
        for (const auto& set : g_OspFiles[name].sets) {
            if (set.selected) total++;
        }
    }

    if (total == 0) {
        AddLog("No sets selected for export.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    int processed = 0;

    for (auto& [name, osp] : g_OspFiles) {
        if (g_CancelRequested) break;

        // 必要なときに詳細をロード
        if (osp.sets.empty()) LoadOSPDetails(name);

        for (const auto& set : osp.sets) {
            if (!set.selected) continue;
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
                std::transform(lowerIn.begin(), lowerIn.end(), lowerIn.begin(), ::tolower);

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
                outPath = outDir / relPath;

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
                    if (nif.Load(inPath.string()) == 0) {
                        fs::path rawOutPath = fs::path(set.outputPath) / (set.outputName + "_1.nif");
                        std::string dbKey = rawOutPath.string();
                        std::string lowerKey = dbKey;
                        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

                        if (lowerKey.find("meshes\\") == 0) dbKey = dbKey.substr(7);
                        else if (lowerKey.find("meshes/") == 0) dbKey = dbKey.substr(7);

                        SlotRecord* targetRec = nullptr;
                        for (auto& r : g_AllRecords) {
                            if (_stricmp(r.femalePath.c_str(), dbKey.c_str()) == 0) {
                                targetRec = &r;
                                break;
                            }
                        }

                        if (targetRec) {
                            std::vector<int> newSlots = ParseSlotString(targetRec->armaSlots);
                            if (!newSlots.empty()) {
                                auto shapes = nif.GetShapes();
                                for (auto s : shapes) {
                                    if (auto bs = dynamic_cast<nifly::BSTriShape*>(s)) {
                                        auto skinRef = bs->SkinInstanceRef();
                                        if (!skinRef->IsEmpty()) {
                                            auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
                                            if (auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin)) {
                                                for (size_t i = 0; i < dis->partitions.size() && i < newSlots.size(); ++i) {
                                                    dis->partitions[i].partID = (uint16_t)newSlots[i];
                                                }
                                            }
                                        }
                                    }
                                }
                                modified++;
                            }
                            {
                                std::lock_guard<std::mutex> lock(g_DataMutex);
                                bool foundInSession = false;
                                for (auto& change : g_SessionChanges) {
                                    if (change.second.sourceFile == targetRec->sourceFile &&
                                        change.second.armaFormID == targetRec->armaFormID) {
                                        change.second = *targetRec;
                                        foundInSession = true;
                                        break;
                                    }
                                }
                                if (!foundInSession) {
                                    std::string newKey = targetRec->sourceFile + "_" + targetRec->armaFormID;
                                    if (g_SessionChanges.count(newKey) > 0) newKey += "_OSP";
                                    g_SessionChanges[newKey] = *targetRec;
                                }
                            }
                        }

                        nif.Save(outPath.string());
                        success++;
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

// ScanBodySlideWorker / ScanBodySlideOSPs は既存のまま（ScanBodySlideOSPs はファイル登録のみにする）
void ScanBodySlideWorker() {
    // 既存実装（変更なし）
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;

    {
        std::lock_guard<std::mutex> lock(g_ProgressMutex);
        g_CurrentProcessItem = "Initializing Scan...";
    }

    std::map<std::string, std::vector<std::string>> localSourceMap;

    if (strlen(g_GameDataPath) == 0 && strlen(g_InputRootPath) == 0) {
        AddLog("Game Data Path / Input Root is not set.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    fs::path shapeDataPath = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "ShapeData";

    if (!fs::exists(shapeDataPath) && strlen(g_InputRootPath) > 0) {
        fs::path altPath = fs::path(g_InputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData";
        if (fs::exists(altPath)) shapeDataPath = altPath;
        else {
            altPath = fs::path(g_InputRootPath).parent_path() / "CalienteTools" / "BodySlide" / "ShapeData";
            if (fs::exists(altPath)) shapeDataPath = altPath;
        }
    }

    if (!fs::exists(shapeDataPath)) {
        AddLog("ShapeData folder not found.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    AddLog("Scanning BodySlide at: " + shapeDataPath.string(), LogType::Info);

    try {
        int count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(shapeDataPath)) {
            if (g_CancelRequested) {
                AddLog("Scan Cancelled by user.", LogType::Warning);
                break;
            }

            try {
                if (entry.is_regular_file() && entry.path().extension() == ".nif") {

                    count++;
                    if (count % 10 == 0) {
                        std::lock_guard<std::mutex> lock(g_ProgressMutex);
                        g_CurrentProcessItem = "Found: " + entry.path().filename().string();
                        g_Progress = (count % 100) / 100.0f;
                    }

                    std::string fullPath = entry.path().string();
                    std::replace(fullPath.begin(), fullPath.end(), '\\', '/');
                    std::string lowerPath = fullPath;
                    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);

                    std::string keyword = "calientetools/bodyslide/shapedata/";
                    size_t pos = lowerPath.find(keyword);
                    std::string relativeStr = "";

                    if (pos != std::string::npos) relativeStr = fullPath.substr(pos + keyword.length());
                    else relativeStr = entry.path().filename().string();

                    size_t lastSlash = relativeStr.find_last_of('/');
                    std::string folderName = (lastSlash != std::string::npos) ? relativeStr.substr(0, lastSlash) : "(Root)";
                    std::string fileName = (lastSlash != std::string::npos) ? relativeStr.substr(lastSlash + 1) : relativeStr;

                    localSourceMap[folderName].push_back(fileName);
                }
            }
            catch (...) { continue; }
        }

        if (!g_CancelRequested) {
            g_BodySlideSourceMap = localSourceMap;
            g_BodySlideScanned = true;
            AddLog("Scanned BodySlide. Found " + std::to_string(g_BodySlideSourceMap.size()) + " directory groups.", LogType::Success);
        }
    }
    catch (const std::exception& ex) {
        AddLog(std::string("Scan Error: ") + ex.what(), LogType::Error);
    }

    g_IsProcessing = false;
}

// ScanBodySlideOSPs: ここも詳細は遅延ロードするように登録のみにする
void ScanBodySlideOSPs() {
    g_OspFiles.clear();
    g_SelectedOspName = "";

    if (strlen(g_GameDataPath) == 0) return;

    fs::path sliderSetsPath = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "SliderSets";

    if (!fs::exists(sliderSetsPath) && strlen(g_InputRootPath) > 0) {
        sliderSetsPath = fs::path(g_InputRootPath) / "CalienteTools" / "BodySlide" / "SliderSets";
    }

    if (!fs::exists(sliderSetsPath)) {
        AddLog("SliderSets folder not found.", LogType::Warning);
        return;
    }

    AddLog("Scanning OSP files in: " + sliderSetsPath.string(), LogType::Info);

    try {
        for (const auto& entry : fs::recursive_directory_iterator(sliderSetsPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".osp") {
                OSPFile osp;
                osp.filename = entry.path().filename().string();
                osp.fullPath = entry.path().string();
                // Parse は行わない（遅延ロード）
                std::lock_guard<std::mutex> lock(g_DataMutex);
                g_OspFiles[osp.filename] = osp;
            }
        }
        AddLog("Found " + std::to_string(g_OspFiles.size()) + " OSP files (registered).", LogType::Success);
    }
    catch (...) {
        AddLog("Error scanning OSP files.", LogType::Error);
    }
}