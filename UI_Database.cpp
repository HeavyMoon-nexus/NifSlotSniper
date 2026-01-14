#ifdef _WIN32
#define NOMINMAX
#undef APIENTRY
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib") //プロジェクト設定を触りたくない場合の簡易対応
#endif
#include "UI_Database.h"
#include "Globals.h"
#include "SlotDictionary.hpp"
#include <imgui.h>
#include <string>
#include <thread>
#include <algorithm> // transform用
#include <set>       // uniqueNifs用
#include "OSP_Logic.h"

// =========================================================================
// 外部関数の呼び出し準備
// (Globals.h で宣言されていますが、明示的に書くことでリンクエラーを防ぎます)
// =========================================================================
extern void LoadSlotDataFolder(const std::string& path);
extern void LoadNifFileCore(const std::string& path);
extern void SaveSessionChangesToFile();
extern void SaveUnifiedConfig();
extern fs::path ConstructSafePath(const std::string& root, const std::string& rel);

// ★ main.cpp にあるワーカー関数
extern void BatchExportWorker();
extern void ScanOSPWorker();
extern void ExportOSPWorker();

// 追加: 遅延読み込み API を UI 側から呼べるように extern 宣言→OSP_Logic.hで置き換え
//extern void LoadOSPDetails(const std::string& filename);

