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
//pending 統合エクスポートワーカー
extern void SaveAndExportAllWorker();

// =========================================================================
// Pending Area の実装
// =========================================================================
void RenderPendingArea() {
    // 表示位置・サイズ設定
    ImGui::SetNextWindowPos(ImVec2(780, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);

    // ウィンドウ開始
    if (ImGui::Begin("Items Pending save")) {

        // ★★★ 追加: ここでデータをロックします ★★★
        // この { } スコープを抜けるまで、他のスレッドはデータを触れなくなります
        // これにより、表示中のクラッシュを防ぎます
        std::lock_guard<std::mutex> lock(g_DataMutex);

        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Pending: %d", (int)g_SessionChanges.size());

        // 保存ボタン
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.85f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.55f, 0.0f, 1.0f));
        if (ImGui::Button("Save TXT & Export NIFs", ImVec2(-1, 30))) {
            std::thread(SaveAndExportAllWorker).detach();
        }
        ImGui::PopStyleColor(3);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Saves slotdata-output.txt AND exports all pending NIFs.\nOSP-derived files will go to ShapeData, others to meshes.");
        }
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