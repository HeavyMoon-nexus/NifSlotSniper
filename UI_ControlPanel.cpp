#include "UI_ControlPanel.h"
#include "Globals.h"
#include "SlotDictionary.hpp"
#include "BoneAnalyzer.hpp"
#include <imgui.h>
#include <string>
#include <cstring> // strncpy_s, memset
#include <algorithm> // find
#include <thread>
#include <filesystem>
#include <NifFile.hpp>  // NifFile型用

namespace fs = std::filesystem;

// =========================================================================
// 外部関数・変数の宣言
// =========================================================================

// UIヘルパー
extern void ShowTooltip(const char* desc);
extern std::string OpenFileDialog(const char* filter);

// ワーカー・ロジック
extern void ScanBodySlideWorker(); // Sourceモード用スキャン
extern void UpdateMeshList();      // 引数なし版 (Main NIF用)
// ★重要: Ref Body読み込み用に引数付きのオーバーロードが必要になります。
// main.cpp にこの定義がない場合、エラーになります。その場合は main.cpp に追加するか、
// このファイルの末尾にダミー関数を作ってください。
extern void UpdateMeshList(nifly::NifFile& nif, std::vector<RenderMesh>& meshes, bool isRef);

extern void ApplySlotChanges(int meshIndex, const std::string& slotStr);
extern void SaveUnifiedConfig();
// 追加: セッション保存トリガー
extern void SaveSessionChangesToFile();

// 外部：スロット文字列パース
extern std::vector<int> ParseSlotString(const std::string& slotStr);

// グローバル変数 (Globals.h にないものがもしあれば追加してください)
extern bool g_ForceTabToList;
extern bool g_ForceTabToSource;
extern bool g_BodySlideScanned;
extern std::string g_StatusMessage;
extern fs::path g_RefNifPath;
extern nifly::NifFile g_RefNifData;
extern std::vector<RenderMesh> g_RefRenderMeshes;
extern bool g_ShowRef;