// =========================================================================
// NIF Database の実装 (UI描画のみ)
// =========================================================================
void RenderDatabase() {
    ImGui::SetNextWindowPos(ImVec2(370, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 650), ImGuiCond_FirstUseEver);

    // ウィンドウ開始
    if (ImGui::Begin("NIF Database")) {

        if (ImGui::BeginTabBar("DBTabs")) {

            // ============================================================
            // TAB 1: Slotdata- List
            // ============================================================
            ImGuiTabItemFlags listFlags = g_ForceTabToList ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Slotdata- List", nullptr, listFlags)) {

                if (g_ForceTabToList) g_ForceTabToList = false;
                else if (g_NifLoadMode == 3) { g_NifLoadMode = 0; }

                // --- 上部コントロール ---
                if (ImGui::Button("Reload TXT")) {
                    // ★修正: パスの解決ロジックを追加
                    fs::path p = g_SlotDataPath;
                    if (p.is_relative()) {
                        // GameDataPath があれば結合
                        if (strlen(g_GameDataPath) > 0) p = fs::path(g_GameDataPath) / p;
                        // なければ InputRootPath で試行
                        else if (strlen(g_InputRootPath) > 0) p = fs::path(g_InputRootPath) / p;
                    }
                    LoadSlotDataFolder(p.string());
                }
                ShowTooltip("Reloads all .txt files from the slotdata folder.");

                ImGui::SameLine();
                if (ImGui::Button("Launch Synthesis")) {
                    if (strlen(g_SynthesisPath) > 0) ShellExecuteA(NULL, "open", g_SynthesisPath, NULL, NULL, SW_SHOWNORMAL);
                }
                ShowTooltip("Launches the Synthesis.exe associated with this tool.");

                ImGui::Separator();
                g_SlotFilter.Draw("Filter");

                // --- 選択ボタン群 ---
                if (ImGui::Button("Select All")) {
                    for (const auto& r : g_AllRecords) {
                        bool bl = false;
                        for (const auto& b : g_SourceBlockedList) if (r.sourceFile == b) bl = true;
                        if (!bl) g_RecordSelectionMap[r.id] = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Deselect All")) {
                    for (auto& [id, b] : g_RecordSelectionMap) b = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Invert")) {
                    for (const auto& r : g_AllRecords) {
                        bool bl = false;
                        for (const auto& b : g_SourceBlockedList) if (r.sourceFile == b) bl = true;
                        if (!bl) g_RecordSelectionMap[r.id] = !g_RecordSelectionMap[r.id];
                    }
                }

                // --- リスト表示エリア ---
                float footerH = ImGui::GetFrameHeightWithSpacing() + 10;
                ImGui::BeginChild("RecList", ImVec2(0, -footerH), true);

                // ロード処理ラムダ
                auto LoadAction = [&](int id) {
                    SlotRecord* target = nullptr;
                    for (auto& r : g_AllRecords) if (r.id == id) { target = &r; break; }

                    if (target) {
                        g_SelectedRecordID = id;
                        std::string pathRel = "";
                        if (g_TargetGender == 0) { // Male
                            if (!target->malePath.empty()) pathRel = target->malePath;
                            else if (!target->femalePath.empty()) {
                                pathRel = target->femalePath;
                                AddLog("Male path missing. Loaded Female path instead.", LogType::Warning);
                            }
                        }
                        else { // Female
                            if (!target->femalePath.empty()) pathRel = target->femalePath;
                            else if (!target->malePath.empty()) {
                                pathRel = target->malePath;
                                AddLog("Female path missing. Loaded Male path instead.", LogType::Warning);
                            }
                        }

                        if (!pathRel.empty()) {
                            fs::path fullPath = ConstructSafePath(g_InputRootPath, pathRel);
                            if (fs::exists(fullPath)) LoadNifFileCore(fullPath.string());
                            else AddLog("File not found: " + fullPath.string(), LogType::Error);
                        }
                        else {
                            AddLog("No valid path found for the selected gender.", LogType::Error);
                        }

                        // Auto-fill logic
                        std::string eidLower = target->armoEditorID;
                        std::transform(eidLower.begin(), eidLower.end(), eidLower.begin(), ::tolower);
                        for (int i = 0; i < g_KeywordList.size(); ++i) {
                            bool matchFound = false;
                            if (!g_KeywordList[i].matchWords.empty()) {
                                for (const auto& w : g_KeywordList[i].matchWords) {
                                    std::string wLower = w;
                                    std::transform(wLower.begin(), wLower.end(), wLower.begin(), ::tolower);
                                    if (!wLower.empty() && eidLower.find(wLower) != std::string::npos) {
                                        matchFound = true; break;
                                    }
                                }
                            }
                            else {
                                std::string kwLower = g_KeywordList[i].keyword;
                                std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);
                                if (eidLower.find(kwLower) != std::string::npos) matchFound = true;
                            }
                            if (matchFound) { g_SelectedKeywordIndex = i; break; }
                        }
                    }
                    };

                // ツリー描画
                for (auto& [espName, genderMap] : g_DisplayTree) {
                    if (!g_SlotFilter.PassFilter(espName.c_str())) continue;

                    bool isBlocked = false;
                    for (const auto& bl : g_SourceBlockedList) if (espName == bl) isBlocked = true;
                    if (isBlocked) continue;

                    bool allInESP = true;
                    for (auto& [g, nm] : genderMap) for (auto& [n, ids] : nm) for (int id : ids) if (!g_RecordSelectionMap[id]) allInESP = false;
                    bool checkESP = allInESP;

                    if (ImGui::Checkbox(("##" + espName).c_str(), &checkESP)) {
                        for (auto& [g, nm] : genderMap) for (auto& [n, ids] : nm) for (int id : ids) g_RecordSelectionMap[id] = checkESP;
                    }
                    ImGui::SameLine();

                    bool openMenu = false;
                    bool nodeOpen = ImGui::TreeNode(espName.c_str());
                    if (ImGui::IsItemClicked(1)) openMenu = true;
                    if (openMenu) ImGui::OpenPopup(("ctx_" + espName).c_str());

                    if (ImGui::BeginPopup(("ctx_" + espName).c_str())) {
                        if (ImGui::MenuItem("Add ESP to BlockedList")) {
                            g_SourceBlockedList.push_back(espName);
                            SaveUnifiedConfig();
                            AddLog("Added to BlockedList: " + espName, LogType::Success);
                        }
                        ImGui::EndPopup();
                    }

                    if (nodeOpen) {
                        for (auto& [genderName, nifMap] : genderMap) {
                            bool allInGen = true;
                            for (auto& [n, ids] : nifMap) for (int id : ids) if (!g_RecordSelectionMap[id]) allInGen = false;
                            bool checkGen = allInGen;
                            if (ImGui::Checkbox(("##" + espName + genderName).c_str(), &checkGen)) {
                                for (auto& [n, ids] : nifMap) for (int id : ids) g_RecordSelectionMap[id] = checkGen;
                            }
                            ImGui::SameLine();
                            if (ImGui::TreeNode(genderName.c_str())) {
                                for (auto& [nifName, ids] : nifMap) {
                                    bool allInNif = true;
                                    for (int id : ids) if (!g_RecordSelectionMap[id]) allInNif = false;
                                    bool checkNif = allInNif;
                                    if (ImGui::Checkbox(("##" + nifName).c_str(), &checkNif)) {
                                        for (int id : ids) g_RecordSelectionMap[id] = checkNif;
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::TreeNode(nifName.c_str())) {
                                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !ids.empty()) LoadAction(ids[0]);
                                        for (int id : ids) {
                                            SlotRecord* r = nullptr;
                                            for (auto& rec : g_AllRecords) if (rec.id == id) r = &rec;
                                            if (!r) continue;
                                            bool selected = g_RecordSelectionMap[id];
                                            if (ImGui::Checkbox(("##id" + std::to_string(id)).c_str(), &selected)) {
                                                g_RecordSelectionMap[id] = selected;
                                            }
                                            ImGui::SameLine();
                                            if (ImGui::Selectable(r->displayText.c_str(), g_SelectedRecordID == id)) LoadAction(id);
                                        }
                                        ImGui::TreePop();
                                    }
                                }
                                ImGui::TreePop();
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::EndChild();

                // Export Button
                std::set<std::string> uniqueNifs;
                for (auto [id, selected] : g_RecordSelectionMap) {
                    if (selected) {
                        for (const auto& r : g_AllRecords) {
                            if (r.id == id) {
                                std::string p = (g_TargetGender == 0) ? r.malePath : r.femalePath;
                                if (p.empty()) p = (g_TargetGender == 0) ? r.femalePath : r.malePath;
                                if (!p.empty()) uniqueNifs.insert(p);
                                break;
                            }
                        }
                    }
                }

                std::string btnText = "Export Selected NIF (" + std::to_string(uniqueNifs.size()) + ")";
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.4f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.55f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.3f, 0.0f, 1.0f));

                // ★ main.cpp のワーカーを呼び出す
                if (ImGui::Button(btnText.c_str(), ImVec2(-1, 30))) {
                    if (!g_SessionChanges.empty()) {
                        AddLog("Auto-saving before export...", LogType::Info);
                        SaveSessionChangesToFile();
                    }
                    std::thread(BatchExportWorker).detach();
                }
                ImGui::PopStyleColor(3);
                ShowTooltip("Batch exports all NIFs checked in the list above.");

                ImGui::EndTabItem();
            }

            // ============================================================
            // TAB 2: Bodyslide OSP Browser
            // ============================================================
            ImGuiTabItemFlags srcFlags = g_ForceTabToSource ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Bodyslide OSP Browser", nullptr, srcFlags)) {
                if (g_ForceTabToSource) g_ForceTabToSource = false;
                else if (g_NifLoadMode != 3) { g_NifLoadMode = 3; }

                // Scan Button
                // ★ OSP_Logic.cpp のワーカーを呼び出す
                if (ImGui::Button("Scan OSP Files")) {
                    std::thread(ScanOSPWorker).detach();
                }
                ShowTooltip("Scans the 'SliderSets' folder for .osp files.");
                ImGui::SameLine();
                static ImGuiTextFilter ospFilter;
                ospFilter.Draw("Filter OSP");

                // Columns
                float footerH = ImGui::GetFrameHeightWithSpacing() + 10;
                ImGui::Columns(2, "OspCols", true);

                ImGui::BeginChild("OspList", ImVec2(0, -footerH), true);
                for (const auto& [name, ospData] : g_OspFiles) {
                    if (!ospFilter.PassFilter(name.c_str())) continue;
                    // ★追加 1: ブロックリストに含まれているかチェックして、あればスキップ
                    bool isBlocked = false;
                    for (const auto& bl : g_SourceBlockedList) {
                        if (name == bl) {
                            isBlocked = true;
                            break;
                        }
                    }
                    if (isBlocked) continue;

                    // リスト項目の描画
                    if (ImGui::Selectable(name.c_str(), g_SelectedOspName == name)) {
                        g_SelectedOspName = name;
                        // 遅延読み込み: ユーザーが選択した瞬間に詳細をロードする
                        LoadOSPDetails(name);
                    }

                    // ★追加 2: 右クリックメニュー (BlockedListへの追加)
                    if (ImGui::BeginPopupContextItem(("ctx_osp_" + name).c_str())) {
                        if (ImGui::MenuItem("Add to BlockedList")) {
                            g_SourceBlockedList.push_back(name);
                            SaveUnifiedConfig(); // 設定ファイルへ即時保存
                            AddLog("Added to BlockedList: " + name, LogType::Success);
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::EndChild();
                ImGui::NextColumn();

                ImGui::BeginChild("SetList", ImVec2(0, -footerH), true);
                if (!g_SelectedOspName.empty() && g_OspFiles.count(g_SelectedOspName)) {
                    if (ImGui::Button("Select All")) { for (auto& s : g_OspFiles[g_SelectedOspName].sets) s.selected = true; }
                    ImGui::SameLine();
                    if (ImGui::Button("Deselect All")) { for (auto& s : g_OspFiles[g_SelectedOspName].sets) s.selected = false; }
                    ImGui::Separator();
                    auto& sets = g_OspFiles[g_SelectedOspName].sets;
                    for (int i = 0; i < sets.size(); ++i) {
                        auto& setInfo = sets[i];
                        ImGui::PushID(i);
                        ImGui::Checkbox("##sel", &setInfo.selected);
                        ImGui::SameLine();
                        if (ImGui::Selectable(setInfo.setName.c_str())) LoadNifFileCore(setInfo.sourceNifPath);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Source: %s\nOutput: %s", fs::path(setInfo.sourceNifPath).filename().string().c_str(), setInfo.outputName.c_str());
                        ImGui::PopID();
                    }
                }
                else {
                    ImGui::TextDisabled("Select an OSP file from the left list.");
                }
                ImGui::EndChild();
                ImGui::Columns(1);

                // Export Button
                int selCount = 0;
                for (const auto& [n, o] : g_OspFiles) for (const auto& s : o.sets) if (s.selected) selCount++;
                std::string btnText = "Export Checked Sources (" + std::to_string(selCount) + ")";

                

                if (selCount > 0) {
                    // ★ OSP_Logic のワーカーを呼び出す
                    if (ImGui::Button(btnText.c_str(), ImVec2(-1, 30))) {
                        std::thread(ExportOSPWorker).detach();
                    }
                }
                else {
                    ImGui::BeginDisabled();
                    ImGui::Button(btnText.c_str(), ImVec2(-1, 30));
                    ImGui::EndDisabled();
                }

                
                ShowTooltip("Exports Checked source NIFs with slots applied from Database.");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

    } // End Begin
    ImGui::End();
}