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
                ImGui::InputText("##GameDataPath", g_GameDataPath, sizeof(g_GameDataPath));
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
                ImGui::InputText("##OutputRootPath", g_OutputRootPath, sizeof(g_OutputRootPath));
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

                // ★Step4: Synthesis Path 欄は撤去（slottool に置換）。

                // ★Step3: SlotTool Path（Mutagen CLI）
                ImGui::Text("SlotTool Path (slottool.exe)");
                ImGui::NextColumn();
                ImGui::PushID("SlotToolPath");
                ImGui::InputText("##SlotToolPath", g_SlotToolPath, sizeof(g_SlotToolPath));
                ImGui::SameLine();
                if (ImGui::Button("Browse##SlotTool")) {
                    const char* filter = "Executable\0*.exe\0All files\0*.*\0";
                    std::string picked = OpenFileDialog(filter);
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_SlotToolPath, sizeof(g_SlotToolPath), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // ★① BSArch Path（BSA 内テクスチャ表示用）
                ImGui::Text("BSArch Path (BSArch.exe)");
                ImGui::NextColumn();
                ImGui::PushID("BsArchPath");
                ImGui::InputText("##BsArchPath", g_BsArchPath, sizeof(g_BsArchPath));
                ImGui::SameLine();
                if (ImGui::Button("Browse##BsArch")) {
                    const char* filter = "Executable\0*.exe\0All files\0*.*\0";
                    std::string picked = OpenFileDialog(filter);
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_BsArchPath, sizeof(g_BsArchPath), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // ★#5: Ref Body Texture Folder（参照ボディ用テクスチャ）
                ImGui::Text("Ref Body Texture Folder");
                ImGui::NextColumn();
                ImGui::PushID("RefTexFolder");
                ImGui::InputText("##RefTexFolder", g_RefTexFolder, sizeof(g_RefTexFolder));
                ImGui::SameLine();
                if (ImGui::Button("Browse##RefTex")) {
                    std::string picked = SelectFolderDialog();
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_RefTexFolder, sizeof(g_RefTexFolder), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // ★スケルトン解決パス（女性）: ボーン名前解決の照合スケルトン NIF（';' 区切りで複数可）
                ImGui::Text("Skeleton NIF (Female, ';' for multiple)");
                ImGui::NextColumn();
                ImGui::PushID("SkeletonPathFemale");
                ImGui::InputText("##SkeletonPathFemale", g_SkeletonPathFemale, sizeof(g_SkeletonPathFemale));
                if (ImGui::IsItemHovered())
                    ShowTooltip("Skeleton NIF(s) used to resolve skin-bone names (e.g. XPMSE skeleton_female.nif).\nSeparate multiple with ';' (names are unioned). Full path or InputRoot-relative.\nUsed to compute boneResolution for the FEMALE display gender.");
                ImGui::SameLine();
                if (ImGui::Button("Browse##SkelF")) {
                    const char* filter = "NIF\0*.nif\0All files\0*.*\0";
                    std::string picked = OpenFileDialog(filter);
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_SkeletonPathFemale, sizeof(g_SkeletonPathFemale), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // ★スケルトン解決パス（男性）
                ImGui::Text("Skeleton NIF (Male, ';' for multiple)");
                ImGui::NextColumn();
                ImGui::PushID("SkeletonPathMale");
                ImGui::InputText("##SkeletonPathMale", g_SkeletonPathMale, sizeof(g_SkeletonPathMale));
                if (ImGui::IsItemHovered())
                    ShowTooltip("Skeleton NIF(s) used to resolve skin-bone names (e.g. XPMSE skeleton.nif).\nSeparate multiple with ';' (names are unioned). Full path or InputRoot-relative.\nUsed to compute boneResolution for the MALE display gender.");
                ImGui::SameLine();
                if (ImGui::Button("Browse##SkelM")) {
                    const char* filter = "NIF\0*.nif\0All files\0*.*\0";
                    std::string picked = OpenFileDialog(filter);
                    if (!picked.empty()) {
                        std::string norm = NormalizePath(picked);
                        strncpy_s(g_SkeletonPathMale, sizeof(g_SkeletonPathMale), norm.c_str(), _TRUNCATE);
                    }
                }
                ImGui::PopID();
                ImGui::NextColumn();

                // ★衣装シード出力トグル: ON で ChangeSet 書き出し時に costume_seed.json も出力
                ImGui::Text("Export costume seed (costume_seed.json)");
                ImGui::NextColumn();
                ImGui::PushID("ExportCostumeSeed");
                if (ImGui::Checkbox("##ExportCostumeSeed", &g_ExportCostumeSeed)) {
                    extern void SaveUnifiedConfig(); SaveUnifiedConfig();
                }
                if (ImGui::IsItemHovered())
                    ShowTooltip("When ON, writes costume_seed.json on Import DB / Convert completion, from the FULL record set\n(includes unedited accessories like nails). Independent of the ChangeSet (which stays an edit history).\nOnly records with at least one skinned shape are included; for the current display gender.\nSeed = { id, nif_path, gender, bones, resolved, default_enabled:null, strip_class:null }.\nPolicy fields stay null (NSS does not decide them; the consumer fills them in).");
                ImGui::PopID();
                ImGui::NextColumn();

                // ★① テクスチャキャッシュ上限（直近N个のNIF分を保持・ref body除外。0=無制限）
                ImGui::Text("Texture Cache (keep last N NIFs, 0=unlimited)");
                ImGui::NextColumn();
                ImGui::PushID("TexCacheNifs");
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputInt("##TexCacheNifs", &g_TexCacheNifLimit)) {
                    if (g_TexCacheNifLimit < 0) g_TexCacheNifLimit = 0;
                }
                if (ImGui::IsItemHovered())
                    ShowTooltip("Keeps textures from the last N viewed NIFs (ref body always kept).\nEviction happens ONLY when switching NIFs, never during draw, so the current NIF always loads fully (no reload loop) no matter how many textures it needs.\n1 = keep only the current NIF. 0 = unlimited.");
                ImGui::PopID();
                ImGui::NextColumn();

                // ★ログレベル（コンソール＋nss_log.txt 共通）。Verbose で診断ログ（[Tex]/[Alt]/[NormalMap]/Export詳細）も出す。
                ImGui::Text("Log Level");
                ImGui::NextColumn();
                ImGui::PushID("LogLevel");
                ImGui::SetNextItemWidth(160);
                const char* logLevels[] = { "Error", "Warning", "Info", "Verbose" };
                if (g_LogLevel < 0) g_LogLevel = 0; if (g_LogLevel > 3) g_LogLevel = 3;
                if (ImGui::Combo("##LogLevel", &g_LogLevel, logLevels, IM_ARRAYSIZE(logLevels))) {
                    extern void SaveUnifiedConfig(); SaveUnifiedConfig();
                }
                if (ImGui::IsItemHovered())
                    ShowTooltip("Filters both the console and nss_log.txt.\nError: errors only. Warning: +warnings. Info: normal operation (default).\nVerbose: +diagnostics ([Tex]/[Alt]/[NormalMap]/per-mesh export). Use to debug; large logs.");
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
                ImGui::TextDisabled(".esp and .osp files that are not shown in the Nif Database appear here. \nYou can also add them to this block list by right-clicking them in the Nif Database.");
                ImGui::EndTabItem();
            }

            // --- Tab 3: Blocked Keywords ---
            if (ImGui::BeginTabItem("Meshes BlockedList")) {
                ImGui::TextDisabled("word to ignore (Auto-Fix):");

                static char newKwBL[64] = "";
                ImGui::InputText("mesh name word", newKwBL, 64);
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
                ImGui::TextDisabled("Words in this block list are ignored during Auto-Fix/Suggest calculations, \nallowing you to skip items like collision bodies.\nYou can also add meshes to this block list by right-clicking them in the mesh list.");
                ImGui::EndTabItem();
            }
            // --- Tab 4: View Settings (新規追加) ---
            if (ImGui::BeginTabItem("View")) {
                ImGui::Text("Camera Settings");
                ImGui::Spacing();

                // 高さ調整スライダー (範囲は -100 〜 200 くらいあれば十分でしょう)
                ImGui::Text("Reference Body Height Offset");
                ImGui::DragFloat("##RefOffset", &g_RefCamZOffset, 0.5f, -100.0f, 300.0f, "%.1f");
                if (ImGui::IsItemHovered()) ShowTooltip("Adjusts the vertical camera center when the Reference Body is visible.\nDefault is 20.0.");

                ImGui::SameLine();
                if (ImGui::Button("Reset##RefOffset")) {
                    g_RefCamZOffset = 20.0f; // ★既定値（Globals.cpp の初期化）に一致させる
                }

                ImGui::Separator();
                ImGui::TextDisabled("Note: This setting is saved in config.ini\nThe view varies based on the reference body's collision floor mesh. \nAdjust it manually (3BA, BHUNP, SoftBody, etc).");

                ImGui::EndTabItem();
            }

            // --- Tab: Direct Overwrite（破壊的・既定 OFF。内部 var/config キーは ForceOverwrite のまま）---
            if (ImGui::BeginTabItem("Direct Overwrite")) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Direct Overwrite (DESTRUCTIVE)");
                ImGui::Spacing();
                if (ImGui::Checkbox("Overwrite original NIF / ESP directly (ignore Output Root)", &g_ForceOverwrite)) {
                    SaveUnifiedConfig(); // 破壊的設定なので即保存
                }
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "When ON, Export ignores Output Root and writes changes straight into the ORIGINAL files:\n"
                    "  - NIF: the resolved input NIF is overwritten in place.\n"
                    "  - ESP/ESL: slots are written into the defining plugin (no separate _SlotPatch.esp).\n"
                    "A one-time .bak backup is created next to each file before the first overwrite (existing .bak is kept).\n"
                    "Applies to both single-mesh edits and the Pending batch export. (OSP export is unaffected.)");
                ImGui::Spacing();
                if (g_ForceOverwrite)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        "ON: originals will be modified. Ensure MO2 / your mod manager tracks these files.");
                else
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "OFF: normal output to Output Root / patch ESP.");
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
                // ★Step4: Synthesis → SlotTool パスの正規化に置換
                std::string normalizedSlotTool = NormalizePath(std::string(g_SlotToolPath));
                strncpy_s(g_SlotToolPath, sizeof(g_SlotToolPath), normalizedSlotTool.c_str(), _TRUNCATE);
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