// =========================================================================
// コントロールパネルの実装
// =========================================================================
void RenderControlPanel() {
    // 表示フラグチェック
    if (!g_ShowControlPanel) return;

    // --- バックアップコードの設定 ---
    ImGui::SetNextWindowPos(ImVec2(10, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 650), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Control Panel")) {

        // ---------------------------------------------------------
        // 4. (New) Auto-Analyze Logic
        // ---------------------------------------------------------
        // NIFがロードされた（パスが変わった）直後に自動で解析を実行する
        static std::string lastAnalyzedNif = "";
        bool needAnalyze = false;

        if (!g_CurrentNifPath.empty() && g_CurrentNifPath.string() != lastAnalyzedNif) {
            // NIFが変わったので解析フラグを立てる
            needAnalyze = true;
            lastAnalyzedNif = g_CurrentNifPath.string();
        }

        if (needAnalyze) {
            std::string currentEid = "";
            for (const auto& r : g_AllRecords) if (r.id == g_SelectedRecordID) currentEid = r.armaEditorID;

            // 解析実行
            auto results = BoneAnalyzer::AnalyzeNif(g_NifData);
            for (const auto& res : results) {
                for (auto& m : g_RenderMeshes) {
                    if (m.name == res.meshName) {
                        // Block Check
                        bool isExcluded = false;
                        for (const auto& blockedWord : g_KeywordBlockedList) {
                            if (m.name.find(blockedWord) != std::string::npos) { isExcluded = true; break; }
                        }
                        if (isExcluded) {
                            m.suggestions.clear(); m.debugReasons.clear();
                            continue;
                        }
                        // Execute
                        m.suggestions = SlotDictionary::SuggestTopSlots(res.influentialBones, res.meshName, currentEid);
                        auto detailed = SlotDictionary::AnalyzeDetailed(res.influentialBones, res.meshName, currentEid);
                        m.suggestions = detailed.topSlots;
                        m.debugReasons = detailed.reasons;
                    }
                }
            }
            AddLog("Auto-Analysis Completed.", LogType::Info);
        }

        // ---------------------------------------------------------
        // 1. Settings (Gender, Mode)
        // ---------------------------------------------------------
        ImGui::Text("Target Gender:");
        if (ImGui::RadioButton("Male", g_TargetGender == 0)) g_TargetGender = 0; ImGui::SameLine();
        if (ImGui::RadioButton("Female", g_TargetGender == 1)) g_TargetGender = 1;

        ImGui::Separator();
        ImGui::Text("Load Mode:");
        if (ImGui::RadioButton("Both", &g_NifLoadMode, 0)) { g_ForceTabToList = true; }
        ShowTooltip("Reads the specified NIF and automatically looks for its pair (_0.nif <-> _1.nif).\nStandard mode for armor editing.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Single", &g_NifLoadMode, 1)) { g_ForceTabToList = true; }
        ShowTooltip("Reads only the specified file.\nUseful for accessories or props that don't have weight sliders.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Pair", &g_NifLoadMode, 2)) { g_ForceTabToList = true; }
        ShowTooltip("Similar to 'Both', but focuses on pair consistency checks.");

        // OSP Mode
        if (ImGui::RadioButton("BS OSP NIF", &g_NifLoadMode, 3)) {
            g_ForceTabToSource = true;
            // OSP用スキャンワーカーの宣言
            extern void ScanOSPWorker();

            if (g_OspFiles.empty() && !g_IsProcessing) {
                std::thread(ScanOSPWorker).detach();
            }
        }
        ShowTooltip("Directly edits .nif files located in BodySlide/ShapeData.\nUse this to fix slots in the source projects before building meshes.");

        if (g_NifLoadMode == 1) ImGui::TextDisabled("(Reads BodySlide ShapeData)");
        if (g_NifLoadMode == 3) ImGui::TextDisabled("(Direct Edit: ShapeData/*.nif)");

        ImGui::Separator();

        // ---------------------------------------------------------
        // 2. Status & Ref Body
        // ---------------------------------------------------------
        if (!g_CurrentNifPath.empty()) {
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "Editing:");
            ImGui::TextWrapped("%s", g_CurrentNifPath.filename().string().c_str());
        }
        else {
            ImGui::TextDisabled("Status: Waiting for selection...");
        }
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", g_StatusMessage.c_str());

        ImGui::Separator();

        if (ImGui::Button("Load Ref Body")) {
            if (strlen(g_InputRootPath) > 0) {
                fs::path refP = fs::path(g_InputRootPath) / "meshes" / "actors" / "character" / "character assets" / "femalebody_1.nif";
                if (fs::exists(refP)) {
                    g_RefNifPath = refP;
                    if (g_RefNifData.Load(refP.string()) == 0) {
                        UpdateMeshList(g_RefNifData, g_RefRenderMeshes, true);
                        AddLog("Ref Body Loaded: femalebody_1.nif");
                    }
                }
                else {
                    std::string s = OpenFileDialog("NIF\0*.nif\0");
                    if (!s.empty() && g_RefNifData.Load(s) == 0) {
                        UpdateMeshList(g_RefNifData, g_RefRenderMeshes, true);
                    }
                }
            }
        }
        ShowTooltip("Loads a reference body (e.g., femalebody_1.nif) to display as an overlay.\nUseful for checking clipping or positioning.");
        ImGui::SameLine();
        ImGui::Checkbox("Show Ref", &g_ShowRef);
        ShowTooltip("Toggle the visibility of the reference body.");

        ImGui::Separator();

        // ---------------------------------------------------------
        // 3. Mesh List (二段表示: 先頭に DB Slots 行、その下に mesh名 + NIF Slots)
        // ---------------------------------------------------------
        ImGui::Text("Mesh List:");
        ShowTooltip("DB slots are shown once at the top; below are meshes with their NIF slots.");

        // リスト高さ確保
        if (ImGui::BeginChild("MeshList", ImVec2(0, 150), true)) {

            // --- DB スロット行（選択レコードに基づく、一度だけ表示） ---
            const SlotRecord* selRec = nullptr;
            if (g_SelectedRecordID != -1) {
                for (const auto& r : g_AllRecords) { if (r.id == g_SelectedRecordID) { selRec = &r; break; } }
            }

            // 検出用キー
            auto makeKey = [](const SlotRecord* rec) {
                if (!rec) return std::string();
                return rec->sourceFile + "_" + rec->armaFormID;
                };

            bool showPendingForSelectedRec = false;
            if (selRec) {
                std::string dbRaw = selRec->armaSlots;
                std::string dbDisplay = dbRaw.empty() ? "(None)" : FormatSlotStringWithNames(dbRaw);

                // pending チェック： g_SessionChanges に登録済みか
                std::string recKey = makeKey(selRec);
                showPendingForSelectedRec = (g_SessionChanges.count(recKey) > 0);

                // DB行表示
                ImGui::Text("DB Slots: %s", dbDisplay.c_str());

                // 右に英語メッセージ（ボタン）を出す（未保存変更がある場合）
                if (showPendingForSelectedRec) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
                    if (ImGui::Button("Write slotdata-Output.txt")) {
                        // ボタン押下で即時保存を実行
                        SaveSessionChangesToFile();
                    }
                    ImGui::PopStyleColor();
                }

                // Before 表示：選択メッシュの before をここに出す (slotdata の下)
                if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
                    const auto& selMesh = g_RenderMeshes[g_SelectedMeshIndex];
                    if (!selMesh.beforeSlotInfo.empty()) {
                        ImGui::TextDisabled("Before: %s", selMesh.beforeSlotInfo.c_str());
                    }
                }
            }
            else {
                ImGui::TextDisabled("DB Slots: (no record selected)");
            }

            ImGui::Separator();

            // --- 各メッシュを表示：mesh名（Selectable） とその下に NIF Slots を単行で表示 ---
            for (int i = 0; i < g_RenderMeshes.size(); ++i) {
                auto& mesh = g_RenderMeshes[i];
                bool isSelected = (g_SelectedMeshIndex == i);

                // ブロック（非表示）判定
                bool isBlocked = false;
                for (const auto& bw : g_KeywordBlockedList) {
                    if (!bw.empty() && mesh.name.find(bw) != std::string::npos) { isBlocked = true; break; }
                }

                ImGui::PushID(i);
                // mesh name selectable (color)
                if (isBlocked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                else ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(mesh.color.r, mesh.color.g, mesh.color.b, 1.0f));

                if (ImGui::Selectable(mesh.name.c_str(), isSelected)) {
                    g_SelectedMeshIndex = i;
                }
                ImGui::PopStyleColor();

                // 右クリックでブロックリストへ
                if (ImGui::IsItemClicked(1)) {
                    if (std::find(g_KeywordBlockedList.begin(), g_KeywordBlockedList.end(), mesh.name) == g_KeywordBlockedList.end()) {
                        g_KeywordBlockedList.push_back(mesh.name);
                        SaveUnifiedConfig();
                        AddLog("Added to BlockedList: " + mesh.name, LogType::Success);
                    }
                }

                // NIF Slots を一行で表示
                {
                    std::string nifLine = "NIF Slots: ";
                    if (!mesh.slotInfo.empty()) nifLine += mesh.slotInfo;
                    else nifLine += "(None)";
                    ImGui::TextDisabled("%s", nifLine.c_str());

                    // mesh行の右にも同じ英語メッセージを表示（選択レコードに対して未保存変更がある場合）
                    if (selRec && showPendingForSelectedRec) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Write slotdata-Output.txt");
                    }
                }

                ImGui::PopID();
            }

            ImGui::EndChild();
        }

        ImGui::Separator();

        // ---------------------------------------------------------
        // 4. Analyze & Apply UI (選択中メッシュの詳細操作)
        // ---------------------------------------------------------
        if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
            // --- メッシュ選択時 ---
            auto& mesh = g_RenderMeshes[g_SelectedMeshIndex];
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Selected: %s", mesh.name.c_str());

            // Suggestions (選択中かつ提案がある場合のみ表示)
            if (!mesh.suggestions.empty()) {
                ImGui::Text("Suggestions:");
                if (ImGui::IsItemHovered()) ShowTooltip("Click to set.\nShift+Click to append (e.g., \"32, 50\").");

                float windowWidth = ImGui::GetContentRegionAvail().x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float btnWidth = (windowWidth - spacing) * 0.5f;

                for (size_t k = 0; k < mesh.suggestions.size(); ++k) {
                    int sID = mesh.suggestions[k].first;
                    std::string sName = SlotDictionary::GetSlotName(sID);
                    std::string btnLabel;
                    if (k == 0) btnLabel = "1ST: " + std::to_string(sID) + " (" + sName + ")";
                    else if (k == 1) btnLabel = "2ND: " + std::to_string(sID) + " (" + sName + ")";
                    else btnLabel = "3RD: " + std::to_string(sID) + " (" + sName + ")";

                    if (ImGui::Button(btnLabel.c_str(), ImVec2(btnWidth, 0))) {
                        std::string sStr = std::to_string(sID);
                        bool append = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
                        if (append && strlen(g_InputBuffer) > 0) {
                            std::string current = g_InputBuffer;
                            if (current.length() + sStr.length() + 2 < 1024) {
                                current += ", " + sStr;
                                strncpy_s(g_InputBuffer, current.c_str(), _TRUNCATE);
                            }
                        }
                        else {
                            strncpy_s(g_InputBuffer, sStr.c_str(), _TRUNCATE);
                        }
                    }
                    if (ImGui::IsItemHovered()) ShowTooltip("Click: Replace\nShift+Click: Append (, )");

                    if ((k % 2) == 0 && (k + 1) < mesh.suggestions.size()) {
                        ImGui::SameLine();
                    }
                }
            }
        }
        else {
            // --- 未選択時 ---
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Selected: (None)");
            ImGui::TextDisabled("(Select a mesh to see suggestions)");
        }

        ImGui::Separator();

        // ★変更: 入力欄とボタンは常に表示する

        ImGui::Text("Manual Input:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##Input")) {
            memset(g_InputBuffer, 0, sizeof(g_InputBuffer));
        }

        ImGui::InputText("##Manual", g_InputBuffer, sizeof(g_InputBuffer));
        if (ImGui::IsItemHovered()) ShowTooltip("Enter slot numbers separated by commas (e.g., \"32, 50\").\nThese will be applied to partitions in order (P0, P1...).");

        // Apply Button 1: Apply Change (This Mesh)
        // 未選択時はグレーアウトするか、押しても警告ログを出す
        if (ImGui::Button("Apply Change (This Mesh)", ImVec2(-1, 30))) {
            if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
                if (strlen(g_InputBuffer) > 0) {
                    ApplySlotChanges(g_SelectedMeshIndex, g_InputBuffer);
                }
            }
            else {
                AddLog("No mesh selected. Cannot apply to 'This Mesh'.", LogType::Warning);
            }
        }

        // Apply Button 2: Apply to ALL Listed Meshes (常に有効)
        if (ImGui::Button("Apply to ALL Listed Meshes", ImVec2(-1, 30))) {
            if (strlen(g_InputBuffer) > 0) {
                std::string targetSlots = g_InputBuffer;
                int applyCount = 0;
                for (int k = 0; k < g_RenderMeshes.size(); ++k) {
                    ApplySlotChanges(k, targetSlots);
                    applyCount++;
                }
                AddLog("Applied [" + targetSlots + "] to " + std::to_string(applyCount) + " meshes.", LogType::Success);
            }
            else {
                AddLog("Input is empty. 'Apply to ALL' cancelled.", LogType::Warning);
            }
        }

        ImGui::Separator();

        // ---------------------------------------------------------
        // 5. Global Analysis
        // ---------------------------------------------------------
        ImGui::Text("Analysis:");
        if (ImGui::Button("Analyze Slots (Manual)", ImVec2(-1, 30))) {
            if (!g_CurrentNifPath.empty()) {
                std::string currentEid = "";
                for (const auto& r : g_AllRecords) if (r.id == g_SelectedRecordID) currentEid = r.armaEditorID;

                auto results = BoneAnalyzer::AnalyzeNif(g_NifData);
                for (const auto& res : results) {
                    for (auto& m : g_RenderMeshes) {
                        if (m.name == res.meshName) {
                            bool isExcluded = false;
                            for (const auto& blockedWord : g_KeywordBlockedList) {
                                if (m.name.find(blockedWord) != std::string::npos) { isExcluded = true; break; }
                            }
                            if (isExcluded) {
                                m.suggestions.clear(); m.debugReasons.clear();
                                continue;
                            }
                            m.suggestions = SlotDictionary::SuggestTopSlots(res.influentialBones, res.meshName, currentEid);
                            auto detailed = SlotDictionary::AnalyzeDetailed(res.influentialBones, res.meshName, currentEid);
                            m.suggestions = detailed.topSlots;
                            m.debugReasons = detailed.reasons;
                        }
                    }
                }
                AddLog("Analysis Completed.", LogType::Success);
            }
            else {
                AddLog("No NIF loaded.", LogType::Warning);
            }
        }
        ShowTooltip("Analyzes bone weights and suggests slots based on Auto-Fix rules.");

    } // <--- if (Begin) の終わり

    ImGui::End();
}