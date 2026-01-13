#include "UI_Settings.h"
#include "Globals.h"
#include "SlotDictionary.hpp" // SaveRulesなどで必要なら
#include <imgui.h>
#include <cstring> // strlen, memset用
#include <string>
#include <algorithm>
#include <cctype>

static std::string TrimString(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

static std::string NormalizePath(std::string p) {
    p = TrimString(p);
    // バックスラッシュをスラッシュに
    std::replace(p.begin(), p.end(), '\\', '/');
    // 末尾スラッシュを除去（ただしルート "/" は残す）
    while (!p.empty() && p.size() > 1 && p.back() == '/') p.pop_back();
    // 小文字化（比較用に一貫化、表示要件があれば外す）
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return p;
}


extern void SaveUnifiedConfig();
extern void AddLog(const std::string& msg, LogType type);//Globals.cpp にある AddLog() を使う

// --- Settings Window ---
extern std::string OpenFileDialog(const char* filter);
extern std::string SelectFolderDialog();

void RenderSettingsWindow() {
    // 表示フラグが false なら即帰る
    if (!g_ShowSettingsWindow) return;

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    // ウィンドウ開始
    if (ImGui::Begin("Settings", &g_ShowSettingsWindow)) {

        if (ImGui::BeginTabBar("SettingsTabs")) {

            // --- Tab 1: Paths ---
            if (ImGui::BeginTabItem("Paths")) {
                ImGui::Text("Path Settings");

                // 2列レイアウトにして5行を縦に並べる（ラベル列 / 入力列）
                ImGui::Columns(2, "SettingsCols", false);

                // Row 1: Game Data Path
                ImGui::Text("Game Data Path");
                ImGui::NextColumn();
                ImGui::PushID("GameDataPath");
                ImGui::InputText("##GameDataPath", g_GameDataPath, 1024);
                ImGui::SameLine();
                if (ImGui::Button("Browse##GameData")) {
                    std::string picked = SelectFolderDialog();
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_GameDataPath, sizeof(g_GameDataPath), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // Row 2: Input root (auto set text)
                ImGui::Text("Input root =");
                ImGui::NextColumn();
                {
                    std::string note;
                    if (strlen(g_GameDataPath) > 0) note = std::string(g_GameDataPath) + "/meshes";
                    else note = "{GameDataPath}/meshes";
                    ImGui::TextDisabled("%s", note.c_str());
                }
                ImGui::NextColumn();

                // Row 3: Output Root Path
                ImGui::Text("Output Root Path");
                ImGui::NextColumn();
                ImGui::PushID("OutputRootPath");
                ImGui::InputText("##OutputRootPath", g_OutputRootPath, 1024);
                ImGui::SameLine();
                if (ImGui::Button("Browse##OutputRoot")) {
                    std::string picked = SelectFolderDialog();
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_OutputRootPath, sizeof(g_OutputRootPath), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // Row 4: Synthesis Path
                ImGui::Text("Synthesis Path");
                ImGui::NextColumn();
                ImGui::PushID("SynthesisPath");
                ImGui::InputText("##SynthesisPath", g_SynthesisPath, 1024);
                ImGui::SameLine();
                if (ImGui::Button("Browse##Synthesis")) {
                    // フィルタ: 実行ファイル優先
                    const char* filter = "Executable\0*.exe\0All files\0*.*\0";
                    std::string picked = OpenFileDialog(filter);
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_SynthesisPath, sizeof(g_SynthesisPath), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // Row 5: Slot Data Folder
                ImGui::Text("Slot Data Folder (slotdata-*.txt)");
                ImGui::NextColumn();
                ImGui::PushID("SlotDataPath");
                ImGui::InputText("##SlotDataPath", g_SlotDataPath, 4096);
                ImGui::SameLine();
                if (ImGui::Button("Browse##SlotData")) {
                    std::string picked = SelectFolderDialog();
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_SlotDataPath, sizeof(g_SlotDataPath), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // 列を戻す
                ImGui::Columns(1);

                ImGui::TextWrapped("Loads slotdata-*.txt files from the folder. Defaults to './slotdataTXT' if left blank.");

                ImGui::EndTabItem();
            }

            // --- Tab 2: Blocked Sources ---
            if (ImGui::BeginTabItem("Source BlockedList")) {
                ImGui::TextDisabled("ESP name to ignore (Auto-Fix):");

                static char newSrcBL[64] = "";
                ImGui::InputText("ESP Name", newSrcBL, 64);
                ImGui::SameLine();
                if (ImGui::Button("Add") && strlen(newSrcBL) > 0) {
                    g_SourceBlockedList.push_back(newSrcBL);
                    memset(newSrcBL, 0, 64);
                }

                ImGui::BeginChild("SBLList", ImVec2(0, 200), true);
                for (int i = 0; i < (int)g_SourceBlockedList.size(); ++i) {
                    if (ImGui::Button(("X##sb" + std::to_string(i)).c_str())) {
                        g_SourceBlockedList.erase(g_SourceBlockedList.begin() + i);
                        i--;
                        continue;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", g_SourceBlockedList[i].c_str());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // --- Tab 3: Blocked Keywords ---
            if (ImGui::BeginTabItem("Meshes BlockedList")) {
                ImGui::TextDisabled("word to ignore (Auto-Fix):");

                static char newKwBL[64] = "";
                ImGui::InputText("word", newKwBL, 64);
                ImGui::SameLine();
                if (ImGui::Button("Add") && strlen(newKwBL) > 0) {
                    g_KeywordBlockedList.push_back(newKwBL);
                    memset(newKwBL, 0, 64);
                }

                ImGui::BeginChild("KBLList", ImVec2(0, 200), true);
                for (int i = 0; i < (int)g_KeywordBlockedList.size(); ++i) {
                    if (ImGui::Button(("X##kb" + std::to_string(i)).c_str())) {
                        g_KeywordBlockedList.erase(g_KeywordBlockedList.begin() + i);
                        i--;
                        continue;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", g_KeywordBlockedList[i].c_str());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        // 保存ボタン
        if (ImGui::Button("Save Settings", ImVec2(-1, 30))) {
            // UI 上のパスを正規化してから保存（SaveUnifiedConfig がそれらを読む前提）
            {
                std::string normalizedGamePath = NormalizePath(std::string(g_GameDataPath));
                strncpy_s(g_GameDataPath, sizeof(g_GameDataPath), normalizedGamePath.c_str(), _TRUNCATE);
            }
            {
                std::string normalizedOutput = NormalizePath(std::string(g_OutputRootPath));
                strncpy_s(g_OutputRootPath, sizeof(g_OutputRootPath), normalizedOutput.c_str(), _TRUNCATE);
            }
            {
                std::string normalizedSynthesis = NormalizePath(std::string(g_SynthesisPath));
                strncpy_s(g_SynthesisPath, sizeof(g_SynthesisPath), normalizedSynthesis.c_str(), _TRUNCATE);
            }

            // InputRoot を自動設定： GameDataPath + "/meshes"（GameDataPath が空なら空にする）
            if (g_GameDataPath[0] != '\0') {
                std::string inRoot = std::string(g_GameDataPath) + "/meshes";
                inRoot = NormalizePath(inRoot);
                strncpy_s(g_InputRootPath, sizeof(g_InputRootPath), inRoot.c_str(), _TRUNCATE);
            }
            else {
                g_InputRootPath[0] = '\0';
            }

            // 正規化済みの値で保存
            SaveUnifiedConfig();
            //AddLog(); // 必要ならログを残す
        }

    } // <--- if (Begin) の終わり

    // ★重要：End は必ず if の外で呼ぶ！
    ImGui::End();
}