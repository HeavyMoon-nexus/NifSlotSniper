#include "OSP_Logic.h"
#include "Globals.h"         // AddLog, g_OspFiles など
#include "SlotDictionary.hpp" // SlotDictionary::GetSlotName など (もし使っていれば)
#include "tinyxml2.h "      // ★XML解析に必須
#include <NifFile.hpp>       // ★Nifファイル操作に必須
#include <iostream>
#include <algorithm>         // replace, transform
#include <mutex>

// 名前空間の省略 (必要に応じて)
using namespace tinyxml2;
extern void SaveSessionChangesToFile();
// =========================================================================
// ヘルパー関数
// =========================================================================

std::string NormalizePath(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// =========================================================================
// 解析ロジック (ParseOSPFile)
// =========================================================================

void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets) {
    XMLDocument doc;
    if (doc.LoadFile(ospPath.string().c_str()) != XML_SUCCESS) return;

    XMLElement* root = doc.FirstChildElement("SliderSetInfo");
    if (!root) return;

    // ShapeDataフォルダの基準パス
    fs::path shapeDataBase = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "ShapeData";
    // フォールバック
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

// =========================================================================
// ワーカー実装 (ScanOSPWorker)
// =========================================================================

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
                g_CurrentProcessItem = "Parsing: " + path.filename().string();
            }

            OSPFile osp;
            osp.filename = path.filename().string();
            osp.fullPath = path.string();

            ParseOSPFile(path, osp.sets);

            if (!osp.sets.empty()) {
                g_OspFiles[osp.filename] = osp;
            }

            count++;
            g_Progress = (float)count / (float)(total > 0 ? total : 1);
        }

        if (!g_CancelRequested) {
            AddLog("Found " + std::to_string(g_OspFiles.size()) + " OSP files.", LogType::Success);
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

// =========================================================================
// ワーカー実装 (ExportOSPWorker)
// =========================================================================

// OSP Source NIF エクスポート用ワーカー (スロット適用版)
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

    // 処理対象をカウント
    for (const auto& [name, osp] : g_OspFiles) {
        for (const auto& set : osp.sets) {
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

        for (const auto& set : osp.sets) {
            if (!set.selected) continue;
            if (g_CancelRequested) break;

            processed++;
            g_Progress = (float)processed / (float)total;

            {
                std::lock_guard<std::mutex> lock(g_ProgressMutex);
                g_CurrentProcessItem = "Exporting: " + set.setName;
            }

            // 入力パス
            fs::path inPath(set.sourceNifPath);
            // ★★★ 修正: 保存先パスの決定ロジック (OSP版) ★★★
            // デフォルト出力先 (OutputRootがない場合)
            // ShapeData内のソースを直接上書きするのは危険なので、OutputRoot未設定時は
            // 「ShapeDataと同じ場所」つまり上書きになりますが、ここではパス生成のみ行います
            fs::path outPath = inPath;

            if (strlen(g_OutputRootPath) > 0) {
                std::string fullInStr = inPath.string();
                std::string lowerIn = fullInStr;
                std::transform(lowerIn.begin(), lowerIn.end(), lowerIn.begin(), ::tolower);

                // パスの中に "shapedata" が含まれているか探す
                // 通常 OSP の SourceFile は .../ShapeData/<DataFolder>/<SourceFile> にある
                size_t shapeDataPos = lowerIn.find("shapedata");
                std::string relPath;

                if (shapeDataPos != std::string::npos) {
                    // "shapedata" の文字数(9) + 区切り文字分を進める
                    size_t startPos = shapeDataPos + 9;
                    // 区切り文字 (\ や /) をスキップ
                    while (startPos < fullInStr.length() && (fullInStr[startPos] == '\\' || fullInStr[startPos] == '/')) {
                        startPos++;
                    }
                    // ここで relPath は "<DataFolder>/<SourceFile>" になる
                    relPath = fullInStr.substr(startPos);
                }
                else {
                    // 万が一 ShapeData フォルダ外ならファイル名だけ使う
                    relPath = inPath.filename().string();
                }

                // 結合: OutputRoot + CalienteTools/BodySlide/ShapeData + DataFolder/File
                fs::path outDir = fs::path(g_OutputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData";
                outPath = outDir / relPath;

                // フォルダ作成
                if (outPath.has_parent_path()) {
                    fs::create_directories(outPath.parent_path());
                }
            }
            if (fs::exists(inPath)) {
                try {
                    fs::create_directories(outPath.parent_path());

                    // NIFとしてロードしてスロット適用を試みる
                    nifly::NifFile nif;
                    if (nif.Load(inPath.string()) == 0) {

                        // DBマッチング (OutputPath + OutputName)
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

                        // スロット適用
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
                                std::lock_guard<std::mutex> lock(g_DataMutex); // データ保護

                                bool foundInSession = false;

                                // マップは "Key" と "Value(SlotRecord)" のペアを持っています
                                // change.first がキー、change.second が中身(SlotRecord)です
                                for (auto& change : g_SessionChanges) {
                                    // sourceFile などのメンバには .second を通してアクセスします
                                    if (change.second.sourceFile == targetRec->sourceFile &&
                                        change.second.armaFormID == targetRec->armaFormID) {

                                        change.second = *targetRec; // 中身を最新の状態で上書き
                                        foundInSession = true;
                                        break;
                                    }
                                }

                                if (!foundInSession) {
                                    // マップには push_back がないので、キーを指定して代入します
                                    // キーを一意にするため "ESP名_FormID" のような形式で作ります
                                    std::string newKey = targetRec->sourceFile + "_" + targetRec->armaFormID;

                                    // 万が一キーが被っていたら連番をつけるなどの安全策（簡易実装）
                                    if (g_SessionChanges.count(newKey) > 0) {
                                        newKey += "_OSP";
                                    }

                                    g_SessionChanges[newKey] = *targetRec;
                                }
                            }
                        }

                        // 保存
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
void ScanBodySlideWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;

    {
        std::lock_guard<std::mutex> lock(g_ProgressMutex);
        g_CurrentProcessItem = "Initializing Scan...";
    }

    // ローカルで結果を構築してから最後にグローバルへ書き戻す手法をとる
    std::map<std::string, std::vector<std::string>> localSourceMap;

    if (strlen(g_GameDataPath) == 0 && strlen(g_InputRootPath) == 0) {
        AddLog("Game Data Path / Input Root is not set.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    fs::path shapeDataPath = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "ShapeData";

    // フォールバック検索
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
        // ファイル数を概算するのは重いので、スキャンしながら不確定バーのような挙動にするか
        // または単純にファイル名を表示し続ける
        int count = 0;

        for (const auto& entry : fs::recursive_directory_iterator(shapeDataPath)) {
            // キャンセルチェック
            if (g_CancelRequested) {
                AddLog("Scan Cancelled by user.", LogType::Warning);
                break;
            }

            try {
                if (entry.is_regular_file() && entry.path().extension() == ".nif") {

                    // 進捗表示の更新（頻繁すぎると重いので適度に間引くか、軽量化する）
                    count++;
                    if (count % 10 == 0) { // 10ファイルに1回更新
                        std::lock_guard<std::mutex> lock(g_ProgressMutex);
                        g_CurrentProcessItem = "Found: " + entry.path().filename().string();
                        // スキャンの総数が不明なため、進捗バーは 0.0-1.0 をループさせるか、0.5固定などにする
                        g_Progress = (count % 100) / 100.0f;
                    }

                    // パス処理ロジック
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
            // 最後にグローバル変数へ反映 (排他制御が必要だが、処理中はUIがロックされるため代入のみでOK)
            g_BodySlideSourceMap = localSourceMap;
            g_BodySlideScanned = true;
            AddLog("Scanned BodySlide. Found " + std::to_string(g_BodySlideSourceMap.size()) + " directory groups.", LogType::Success);
        }
    }
    catch (const std::exception& ex) {
        AddLog(std::string("Scan Error: ") + ex.what(), LogType::Error);
    }

    g_IsProcessing = false; // 完了
}

// SliderSetsフォルダをスキャンする関数
void ScanBodySlideOSPs() {
    g_OspFiles.clear();
    g_SelectedOspName = "";

    if (strlen(g_GameDataPath) == 0) return;

    // 検索パス: CalienteTools/BodySlide/SliderSets
    fs::path sliderSetsPath = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "SliderSets";

    // フォールバック
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

                ParseOSPFile(entry.path(), osp.sets);

                if (!osp.sets.empty()) {
                    g_OspFiles[osp.filename] = osp;
                }
            }
        }
        AddLog("Found " + std::to_string(g_OspFiles.size()) + " OSP files.", LogType::Success);
    }
    catch (...) {
        AddLog("Error scanning OSP files.", LogType::Error);
    }
}