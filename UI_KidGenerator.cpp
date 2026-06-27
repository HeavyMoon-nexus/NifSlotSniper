#include "UI_KidGenerator.h"
#include "Globals.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <algorithm> // sort, transform, find
#include <set>
#include <fstream>   // ofstream
#include <filesystem>
#include <cstring>   // strncpy_s, memset
#include <sstream>

namespace fs = std::filesystem;

// =========================================================================
// ヘルパー関数
// =========================================================================

static bool HasIntersection(const std::vector<int>& kwSlots, const std::vector<int>& nifSlots) {
    for (int ks : kwSlots) {
        for (int ns : nifSlots) {
            if (ks == ns) return true;
        }
    }
    return false;
}

// ★#4: 「半透明向け」キーワード判定。
//   NIF が alphaBlend を持つ（=半透明）とき、透け系キーワードを優先表示するための規約。
//   キーワード名 or matchWords のいずれかにシグナル語を含めば該当とみなす。
//   データ構造・config を変えずに済む軽量版（Handoff の方針(a)）。
static bool IsTranslucentKeyword(const KidKeyword& kw) {
    // 透け・透明を示唆する語（小文字・部分一致）。日本語ローマ字綴りも一部許容。
    static const char* kSignals[] = {
        "translucent", "transparent", "sheer", "seethrough", "see-through",
        "see_through", "transluc", "glass", "veil", "gauze", "mesh",
        "alpha", "skimpy", "lace", "lacy", "sukesuke", "suke",
    };
    auto containsSignal = [](const std::string& src) {
        std::string low = src;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        for (const char* sig : kSignals) {
            if (low.find(sig) != std::string::npos) return true;
        }
        return false;
        };

    if (containsSignal(kw.keyword)) return true;
    for (const auto& w : kw.matchWords) {
        if (containsSignal(w)) return true;
    }
    return false;
}

