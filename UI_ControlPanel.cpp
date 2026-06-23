#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // ★SEH(__try/__except) 用
#endif

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
#include <NifFile.hpp>

namespace fs = std::filesystem;

// ★単発ロード系SEH: 参照ボディ NIF の Load をネイティブ例外から保護（不正 NIF でアプリ全体が
//   落ちるのを防ぐ）。Main.cpp の SafeNifLoad は static のためここでは file-local 版を用意。
static const uintmax_t kCpMaxNifBytes = 512ull * 1024 * 1024; // ★B6: 巨大 NIF 拒否
#ifdef _WIN32
static int CpSafeNifLoadSEH(nifly::NifFile& nif, const fs::path& p) {
    __try { return nif.Load(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}
#else
static int CpSafeNifLoadSEH(nifly::NifFile& nif, const fs::path& p) { return nif.Load(p); }
#endif
// ★B2: SEH に加え C++ 例外（bad_alloc 等）も捕捉（UI スレッドの参照ボディ読込経路）。
static int CpSafeNifLoad(nifly::NifFile& nif, const fs::path& p) {
    std::error_code ec;
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > kCpMaxNifBytes) {
        AddLog("Ref NIF too large, refusing to load (" + std::to_string(sz) + " bytes): " + p.string(), LogType::Error);
        return -9998;
    }
    try { return CpSafeNifLoadSEH(nif, p); }
    catch (const std::exception& e) { AddLog(std::string("Ref NIF load exception: ") + e.what() + " (" + p.string() + ")", LogType::Error); return -9997; }
    catch (...) { AddLog("Ref NIF load unknown exception (" + p.string() + ")", LogType::Error); return -9996; }
}

// =========================================================================
// External function declarations
// =========================================================================
extern void ShowTooltip(const char* desc);
extern std::string OpenFileDialog(const char* filter);

extern void UpdateMeshList();
extern void UpdateMeshList(nifly::NifFile& nif, std::vector<RenderMesh>& meshes, bool isRef);

extern void ApplySlotChanges(int meshIndex, const std::string& slotStr);
extern void SaveUnifiedConfig();
extern void SaveSessionChangesToFile();

extern std::vector<int> ParseSlotString(const std::string& slotStr);

// ★#5: 指定パスの参照ボディを読み込む共有ヘルパー（ボタン・起動時の両方が使う）。
//   データ読込のみ行い、g_ShowRef/カメラは呼び出し側で制御する。
bool LoadReferenceBody(const std::string& path) {
    // ★③ 失敗を握りつぶさずログに出す（空パスは自動起動時の無指定なので無言で false）。
    if (path.empty()) return false;
    if (!fs::exists(path)) {
        AddLog("Ref Body not found: " + path, LogType::Warning);
        return false;
    }
    if (CpSafeNifLoad(g_RefNifData, fs::path(path)) != 0) {
        AddLog("Ref Body load failed (nifly): " + path, LogType::Error);
        return false;
    }
    g_RefNifPath = path;
    strncpy_s(g_RefBodyPath, sizeof(g_RefBodyPath), path.c_str(), _TRUNCATE);
    UpdateMeshList(g_RefNifData, g_RefRenderMeshes, true);
    AddLog("Ref Body Loaded: " + fs::path(path).filename().string(), LogType::Success);
    return true;
}

// =========================================================================
// Control Panel implementation
// =========================================================================
// ★C26110/C26117 抑制: g_DataMutex(recursive_mutex) を lock_guard で RAII 保持しており正しいが、
//   SAL 並行性チェッカは recursive_mutex を正しく追跡できず誤検出するため抑制する（誤検出）。
#pragma warning(push)
#pragma warning(disable: 26110 26117)
void RenderControlPanel() {
    if (!g_ShowControlPanel) return;

    // ★固定・同期レイアウト
    ImVec2 _p, _s; GetMainPanelRect(MainPanel::ControlPanel, _p, _s);
    ImGui::SetNextWindowPos(_p, ImGuiCond_Always);
    ImGui::SetNextWindowSize(_s, ImGuiCond_Always);

    if (ImGui::Begin("Control Panel", nullptr, PIN_PANEL_FLAGS)) {

        // ★#1: 本パネルは g_AllRecords / g_SessionChanges / g_OspFiles を反復・参照する。
        //   detach ワーカーの書込と競合するため描画全体を g_DataMutex（recursive）で保護する。
        std::lock_guard<std::recursive_mutex> _cpLock(g_DataMutex);

        // ---------------------------------------------------------
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
                        // ★修正: SuggestTopSlots は直後に上書きされる二重計算だったため除去
                        auto detailed = SlotDictionary::AnalyzeDetailed(res.influentialBones, res.meshName, currentEid);
                        m.suggestions = detailed.topSlots;
                        m.debugReasons = detailed.reasons;
                    }
                }
            }
            AddLog("Auto-Analysis Completed.", LogType::Info);
        }

        // ---------------------------------------------------------
        // ---------------------------------------------------------
        ImGui::Text("Target Gender:");
        if (ImGui::RadioButton("Male", g_TargetGender == 0)) g_TargetGender = 0; ImGui::SameLine();
        if (ImGui::RadioButton("Female", g_TargetGender == 1)) g_TargetGender = 1;

        // ★Tier3: 表示モード切替（既定フラット）。透過判定したいときだけ ON。
        //   ★ON にした瞬間にテクスチャキャッシュ（負キャッシュ含む）をクリアして再取得。
        //   これにより、以前失敗した（負キャッシュされた）テクスチャも再試行される。
        //   BSA の展開済み集合・索引は保持されるので再 unpack は走らない（速い）。
        if (ImGui::Checkbox("Texture Mode", &g_TextureMode)) {
            if (g_TextureMode) ClearTextureCache();
        }
        ShowTooltip("OFF: flat slot-colors (fast, for slot work).\nON: shows loose/BSA diffuse textures with alpha (for KID translucency).\nToggling ON re-scans textures (clears the load cache, retries failures).\nBSA unpack happens once per archive and is kept cached.");

        // ★#3-debug: 法線マップ基底の切替（ref ボディの法線崩れを実機で当てる）。
        if (g_TextureMode) {
            ImGui::Indent();
            ImGui::Checkbox("Normal Map", &g_UseNormalMap);
            ShowTooltip("Apply NIF normal maps (slot1). Turn OFF to see geometry-only shading.\nUse this to confirm whether a dark patch comes from the normal map.");
            if (g_UseNormalMap) {
                ImGui::SameLine(); ImGui::Checkbox("Flip Green", &g_NmFlipGreen);
                ShowTooltip("Flip the normal map's green channel (DirectX vs OpenGL convention).\nIf bumps look inverted (concave<->convex), try this.");
                ImGui::SameLine(); ImGui::Checkbox("Flip Hand", &g_NmFlipHand);
                ShowTooltip("Flip tangent handedness. If one half of a mirrored-UV body is dark/wrong, try this.");
            }
            // ★テクスチャ解決ログ（LOOSE/BSA/Alt）は Settings の Log Level = Verbose に統合。
            ImGui::Unindent();
        }

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
            // ★#2: 起動側で g_IsProcessing を立ててから detach（二重起動・clear 競合の防止）。
            if (g_OspFiles.empty() && !g_IsProcessing.load()) { g_IsProcessing = true; std::thread(ScanOSPWorker).detach(); }
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
        // ★#5: g_StatusMessage は AddLog が g_LogMutex 下で書き換える。ワーカーからの更新中に
        //   無ロックで .c_str() を読むと std::string 再確保と競合し得るため、ロックしてコピーする。
        std::string statusCopy;
        { std::lock_guard<std::mutex> lk(g_LogMutex); statusCopy = g_StatusMessage; }
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", statusCopy.c_str());

        ImGui::Separator();

        if (ImGui::Button("Load Ref Body")) {
            // ★#5: 記憶パス → 既定 femalebody → ファイルダイアログ の順で選ぶ。
            std::string chosen;
            if (strlen(g_RefBodyPath) > 0 && fs::exists(g_RefBodyPath)) {
                chosen = g_RefBodyPath;
            }
            else if (strlen(g_InputRootPath) > 0) {
                fs::path refP = fs::path(g_InputRootPath) / "meshes" / "actors" / "character" / "character assets" / "femalebody_1.nif";
                if (fs::exists(refP)) chosen = refP.string();
            }
            if (chosen.empty()) {
                std::string s = OpenFileDialog("NIF\0*.nif\0");
                if (!s.empty()) chosen = s;
            }

            if (!chosen.empty() && LoadReferenceBody(chosen)) {
                g_ShowRef = true;
                g_CamFocus = CamFocus::Nif;
                g_CamTargetMeshIndex = -1;
                g_CamOffset = glm::vec3(0.0f);
                g_ModelRotation[0] = 0.0f; g_ModelRotation[1] = 0.0f; g_ModelRotation[2] = 0.0f;
                float maxRefRadius = 0.0f;
                for (const auto& rm : g_RefRenderMeshes) maxRefRadius = std::max(maxRefRadius, rm.boundingRadius);
                float maxNifRadius = 0.0f;
                for (const auto& rm : g_RenderMeshes) maxNifRadius = std::max(maxNifRadius, rm.boundingRadius);
                float pickRadius = std::max(maxRefRadius, maxNifRadius);
                if (pickRadius > 0.0f) g_CamDistance = std::max(pickRadius * 3.0f, 100.0f) + g_RefCamZOffset;
                SaveUnifiedConfig(); // ★パスを記憶
            }
        }
        ShowTooltip("Loads a reference body. The chosen path is remembered and auto-loaded on startup.");
        ImGui::SameLine();
        ImGui::Checkbox("Show Ref", &g_ShowRef);
        ShowTooltip("Toggle the visibility of the reference body.");

        ImGui::Separator();

        // ---------------------------------------------------------
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
            // ★スレッド安全: g_SessionChanges は detach ワーカー（Export）が erase するため、読みもロックする。
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                showPendingForSelectedRec = (g_SessionChanges.count(recKey) > 0);
            }

            ImGui::Text("DB Slots: %s", dbDisplay.c_str());
            // ★削除（要望③）: 「Write slotdata-Output.txt」ボタンは撤去（出力は Pending の統合 Export に一元化）。
            //   このレコードに pending 編集があることだけ示す。
            if (showPendingForSelectedRec) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "(pending)");
            }

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
        // ---------------------------------------------------------
        ImGui::Text("Mesh List:");
        ImGui::SameLine();
        ImGui::TextDisabled("Meshes: %d", static_cast<int>(g_RenderMeshes.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Focus NIF")) {
            g_CamFocus = CamFocus::Nif;
            g_CamTargetMeshIndex = -1;
            g_CamOffset = glm::vec3(0.0f);
            g_ModelRotation[0] = g_ModelRotation[1] = g_ModelRotation[2] = 0.0f;
            float maxRadius = 0.0f;
            for (const auto& rm : g_RenderMeshes) maxRadius = std::max(maxRadius, rm.boundingRadius);

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

        if (ImGui::BeginChild("MeshList", ImVec2(0, 150), true)) {
            if (g_RenderMeshes.empty()) {
                ImGui::TextDisabled("No meshes detected. Load a NIF or click 'Refresh Mesh List'.");
                ImGui::EndChild();
            }
            else {
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

                        g_CamOffset = glm::vec3(0.0f);
                        g_ModelRotation[0] = 0.0f; g_ModelRotation[1] = 0.0f; g_ModelRotation[2] = 0.0f;

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

                    // ★④: BSD はパーティション単位で1行ずつ表示（どの index にどのスロットかを明示）。
                    if (mesh.slotInfo == "NiSkin") {
                        ImGui::TextDisabled("NIF: NiSkin (no partitions)");
                    }
                    else if (!mesh.activeSlots.empty()) {
                        ImGui::TextDisabled("NIF partitions:");
                        if (selRec && showPendingForSelectedRec) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "(pending)");
                        }
                        for (size_t pi = 0; pi < mesh.activeSlots.size(); ++pi) {
                            int sid = mesh.activeSlots[pi];
                            ImGui::TextDisabled("  [%d] slot %d (%s)", (int)pi, sid, SlotDictionary::GetSlotName(sid).c_str());
                        }
                    }
                    else {
                        ImGui::TextDisabled("NIF Slots: (None)");
                    }

                    ImGui::PopID();
                }

                ImGui::EndChild();
            }
        }

        ImGui::Separator();

        // ---------------------------------------------------------
        // ---------------------------------------------------------
        if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
            auto& mesh = g_RenderMeshes[g_SelectedMeshIndex];
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Selected: %s", mesh.name.c_str());

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
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Selected: (None)");
            ImGui::TextDisabled("(Select a mesh to see suggestions)");
        }

        // ★④ per-partition エディタ（案B）: BSD 選択時、各パーティションのスロットを個別指定して
        //   This Mesh に適用する（どの index に何が入るか明示）。NiSkin/パーティション無しは非表示。
        if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < (int)g_RenderMeshes.size()) {
            auto& selM = g_RenderMeshes[g_SelectedMeshIndex];
            const bool isNiSkin = (selM.slotInfo == "NiSkin");
            const size_t partCount = selM.activeSlots.size();
            if (!isNiSkin && partCount > 0) {
                static int s_ppMeshIdx = -2;
                static std::vector<int> s_ppInputs;
                // 選択変更 or パーティション数変化（適用後の再構築）で NIF 実値へ再同期。
                if (s_ppMeshIdx != g_SelectedMeshIndex || s_ppInputs.size() != partCount) {
                    s_ppMeshIdx = g_SelectedMeshIndex;
                    s_ppInputs = selM.activeSlots;
                }
                ImGui::Separator();
                ImGui::Text("Per-partition slots (This Mesh): %d", (int)partCount);
                ImGui::TextDisabled("Each row = one NIF partition; set its slot explicitly.");
                for (size_t pi = 0; pi < partCount; ++pi) {
                    ImGui::PushID((int)(1000 + pi));
                    ImGui::SetNextItemWidth(90);
                    ImGui::InputInt("##ppslot", &s_ppInputs[pi], 0);
                    ImGui::SameLine();
                    ImGui::Text("[%d] -> slot %d (%s)", (int)pi, s_ppInputs[pi],
                        SlotDictionary::GetSlotName(s_ppInputs[pi]).c_str());
                    ImGui::PopID();
                }
                if (ImGui::Button("Apply Per-Partition (This Mesh)")) {
                    std::string s;
                    for (size_t i = 0; i < s_ppInputs.size(); ++i) { if (i) s += ","; s += std::to_string(s_ppInputs[i]); }
                    ApplySlotChanges(g_SelectedMeshIndex, s);
                }
                if (ImGui::IsItemHovered()) ShowTooltip("Writes each partition's slot exactly as shown (partition index -> slot).");
                ImGui::SameLine();
                if (ImGui::Button("Reset##pp")) { s_ppInputs = selM.activeSlots; }

                // ★BSD→NiSkin 変換（このメッシュのスロットを NIF/ESP 両方から外す）。
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.0f, 1.0f));
                if (ImGui::Button("Convert to NiSkin (remove slots)")) {
                    ConvertSelectedMeshToNiSkin();
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ShowTooltip("Converts this mesh's BSDismemberSkinInstance to a plain NiSkinInstance.\nRemoves its biped slots from BOTH the NIF and the ESP (armaSlots).\nApplied on Export (and to the original NIF when Direct Overwrite is ON).\nFor advanced use: NiSkin-distributed costumes, SMP collision meshes, etc. One-way.");
            } // close: if (!isNiSkin && partCount > 0)
            else if (isNiSkin) {
                // ★NiSkin→BSD 逆変換（連続テスト用）。1 パーティション（slot 32）を付与して BSD 化。
                ImGui::Separator();
                ImGui::TextDisabled("This mesh is NiSkin (no biped slots).");
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.40f, 0.55f, 1.0f));
                if (ImGui::Button("Convert to BSD (add slot)")) {
                    ConvertSelectedMeshToBSD();
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ShowTooltip("Converts this NiSkinInstance mesh to a BSDismemberSkinInstance with one partition (slot 32),\nthen edit the slot in the per-partition editor above. Adds the slot to the ESP too.\nApplied on Export (and to the original NIF when Direct Overwrite is ON).");
            }
        }     // close: if (selected mesh valid)

        ImGui::Separator();

        ImGui::Text("Manual Input:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##Input")) {
            memset(g_InputBuffer, 0, sizeof(g_InputBuffer));
        }

        ImGui::InputText("##Manual", g_InputBuffer, sizeof(g_InputBuffer));
        if (ImGui::IsItemHovered()) ShowTooltip("Enter slot numbers separated by commas (e.g., \"32, 50\").");

        std::vector<int> currentInputSlots = ParseSlotString(g_InputBuffer);
        int inputCount = static_cast<int>(currentInputSlots.size());

        // Apply Button 1: Apply Change (This Mesh)
        bool disableThisMeshBtn = false;
        int maxPartsThis = 0;

        if (g_SelectedMeshIndex == -1 || g_SelectedMeshIndex >= g_RenderMeshes.size()) {
            disableThisMeshBtn = true;
        }
        else {
            const auto& selMesh = g_RenderMeshes[g_SelectedMeshIndex];

            bool isNiSkin = (!selMesh.slotInfo.empty() && selMesh.slotInfo == "NiSkin");

            size_t partsSz = selMesh.activeSlots.size();
            const size_t kMaxReasonableParts = 1000;

            if (isNiSkin) {
                maxPartsThis = 0;
                ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.2f, 1.0f),
                    "Note: This mesh uses NiSkin (no partitions on NIF). Slots can still be saved to ESP/slotdata.");
                disableThisMeshBtn = false;
            }
            else if (partsSz == 0) {
                maxPartsThis = 0;
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "Error: No partitions detected on this mesh.");
                disableThisMeshBtn = true;
            }
            else if (partsSz > kMaxReasonableParts) {
                maxPartsThis = 0;
                AddLog(std::string("Warning: suspicious partition count for mesh: ") + selMesh.name
                    + " count=" + std::to_string(partsSz), LogType::Warning);
                disableThisMeshBtn = true;
            }
            else {
                maxPartsThis = static_cast<int>(partsSz);
            }
        }

        bool finalDisable = disableThisMeshBtn;

        if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
            const auto& selMesh = g_RenderMeshes[g_SelectedMeshIndex];
            bool isNiSkinLocal = (!selMesh.slotInfo.empty() && selMesh.slotInfo == "NiSkin");

            if (!finalDisable) {
                if (maxPartsThis == 0 && !isNiSkinLocal) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: No partitions detected on this mesh.");
                    finalDisable = true;
                }
                else if (inputCount > maxPartsThis && !isNiSkinLocal) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        "Too many slots! Input: %d / Limit: %d", inputCount, maxPartsThis);
                    finalDisable = true;
                }
                else if (isNiSkinLocal) {
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
                            auto ltrim = [](std::string& str) {
                                str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                                };
                            auto rtrim = [](std::string& str) {
                                str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), str.end());
                                };
                            ltrim(s); rtrim(s);
                            std::replace(s.begin(), s.end(), '\\', '/');
                            AsciiLowerInplace(s); // ★B5: パス照合キー
                            while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
                            return s;
                            };

                        auto buildTailKey = [](const std::string& normalized, size_t segments) -> std::string {
                            if (normalized.empty() || segments == 0) return std::string();
                            std::vector<std::string> parts;
                            size_t start = 0;
                            while (start < normalized.size()) {
                                size_t pos = normalized.find('/', start);
                                std::string token = (pos == std::string::npos)
                                    ? normalized.substr(start)
                                    : normalized.substr(start, pos - start);
                                if (!token.empty()) parts.push_back(token);
                                if (pos == std::string::npos) break;
                                start = pos + 1;
                            }
                            if (parts.size() < segments) return std::string();
                            std::string result;
                            for (size_t i = parts.size() - segments; i < parts.size(); ++i) {
                                if (!result.empty()) result += '/';
                                result += parts[i];
                            }
                            return result;
                            };

                        // Prepare normalized candidates
                        std::vector<std::string> normCandidates;
                        for (const auto& p : validGamePaths) normCandidates.push_back(normalizePath(p));

                        auto matchesRecord = [&](const std::string& candidate, const SlotRecord& rec) -> bool {
                            std::string male = normalizePath(rec.malePath);
                            std::string female = normalizePath(rec.femalePath);

                            const std::string candTail3 = buildTailKey(candidate, 3);
                            const std::string candTail2 = buildTailKey(candidate, 2);
                            const std::string candTail1 = buildTailKey(candidate, 1);

                            auto matchPath = [&](const std::string& recPath) -> bool {
                                if (recPath.empty()) return false;

                                if (candidate.size() >= recPath.size() &&
                                    candidate.compare(candidate.size() - recPath.size(), recPath.size(), recPath) == 0) {
                                    return true;
                                }

                                const std::string recTail3 = buildTailKey(recPath, 3);
                                if (!recTail3.empty() && !candTail3.empty() && candTail3 == recTail3) return true;

                                const std::string recTail2 = buildTailKey(recPath, 2);
                                if (!recTail2.empty() && !candTail2.empty() && candTail2 == recTail2) return true;

                                if (recTail2.empty() && recTail3.empty()) {
                                    const std::string recTail1 = buildTailKey(recPath, 1);
                                    if (!recTail1.empty() && !candTail1.empty() && candTail1 == recTail1) return true;
                                }

                                return false;
                                };

                            return matchPath(male) || matchPath(female);
                        };

                        int registered = 0;
                        {
                            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                            for (auto& r : g_AllRecords) {
                                bool match = false;
                                for (const auto& cand : normCandidates) {
                                    if (matchesRecord(cand, r)) { match = true; break; }
                                }
                                if (!match) continue;

                                r.armaSlots = inputStr;
                                r.armoSlots = inputStr;
                                r.originalNifPath = currentNifPath;
                                r.isOspSource = false;
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

        if (ImGui::Button("Apply to ALL Listed Meshes", ImVec2(-1, 30))) {
            if (strlen(g_InputBuffer) > 0) {
                std::string targetSlots = g_InputBuffer;
                std::vector<int> newSlots = ParseSlotString(targetSlots);
                int applyCount = 0;
                int skipCount = 0;
                for (int k = 0; k < g_RenderMeshes.size(); ++k) {
                    size_t maxParts = g_RenderMeshes[k].activeSlots.size();
                    if (maxParts == 0) {
                        skipCount++;
                        continue;
                    }
                    if (newSlots.size() > maxParts) {
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
                            // ★修正: SuggestTopSlots は直後に上書きされる二重計算だったため除去
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

    }

    ImGui::End();
}
#pragma warning(pop)
