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
// 外部関数の宣言（Globals.h に存在しないもののみ）
//  UIヘルパー
extern void ShowTooltip(const char* desc);
extern std::string OpenFileDialog(const char* filter);

// ワーカー・ロジック（既に Globals.h に宣言がある物も多いが、関数プロトタイプは冗長でも安全）
extern void ScanBodySlideWorker(); // Sourceモード用スキャン
extern void UpdateMeshList();      // 引数なし版 (Main NIF用)
extern void UpdateMeshList(nifly::NifFile& nif, std::vector<RenderMesh>& meshes, bool isRef);

extern void ApplySlotChanges(int meshIndex, const std::string& slotStr);
extern void SaveUnifiedConfig();
// 追加: セッション保存トリガー
extern void SaveSessionChangesToFile();

// 外部：スロット文字列パース
extern std::vector<int> ParseSlotString(const std::string& slotStr);

// ※ グローバル変数はすべて `Globals.h` で `extern` 宣言されているため、
//    ここで再宣言しない（型不一致による再定義エラー回避）。
// =========================================================================
// コントロールパネルの実装
// =========================================================================
void RenderControlPanel() {
    // 表示フラグチェック
    if (!g_ShowControlPanel) return;

    ImGui::SetNextWindowPos(ImVec2(10, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 650), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Control Panel")) {

        // ---------------------------------------------------------
        // Auto-Analyze (省略: 既存ロジックはそのまま)
        // ---------------------------------------------------------
        static std::string lastAnalyzedNif = "";
        bool needAnalyze = false;
        if (!g_CurrentNifPath.empty() && g_CurrentNifPath.string() != lastAnalyzedNif) {
            needAnalyze = true; lastAnalyzedNif = g_CurrentNifPath.string();
        }
        if (needAnalyze) {
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
                        if (isExcluded) { m.suggestions.clear(); m.debugReasons.clear(); continue; }
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
        // Settings (既存)
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

        if (ImGui::RadioButton("BS OSP NIF", &g_NifLoadMode, 3)) {
            g_ForceTabToSource = true;
            extern void ScanOSPWorker();
            if (g_OspFiles.empty() && !g_IsProcessing.load()) { std::thread(ScanOSPWorker).detach(); }
        }
        ShowTooltip("Directly edits .nif files located in BodySlide/ShapeData.");

        if (g_NifLoadMode == 1) ImGui::TextDisabled("(Reads BodySlide ShapeData)");
        if (g_NifLoadMode == 3) ImGui::TextDisabled("(Direct Edit: ShapeData/*.nif)");

        ImGui::Separator();

        // ---------------------------------------------------------
        // Status & Ref Body
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

                        // 参照があるときは初期注視を NIF 全体にする（要件）
                        g_ShowRef = true;
                        g_CamFocus = CamFocus::Nif;
                        g_CamTargetMeshIndex = -1;

                        g_CamOffset = glm::vec3(0.0f);
                        g_ModelRotation[0] = 0.0f; g_ModelRotation[1] = 0.0f; g_ModelRotation[2] = 0.0f;

                        // 適切なカメラ距離を決める（g_RefCamZOffset を反映）
                        float maxRefRadius = 0.0f;
                        for (const auto& rm : g_RefRenderMeshes) maxRefRadius = std::max(maxRefRadius, rm.boundingRadius);
                        float maxNifRadius = 0.0f;
                        for (const auto& rm : g_RenderMeshes) maxNifRadius = std::max(maxNifRadius, rm.boundingRadius);
                        float pickRadius = std::max(maxRefRadius, maxNifRadius);
                        if (pickRadius > 0.0f) g_CamDistance = std::max(pickRadius * 3.0f, 100.0f) + g_RefCamZOffset;
                    }
                }
                else {
                    std::string s = OpenFileDialog("NIF\0*.nif\0");
                    if (!s.empty() && g_RefNifData.Load(s) == 0) {
                        UpdateMeshList(g_RefNifData, g_RefRenderMeshes, true);
                        g_ShowRef = true;
                        g_CamFocus = CamFocus::Nif;
                        g_CamTargetMeshIndex = -1;
                        g_CamOffset = glm::vec3(0.0f);
                        g_ModelRotation[0] = 0.0f; g_ModelRotation[1] = 0.0f; g_ModelRotation[2] = 0.0f;
                        float maxRefRadius = 0.0f;
                        for (const auto& rm : g_RefRenderMeshes) maxRefRadius = std::max(maxRefRadius, rm.boundingRadius);
                        if (maxRefRadius > 0.0f) g_CamDistance = std::max(maxRefRadius * 3.0f, 100.0f) + g_RefCamZOffset;
                    }
                }
            }
        }
        ShowTooltip("Loads a reference body to display as an overlay.");
        ImGui::SameLine();
        ImGui::Checkbox("Show Ref", &g_ShowRef);
        ShowTooltip("Toggle the visibility of the reference body.");

        ImGui::Separator();

        // ---------------------------------------------------------
        // DB row復元: 選択レコード情報と Pending 表示（欠落していた宣言をここで復元）
        // ---------------------------------------------------------
        const SlotRecord* selRec = nullptr;
        if (g_SelectedRecordID != -1) {
            for (const auto& r : g_AllRecords) { if (r.id == g_SelectedRecordID) { selRec = &r; break; } }
        }
        auto makeKey = [](const SlotRecord* rec) {
            if (!rec) return std::string();
            return rec->sourceFile + "_" + rec->armaFormID;
            };
        bool showPendingForSelectedRec = false;
        if (selRec) {
            std::string dbRaw = selRec->armaSlots;
            std::string dbDisplay = dbRaw.empty() ? "(None)" : FormatSlotStringWithNames(dbRaw);
            std::string recKey = makeKey(selRec);
            showPendingForSelectedRec = (g_SessionChanges.count(recKey) > 0);

            ImGui::Text("DB Slots: %s", dbDisplay.c_str());
            if (showPendingForSelectedRec) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
                if (ImGui::Button("Write slotdata-Output.txt")) {
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

        // ---------------------------------------------------------
        // Mesh List (追加: Focus NIF ボタン)
        // ---------------------------------------------------------
        ImGui::Text("Mesh List:");
        ImGui::SameLine();
        ImGui::TextDisabled("Meshes: %d", static_cast<int>(g_RenderMeshes.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Focus NIF")) {
            // ユーザが明示的に NIF 全体を注視したい場合
            g_CamFocus = CamFocus::Nif;
            g_CamTargetMeshIndex = -1;
            g_CamOffset = glm::vec3(0.0f);
            g_ModelRotation[0] = g_ModelRotation[1] = g_ModelRotation[2] = 0.0f;
            float maxRadius = 0.0f;
            for (const auto& rm : g_RenderMeshes) maxRadius = std::max(maxRadius, rm.boundingRadius);

            // g_RefCamZOffset を読み込み反映（ref がある場合は加算）
            if (maxRadius > 0.0f) {
                g_CamDistance = std::max(maxRadius * 3.0f, 60.0f);
                if (g_ShowRef && !g_RefRenderMeshes.empty()) g_CamDistance += g_RefCamZOffset;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh Mesh List")) {
            UpdateMeshList(g_NifData, g_RenderMeshes, false);
            AddLog("Mesh list refreshed (manual).", LogType::Info);
        }
        ShowTooltip("DB slots are shown once at the top; below are meshes with their NIF slots.");

        // Begin child list (既存)
        if (ImGui::BeginChild("MeshList", ImVec2(0, 150), true)) {
            if (g_RenderMeshes.empty()) {
                ImGui::TextDisabled("No meshes detected. Load a NIF or click 'Refresh Mesh List'.");
                ImGui::EndChild();
            }
            else {
                // DB 行・各メッシュ表示の既存ロジック（略）...
                // --- 各メッシュを表示: 選択時の挙動をメッシュ注視に変更 ---
                for (int i = 0; i < g_RenderMeshes.size(); ++i) {
                    auto& mesh = g_RenderMeshes[i];
                    bool isSelected = (g_SelectedMeshIndex == i);

                    bool isBlocked = false;
                    for (const auto& bw : g_KeywordBlockedList) {
                        if (!bw.empty() && mesh.name.find(bw) != std::string::npos) { isBlocked = true; break; }
                    }

                    ImGui::PushID(i);
                    if (isBlocked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    else ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(mesh.color.r, mesh.color.g, mesh.color.b, 1.0f));

                    if (ImGui::Selectable(mesh.name.c_str(), isSelected)) {
                        g_SelectedMeshIndex = i;

                        // パン・回転をリセット
                        g_CamOffset = glm::vec3(0.0f);
                        g_ModelRotation[0] = 0.0f; g_ModelRotation[1] = 0.0f; g_ModelRotation[2] = 0.0f;

                        // 要件: mesh 選択時は mesh の中心を注視
                        g_CamFocus = CamFocus::Mesh;
                        g_CamTargetMeshIndex = i;

                        float newDist = std::max(mesh.boundingRadius * 3.0f, 60.0f);
                        g_CamDistance = newDist;
                    }
                    ImGui::PopStyleColor();

                    if (ImGui::IsItemClicked(1)) {
                        if (std::find(g_KeywordBlockedList.begin(), g_KeywordBlockedList.end(), mesh.name) == g_KeywordBlockedList.end()) {
                            g_KeywordBlockedList.push_back(mesh.name);
                            SaveUnifiedConfig();
                            AddLog("Added to BlockedList: " + mesh.name, LogType::Success);
                        }
                    }

                    std::string nifLine = "NIF Slots: ";
                    if (!mesh.slotInfo.empty()) nifLine += mesh.slotInfo;
                    else nifLine += "(None)";
                    ImGui::TextDisabled("%s", nifLine.c_str());
                    if (selRec && showPendingForSelectedRec) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Write slotdata-Output.txt");
                    }

                    ImGui::PopID();
                }

                ImGui::EndChild();
            }
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

        // Manual Input Area (手動入力)
        ImGui::Text("Manual Input:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##Input")) {
            memset(g_InputBuffer, 0, sizeof(g_InputBuffer));
        }

        // 入力欄
        ImGui::InputText("##Manual", g_InputBuffer, sizeof(g_InputBuffer));
        if (ImGui::IsItemHovered()) ShowTooltip("Enter slot numbers separated by commas (e.g., \"32, 50\").");

        // 入力スロット解析
        std::vector<int> currentInputSlots = ParseSlotString(g_InputBuffer);
        int inputCount = static_cast<int>(currentInputSlots.size());

        // Apply Button 1: Apply Change (This Mesh)
        bool disableThisMeshBtn = false;
        int maxPartsThis = 0;

        // メッシュ未選択時は計算・警告を行わない
        if (g_SelectedMeshIndex == -1 || g_SelectedMeshIndex >= g_RenderMeshes.size()) {
            disableThisMeshBtn = true;
        }
        else {
            const auto& selMesh = g_RenderMeshes[g_SelectedMeshIndex];

            // NiSkin 判定（slotInfo フィールドが "NiSkin" のとき）
            bool isNiSkin = (!selMesh.slotInfo.empty() && selMesh.slotInfo == "NiSkin");

            // activeSlots のサイズを安全に取得
            size_t partsSz = selMesh.activeSlots.size();
            const size_t kMaxReasonableParts = 1000;

            if (isNiSkin) {
                // NiSkin を含むメッシュは NIF 上の partition がない（=0）と扱うが、
                // UI 上は Apply を無効化しない（ESP 側への保存は可能にする）。
                maxPartsThis = 0;
                ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.2f, 1.0f),
                    "Note: This mesh uses NiSkin (no partitions on NIF). Slots can still be saved to ESP/slotdata.");
                // disableThisMeshBtn は true にしない（Apply を有効化するため）
                disableThisMeshBtn = false;
            }
            else if (partsSz == 0) {
                // partitions がゼロ（NiSkin ではないが partition がない）
                maxPartsThis = 0;
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "Error: No partitions detected on this mesh.");
                disableThisMeshBtn = true;
            }
            else if (partsSz > kMaxReasonableParts) {
                // 異常に大きな partition 数は無効化してログに残す
                maxPartsThis = 0;
                AddLog(std::string("Warning: suspicious partition count for mesh: ") + selMesh.name
                    + " count=" + std::to_string(partsSz), LogType::Warning);
                disableThisMeshBtn = true;
            }
            else {
                maxPartsThis = static_cast<int>(partsSz);
                // 入力と上限は後で比較して表示
            }
        }

        // UI 表示とボタン無効化
        // 注意: disableThisMeshBtn を後から変更すると ImGui::BeginDisabled()/EndDisabled() の整合が崩れるため、
        // 表示制御用の最終フラグを `finalDisable` としてここで決定します。
        bool finalDisable = disableThisMeshBtn;

        // まずメッセージ表示（BeginDisabled はまだ呼ばない）
        if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
            const auto& selMesh = g_RenderMeshes[g_SelectedMeshIndex];
            bool isNiSkinLocal = (!selMesh.slotInfo.empty() && selMesh.slotInfo == "NiSkin");

            if (!finalDisable) {
                if (maxPartsThis == 0 && !isNiSkinLocal) {
                    // NiSkin ではないが partitions が 0 の場合は無効化/エラー表示
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: No partitions detected on this mesh.");
                    finalDisable = true;
                }
                else if (inputCount > maxPartsThis && !isNiSkinLocal) {
                    // partitions を持つ通常メッシュで入力が多すぎる場合は無効化
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        "Too many slots! Input: %d / Limit: %d", inputCount, maxPartsThis);
                    finalDisable = true;
                }
                else if (isNiSkinLocal) {
                    // NiSkin の場合は NIF へは書き込めないが ESP に反映可能であることを明示する
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f),
                        "NiSkin: NIF has no partitions. Press Apply to save slots to ESP/slotdata (NIF not modified).");
                }
                else if (inputCount > 0) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                        "Partitions to fill: %d / %d", inputCount, maxPartsThis);
                }
                else {
                    ImGui::TextUnformatted("Enter slots to apply to the selected mesh.");
                }
            }
        }
        else {
            ImGui::TextUnformatted("Select a mesh to enable apply.");
        }

        // Apply ボタンを描画する前に finalDisable に従って BeginDisabled を呼ぶ（Begin/End の整合を保つ）
        if (finalDisable) ImGui::BeginDisabled(true);

        // Apply Button 1: Apply Change (This Mesh)
        if (ImGui::Button("Apply Change (This Mesh)", ImVec2(-1, 30))) {
            if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
                if (strlen(g_InputBuffer) == 0) {
                    AddLog("No input provided. Nothing to apply.", LogType::Warning);
                }
                else {
                    const auto& selMesh = g_RenderMeshes[g_SelectedMeshIndex];
                    std::string inputStr = g_InputBuffer;
                    std::vector<int> newSlots = ParseSlotString(inputStr);

                    bool isNiSkinLocal = (!selMesh.slotInfo.empty() && selMesh.slotInfo == "NiSkin");
                    size_t maxParts = selMesh.activeSlots.size();

                    if (isNiSkinLocal) {
                        // NiSkin の場合：NIF へは書き込めないが、ESP 側への同期（セッション登録）は可能。
                        // ここでは即時にファイルへ書かず、g_SessionChanges に登録するのみとする。
                        std::string currentNifPath = g_CurrentNifPath.string();

                        // Build candidate game paths (current nif + potential OSP outputs)
                        std::vector<std::string> validGamePaths;
                        validGamePaths.push_back(currentNifPath);
                        for (const auto& kv : g_OspFiles) {
                            const auto& ospData = kv.second;
                            for (const auto& set : ospData.sets) {
                                fs::path outBase = fs::path(set.outputPath) / set.outputName;
                                validGamePaths.push_back((outBase.string() + "_0.nif"));
                                validGamePaths.push_back((outBase.string() + "_1.nif"));
                                validGamePaths.push_back((outBase.string() + ".nif"));
                            }
                        }

                        // Normalization helper: trim, lowercase, backslash->slash, remove trailing slash
                        auto normalizePath = [](std::string s) -> std::string {
                            // trim
                            auto ltrim = [](std::string& str) {
                                str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                                };
                            auto rtrim = [](std::string& str) {
                                str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), str.end());
                                };
                            ltrim(s); rtrim(s);
                            // backslash -> slash
                            std::replace(s.begin(), s.end(), '\\', '/');
                            // lowercase
                            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                            // remove trailing slash
                            while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
                            return s;
                            };

                        // Prepare normalized candidates
                        std::vector<std::string> normCandidates;
                        for (const auto& p : validGamePaths) normCandidates.push_back(normalizePath(p));

                        // Helper: check if candidate ends with record path or filename matches
                        auto matchesRecord = [&](const std::string& candidate, const SlotRecord& rec) -> bool {
                            std::string male = normalizePath(rec.malePath);
                            std::string female = normalizePath(rec.femalePath);
                            if (!male.empty() && candidate.size() >= male.size() && candidate.find(male, candidate.size() - male.size()) != std::string::npos) return true;
                            if (!female.empty() && candidate.size() >= female.size() && candidate.find(female, candidate.size() - female.size()) != std::string::npos) return true;
                            // fallback: compare filenames
                            std::string candFile = fs::path(candidate).filename().string();
                            std::transform(candFile.begin(), candFile.end(), candFile.begin(), ::tolower);
                            std::string maleFile = fs::path(male).filename().string();
                            std::string femaleFile = fs::path(female).filename().string();
                            std::transform(maleFile.begin(), maleFile.end(), maleFile.begin(), ::tolower);
                            std::transform(femaleFile.begin(), femaleFile.end(), femaleFile.begin(), ::tolower);
                            if (!maleFile.empty() && candFile == maleFile) return true;
                            if (!femaleFile.empty() && candFile == femaleFile) return true;
                            return false;
                            };

                        int registered = 0;
                        {
                            std::lock_guard<std::mutex> lock(g_DataMutex);
                            for (auto& r : g_AllRecords) {
                                bool match = false;
                                for (const auto& cand : normCandidates) {
                                    if (matchesRecord(cand, r)) { match = true; break; }
                                }
                                if (!match) continue;

                                // 入力文字列をそのままレコードに登録（個数制限なし）
                                r.armaSlots = inputStr;
                                r.armoSlots = inputStr;
                                r.originalNifPath = currentNifPath;
                                r.isOspSource = false;
                                // NiSkin 登録経路では NIF を変更しない前提なので pendingOnly フラグを付与
                                r.pendingOnly = true;
                                r.nifModified = false;

                                std::string key = r.sourceFile + "_" + r.armaFormID;
                                g_SessionChanges[key] = r;
                                registered++;
                            }
                        }

                        if (registered > 0) {
                            AddLog("NiSkin: registered " + std::to_string(registered) + " record(s) to pending session. Use 'Write slotdata-Output.txt' or Export to flush.", LogType::Success);
                        }
                        else {
                            AddLog("NiSkin: no matching ESP records found for this NIF. Registered nothing.", LogType::Warning);
                        }
                    }
                    else {
                        // 通常メッシュ（partition を持つ）向けの既存処理
                        if (maxParts == 0) {
                            AddLog("Cannot apply: target mesh has no partitions detected.", LogType::Error);
                        }
                        else if (newSlots.size() > maxParts) {
                            AddLog("Input too long: target mesh supports up to " + std::to_string(maxParts) + " slots.", LogType::Error);
                        }
                        else {
                            ApplySlotChanges(g_SelectedMeshIndex, inputStr);
                        }
                    }
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
                std::vector<int> newSlots = ParseSlotString(targetSlots);
                int applyCount = 0;
                int skipCount = 0;
                for (int k = 0; k < g_RenderMeshes.size(); ++k) {
                    size_t maxParts = g_RenderMeshes[k].activeSlots.size();
                    if (maxParts == 0) {
                        // パーティションが検出されていないメッシュはスキップ
                        skipCount++;
                        continue;
                    }
                    if (newSlots.size() > maxParts) {
                        // 個別に適用できない場合はスキップ（ログは集約で出す）
                        skipCount++;
                        continue;
                    }
                    ApplySlotChanges(k, targetSlots);
                    applyCount++;
                }
                AddLog("Applied [" + targetSlots + "] to " + std::to_string(applyCount) + " meshes. Skipped: " + std::to_string(skipCount) + ".", LogType::Success);
            }
            else {
                AddLog("Input is empty. 'Apply to ALL' cancelled.", LogType::Warning);
            }
        }
        // BeginDisabled の呼び出しがあった場合はここで必ず EndDisabled を呼んで整合を保つ
        if (finalDisable) ImGui::EndDisabled();

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