// カンマ区切りの文字列をベクターに分解し、空白を除去するヘルパー
static std::vector<std::string> ParseTargetBuffer(const char* buffer) {
    std::vector<std::string> result;
    std::string bufStr = buffer;
    std::stringstream ss(bufStr);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        item.erase(0, item.find_first_not_of(" \t\r\n"));
        item.erase(item.find_last_not_of(" \t\r\n") + 1);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

// =========================================================================
// KID Generator の実装
// =========================================================================
void RenderKidGenerator() {
    if (!g_ShowKIDGeneratorWindow) return;
    // 表示位置・サイズ設定
    ImGui::SetNextWindowPos(ImVec2(780, 270), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 450), ImGuiCond_FirstUseEver);


    // ウィンドウ開始
    if (ImGui::Begin("KID Generator", &g_ShowKIDGeneratorWindow)) {

        // ★#1: g_AllRecords / g_RecordSelectionMap を反復・参照するため g_DataMutex（recursive）で保護。
        std::lock_guard<std::recursive_mutex> _kidLock(g_DataMutex);

        // ★ NIF内の全メッシュのスロットを統合して取得
        std::set<int> distinctSlots;
        for (const auto& m : g_RenderMeshes) {
            for (int s : m.activeSlots) distinctSlots.insert(s);
        }
        std::vector<int> targetNifSlots(distinctSlots.begin(), distinctSlots.end());

        // ★#4: NIF 単位の半透明判定。いずれかのメッシュが alphaBlend なら半透明とみなす。
        bool nifTranslucent = false;
        for (const auto& m : g_RenderMeshes) {
            if (m.alphaBlend) { nifTranslucent = true; break; }
        }

        // --- ヘッダー表示 ---
        if (targetNifSlots.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Target: Whole NIF (No Slots Detected)");
        }
        else {
            std::string sList = "";
            for (int s : targetNifSlots) {
                if (!sList.empty()) sList += ",";
                sList += std::to_string(s);
            }
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Target: Whole NIF (All Meshes)");
            ImGui::TextDisabled("Detected Slots: [%s]", sList.c_str());
        }

        // ★#4: 半透明 NIF のヒント。透け系キーワードの付与を促す。
        if (nifTranslucent) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                "This item looks translucent (alpha blend).");
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                "Consider a sheer/transparent keyword (highlighted below).");
        }
        ImGui::Separator();

        // フィルタ入力
        if (ImGui::InputTextWithHint("##KidFilter", "search keyword...", g_KIDKeywordFilter.InputBuf, IM_ARRAYSIZE(g_KIDKeywordFilter.InputBuf))) {
            g_KIDKeywordFilter.Build();
        }

        // ソート用インデックス作成
        std::vector<int> sortedIndices;
        for (int i = 0; i < g_KeywordList.size(); ++i) sortedIndices.push_back(i);

        // ソート処理 (マッチするものを優先)
        // ★#4: 第1キー=スロット一致、第2キー=半透明一致（半透明 NIF のときのみ有効）。
        if (!targetNifSlots.empty() || nifTranslucent) {
            std::stable_sort(sortedIndices.begin(), sortedIndices.end(), [&](int a, int b) {
                bool aSlot = !targetNifSlots.empty() && HasIntersection(g_KeywordList[a].targetSlots, targetNifSlots);
                bool bSlot = !targetNifSlots.empty() && HasIntersection(g_KeywordList[b].targetSlots, targetNifSlots);
                if (aSlot != bSlot) return aSlot > bSlot; // スロット一致を最優先

                if (nifTranslucent) {
                    bool aTrans = IsTranslucentKeyword(g_KeywordList[a]);
                    bool bTrans = IsTranslucentKeyword(g_KeywordList[b]);
                    if (aTrans != bTrans) return aTrans > bTrans; // 次に半透明一致
                }
                return false; // それ以外は安定ソートで元順維持
                });
        }

        // 現在選択中のレコードの EditorID を取得 (小文字化して比較用)
        std::string currentEidLower = "";
        if (g_SelectedRecordID != -1) {
            for (const auto& r : g_AllRecords) {
                if (r.id == g_SelectedRecordID) {
                    currentEidLower = r.armoEditorID;
                    std::transform(currentEidLower.begin(), currentEidLower.end(), currentEidLower.begin(), ::tolower);
                    break;
                }
            }
        }

        // コンボボックス表示
        std::string preview = "Select Keyword...";
        if (g_SelectedKeywordIndex >= 0 && g_SelectedKeywordIndex < g_KeywordList.size()) {
            preview = g_KeywordList[g_SelectedKeywordIndex].keyword;
        }

        if (ImGui::BeginCombo("##KwCombo", preview.c_str())) {
            for (int idx : sortedIndices) {
                const auto& item = g_KeywordList[idx];
                if (!g_KIDKeywordFilter.PassFilter(item.keyword.c_str())) continue;

                // A. Slotマッチ判定
                bool slotMatch = !targetNifSlots.empty() && HasIntersection(item.targetSlots, targetNifSlots);
                // ★#4: C. 半透明マッチ判定（半透明 NIF のときのみ意味を持つ）
                bool transMatch = nifTranslucent && IsTranslucentKeyword(item);
                // B. EditorIDマッチ判定
                bool idMatch = false;
                if (!currentEidLower.empty()) {
                    if (!item.matchWords.empty()) {
                        for (const auto& w : item.matchWords) {
                            std::string wLower = w;
                            std::transform(wLower.begin(), wLower.end(), wLower.begin(), ::tolower);
                            if (!wLower.empty() && currentEidLower.find(wLower) != std::string::npos) {
                                idMatch = true; break;
                            }
                        }
                    }
                    else {
                        std::string kwLower = item.keyword;
                        std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);
                        if (currentEidLower.find(kwLower) != std::string::npos) idMatch = true;
                    }
                }

                std::string prefix = "";
                if (slotMatch && idMatch) prefix = "** ";
                else if (slotMatch || idMatch) prefix = "* ";
                // ★#4: 半透明一致は先頭に "~" を付けて識別しやすくする。
                if (transMatch) prefix = "~" + prefix;

                std::string label = prefix + item.keyword;
                if (!item.targetSlots.empty()) {
                    label += " (Slots:";
                    for (size_t k = 0; k < item.targetSlots.size(); ++k) {
                        label += std::to_string(item.targetSlots[k]) + (k == item.targetSlots.size() - 1 ? "" : ",");
                    }
                    label += ")";
                }
                if (transMatch) label += " [sheer]";

                // 色: EditorID 一致=オレンジ優先。なければ半透明一致=シアン。
                bool pushedColor = false;
                if (idMatch) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f)); pushedColor = true; }
                else if (transMatch) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f)); pushedColor = true; }

                if (ImGui::Selectable(label.c_str(), idx == g_SelectedKeywordIndex)) {
                    g_SelectedKeywordIndex = idx;
                }

                if (pushedColor) ImGui::PopStyleColor();
                if (idx == g_SelectedKeywordIndex) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ShowTooltip("Select a keyword definition.\n'*' = matches the current NIF's slots / EditorID.\n'~' (cyan, [sheer]) = suggested for translucent (alpha-blend) items.");

        // 選択中のARMO名表示
        std::string selRec = "(None)";
        if (g_SelectedRecordID != -1) for (const auto& r : g_AllRecords) if (r.id == g_SelectedRecordID) selRec = r.armoEditorID;
        ImGui::TextDisabled("Selected ARMO: %s", selRec.c_str());

        // ★重複していた "Target ARMO List:" ラベルを1つに集約。
        ImGui::Text("Target ARMO List:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear ARMO##ClearTargets")) {
            memset(g_KIDTargetBuffer, 0, 4096);
        }
        ShowTooltip("Clears the list of target ARMO EditorIDs.");

        // Add Button (修正版: スレッド削除・ロジック改善)
        if (ImGui::Button("Add ARMOs to KID list", ImVec2(-1, 30))) {
            // 現在のバッファをパースしてリスト化
            std::vector<std::string> currentList = ParseTargetBuffer(g_KIDTargetBuffer);
            int addCount = 0;

            auto AddUnique = [&](const std::string& editorID) {
                if (editorID.empty() || editorID == "(None)") return;
                // 完全一致で存在チェック
                if (std::find(currentList.begin(), currentList.end(), editorID) == currentList.end()) {
                    currentList.push_back(editorID);
                    addCount++;
                }
                };

            // 1. 選択中アイテム
            if (g_SelectedRecordID != -1) {
                for (const auto& r : g_AllRecords) if (r.id == g_SelectedRecordID) AddUnique(r.armoEditorID);
            }

            // 2. チェック済みアイテム
            for (const auto& record : g_AllRecords) {
                if (g_RecordSelectionMap.count(record.id) && g_RecordSelectionMap[record.id]) {
                    AddUnique(record.armoEditorID);
                }
            }

            if (addCount > 0) {
                // 文字列を再構築
                std::string newBuffer;
                for (size_t i = 0; i < currentList.size(); ++i) {
                    if (i > 0) newBuffer += ",";
                    newBuffer += currentList[i];
                }

                // バッファオーバーフロー対策
                if (newBuffer.length() < 4096) {
                    strncpy_s(g_KIDTargetBuffer, newBuffer.c_str(), _TRUNCATE);
                    AddLog("KID List updated: added " + std::to_string(addCount) + " ARMOs.", LogType::Success);
                }
                else {
                    AddLog("Error: Buffer overflow. Too many targets.", LogType::Error);
                }
            }
            else {
                AddLog("No new ARMOs to add.", LogType::Warning);
            }
        }
        ShowTooltip("Adds the currently selected record AND all checked records in the Database to the KID list.");

        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Result##ClearTargets")) {
            memset(g_KIDResultBuffer, 0, 4096);
        }
        ImGui::InputTextMultiline("##Targets", g_KIDTargetBuffer, 4096, ImVec2(-1, 80));

        // Generate Button
        if (ImGui::Button("Generate String", ImVec2(-1, 30))) {
            if (g_SelectedKeywordIndex >= 0 && g_SelectedKeywordIndex < g_KeywordList.size()) {
                std::string res = "Keyword = " + g_KeywordList[g_SelectedKeywordIndex].keyword + "|Armor|" + std::string(g_KIDTargetBuffer);
                strncpy_s(g_KIDResultBuffer, res.c_str(), _TRUNCATE);
            }
        }
        ShowTooltip("Generates the KID config line based on the selected Keyword and Target ARMOs.");

        ImGui::Separator();
        ImGui::Text("Generated KID Result:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##ClearResult")) {
            memset(g_KIDResultBuffer, 0, 4096);
        }
        ImGui::InputTextMultiline("##Result", g_KIDResultBuffer, 4096, ImVec2(-1, 80));

        // --- ファイル保存ロジック (Output Path対応) ---
        // ファイル名の自動決定（ソースESP名由来）
        std::string kidFileName = "output_kid.ini";
        if (g_SelectedRecordID != -1) {
            for (const auto& r : g_AllRecords) {
                if (r.id == g_SelectedRecordID && !r.sourceFile.empty()) {
                    kidFileName = fs::path(r.sourceFile).stem().string() + "_KID.ini";
                    break;
                }
            }
        }

        // Output Root が設定されていればそれを使う
        fs::path finalKidPath;
        if (strlen(g_OutputRootPath) > 0) {
            finalKidPath = fs::path(g_OutputRootPath) / kidFileName;
        }
        else {
            finalKidPath = fs::path(kidFileName);
        }

        std::string btnLabel = "Append to " + kidFileName;
        if (ImGui::Button(btnLabel.c_str(), ImVec2(-1, 30))) {
            if (strlen(g_KIDResultBuffer) > 0) {
                try {
                    if (finalKidPath.has_parent_path()) {
                        fs::create_directories(finalKidPath.parent_path());
                    }
                    // ★修正: fs::path を直接渡して文字化け防止 (C++17)
                    std::ofstream o(finalKidPath, std::ios::app);
                    if (o.is_open()) {
                        o << g_KIDResultBuffer << "\n";
                        AddLog("Appended to: " + finalKidPath.string(), LogType::Success);
                    }
                    else {
                        AddLog("Failed to open file for writing.", LogType::Error);
                    }
                }
                catch (const std::exception& e) {
                    AddLog("File Error: " + std::string(e.what()), LogType::Error);
                }
            }
            else {
                AddLog("KID Result is empty!", LogType::Warning);
            }
        }
        ShowTooltip(("Saves the generated result to a file named after the source ESP.\nCurrently: " + finalKidPath.filename().string()).c_str());

    } // End Begin
    ImGui::End();
}