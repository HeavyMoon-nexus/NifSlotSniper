#include "UI_AnalysisDetails.h"
#include "Globals.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <algorithm> // find

// 外部関数の宣言
extern void SaveUnifiedConfig();
extern void AddLog(const std::string& message, LogType type);

// =========================================================================
// Debug Log Window の実装
// =========================================================================
void RenderAnalysisDetailsLog() {
    // 表示フラグが false なら何もしない
    if (!g_ShowAnalysisDetailsLog) return;

    // ウィンドウの初期設定
    ImGui::SetNextWindowPos(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Analysis Details", &g_ShowAnalysisDetailsLog)) {
        // 選択中のメッシュがあるかチェック
        if (g_SelectedMeshIndex != -1 && g_SelectedMeshIndex < g_RenderMeshes.size()) {
            auto& m = g_RenderMeshes[g_SelectedMeshIndex];

            ImGui::Text("Target Mesh: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", m.name.c_str());

            ImGui::SameLine();
            // Block ボタン (ここにも置いておくと便利)
            if (ImGui::SmallButton("Block This Mesh")) {
                if (std::find(g_KeywordBlockedList.begin(), g_KeywordBlockedList.end(), m.name) == g_KeywordBlockedList.end()) {
                    g_KeywordBlockedList.push_back(m.name);
                    SaveUnifiedConfig();
                    AddLog("Blocked: " + m.name, LogType::Success);
                }
            }

            ImGui::Separator();

            // ログリスト表示エリア
            if (ImGui::BeginChild("DebugLogContent", ImVec2(0, 0), true)) {
                if (m.debugReasons.empty()) {
                    ImGui::TextDisabled("No analysis data available for this mesh.\nPlease run 'Analyze' in Control Panel.");
                }
                else {
                    for (const auto& r : m.debugReasons) {
                        // 理由の種類によって色分け
                        ImVec4 color = (r.type == "COMBO") ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f) : // Orange
                            (r.type == "NAME") ? ImVec4(0.4f, 0.8f, 1.0f, 1.0f) : // Blue
                            ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray

                        ImGui::TextColored(color, "[%s]", r.type.c_str());
                        ImGui::SameLine();
                        ImGui::Text("%s -> Slot %d (+%d)", r.match.c_str(), r.slotID, r.score);
                    }
                }
                ImGui::EndChild();
            }
        }
        else {
            ImGui::TextDisabled("No analysis data available for this mesh.\nPlease run 'Analyze' in Control Panel.");
        }

    }
    ImGui::End();
}