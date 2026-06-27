#include "UI_PendingArea.h"
#include "Globals.h" // g_SessionChanges を使うために必要
#include <imgui.h>
#include <string>
#include <mutex> // ★追加: 排他制御用
#include <filesystem> // 追加: nif ファイル名取得

namespace fs = std::filesystem;

// =========================================================================
// 外部関数の呼び出し準備
// =========================================================================
// main.cpp にある保存関数を呼び出せるようにする
extern void SaveSessionChangesToFile();
//pending 統合エクスポートワーカー（宣言は Globals.h。eslPatchMode 引数あり）

// =========================================================================
// Pending Area の実装
// =========================================================================
// ★C26110/C26117 抑制: g_DataMutex(recursive_mutex) を lock_guard で RAII 保持しており正しいが、
//   SAL 並行性チェッカは recursive_mutex を正しく追跡できず誤検出するため抑制する（誤検出）。
#pragma warning(push)
#pragma warning(disable: 26110 26117)
void RenderPendingArea() {
    // ★固定・同期レイアウト
    ImVec2 _p, _s; GetMainPanelRect(MainPanel::Pending, _p, _s);
    ImGui::SetNextWindowPos(_p, ImGuiCond_Always);
    ImGui::SetNextWindowSize(_s, ImGuiCond_Always);

    // ウィンドウ開始
    if (ImGui::Begin("Items Pending save", nullptr, PIN_PANEL_FLAGS)) {

        // ★★★ 追加: ここでデータをロックします ★★★
        // この { } スコープを抜けるまで、他のスレッドはデータを触れなくなります
        // これにより、表示中のクラッシュを防ぎます
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Pending: %d", (int)g_SessionChanges.size());

        // ★統合 Export: 何を出力するかをチェックボックスで選択。状態は config に保存。
        bool prefsChanged = false;
        if (ImGui::Checkbox("slotdata Json Out", &g_ExportTxt)) prefsChanged = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Update slotdata-ChangeSet.json (per-mesh edit history / batch accumulator).");
        ImGui::SameLine();
        if (ImGui::Checkbox("ESL/ESP", &g_ExportEsl)) prefsChanged = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Generate per-source override-only ESL/ESP patches via slottool.");
        ImGui::SameLine();
        if (ImGui::Checkbox("NIF", &g_ExportNif)) prefsChanged = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write the modified NIF partitions to Output Root.");
        if (prefsChanged) { extern void SaveUnifiedConfig(); SaveUnifiedConfig(); }

        // 警告: 重要な出力が OFF のとき。
        if (!g_ExportEsl)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "! ESL off: changes may NOT apply in-game.");
        if (!g_ExportNif)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "! NIF off: may NOT apply in-game (OK if NiSkin/pending-only).");

        // 統合 Export ボタン
        bool nothingSelected = !(g_ExportTxt || g_ExportEsl || g_ExportNif);
        // ★Direct Overwrite 中はボタンを赤＋ "## DIRECT OVERWRITE ##" で強調（破壊的操作の明示）。
        const bool directMode = g_ForceOverwrite;
        if (directMode) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.00f, 0.00f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // ★ボタン文字を白に
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.85f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.55f, 0.0f, 1.0f));
        }
        // ★注意: ImGui はラベル中の "##" を ID 区切り（以降非表示）として扱うため、装飾には使えない。
        //   視認できる ">> <<" で強調し、"###id" で表示と無関係な安定 ID を付与する。
        const char* exportLabel = directMode
            ? ">> DIRECT OVERWRITE <<   Export Pending   >> DIRECT OVERWRITE <<###exportPendingBtn"
            : "Export Pending (selected outputs)###exportPendingBtn";
        if (nothingSelected) ImGui::BeginDisabled();
        if (ImGui::Button(exportLabel, ImVec2(-1, 30))) {
            if (!g_SessionChanges.empty() && !g_IsProcessing.load())
                std::thread(SaveAndExportAllWorker, g_ExportNif, g_ExportEsl, g_ExportTxt).detach();
            else if (g_SessionChanges.empty())
                AddLog("No pending changes to export.", LogType::Warning);
        }
        if (nothingSelected) ImGui::EndDisabled();
        ImGui::PopStyleColor(directMode ? 4 : 3); // directMode は文字色(白)も push しているため +1

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Exports the checked outputs for all pending changes.\nNIF/ESL go under Output Root (set it to <MO2>\\mods\\<folder> for MO2).\nNIF-failed records stay pending for retry.");
        }

        // ★詳細ログは Settings の Log Level = Verbose に統合（旧 Verbose export log トグルは廃止）。

        // リスト表示
        if (ImGui::BeginChild("Stack", ImVec2(0, 0), true)) {
            for (auto it = g_SessionChanges.begin(); it != g_SessionChanges.end(); ) {
                // キーを表示用に取得
                std::string label = it->first;

                ImGui::PushID(label.c_str());

                // 削除ボタン (赤色)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("X")) {
                    it = g_SessionChanges.erase(it); // ここで削除してもロック中なので安全
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    continue;
                }
                ImGui::PopStyleColor();

                ImGui::SameLine();

                // 表示: ESP名 + NIF名（slotdata の対象） -> [armaSlots]
                const SlotRecord& rec = it->second;
                std::string espName = rec.sourceFile.empty() ? "(Unknown ESP)" : rec.sourceFile;

                // nif名は優先順に nifPath, femalePath, malePath のファイル名を使う
                std::string nifCandidate;
                if (!rec.nifPath.empty()) nifCandidate = fs::path(rec.nifPath).filename().string();
                else if (!rec.femalePath.empty()) nifCandidate = fs::path(rec.femalePath).filename().string();
                else if (!rec.malePath.empty()) nifCandidate = fs::path(rec.malePath).filename().string();
                else nifCandidate = "(None)";

                ImGui::Text("%s : %s -> [%s]", espName.c_str(), nifCandidate.c_str(), rec.armaSlots.c_str());

                ImGui::PopID();
                ++it;
            }
            ImGui::EndChild();
        }

    } // <--- ここでロックが解除されます (std::lock_guardのデストラクタ)

    // ウィンドウ終了
    ImGui::End();
}
#pragma warning(pop)