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
#include <unordered_set> // ★フィルタ可視IDキャッシュ用
#include <unordered_map> // RecordSelectionMap用
#include "OSP_Logic.h"
#include <sstream>

// =========================================================================
// 外部関数の呼び出し準備
// (Globals.h で宣言されていますが、明示的に書くことでリンクエラーを防ぎます)
// =========================================================================
extern void LoadNifFileCore(const std::string& path);
extern void SaveSessionChangesToFile();
extern void SaveUnifiedConfig();
extern fs::path ConstructSafePath(const std::string& root, const std::string& rel);

// ★ ワーカー関数
extern void ScanOSPWorker();
extern void ExportOSPWorker();

// 追加: 遅延読み込み API を UI 側から呼べるように extern 宣言→OSP_Logic.hで置き換え
//extern void LoadOSPDetails(const std::string& filename);

// =========================================================================
// NIF Database の実装 (UI描画のみ)
// =========================================================================
void RenderDatabase() {
    // ★固定・同期レイアウト
    ImVec2 _p, _s; GetMainPanelRect(MainPanel::Database, _p, _s);
    ImGui::SetNextWindowPos(_p, ImGuiCond_Always);
    ImGui::SetNextWindowSize(_s, ImGuiCond_Always);

    // ウィンドウ開始
    if (ImGui::Begin("NIF Database", nullptr, PIN_PANEL_FLAGS)) {

        // ★#1: 本パネルは g_DisplayTree / g_AllRecords / g_RecordSelectionMap / g_OspFiles を毎フレーム
        //   反復・変更する。detach ワーカー（ScanOSP/ExportOSP/ScanNifSlots 等）の書込と競合するため、
        //   描画全体を g_DataMutex（recursive）で保護する。配下で呼ぶ LoadOSPDetails 等の再ロックは安全。
        std::lock_guard<std::recursive_mutex> _dbLock(g_DataMutex);

        if (ImGui::BeginTabBar("DBTabs")) {

            // ============================================================
            // TAB 1: Slotdata- List
            // ============================================================
            ImGuiTabItemFlags listFlags = g_ForceTabToList ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Slotdata- List", nullptr, listFlags)) {

                if (g_ForceTabToList) g_ForceTabToList = false;
                else if (g_NifLoadMode == 3) { g_NifLoadMode = 0; }

                // ★#2: 不一致レコード集合を 1 フレーム 1 回スナップショット（ワーカーの swap との競合回避）。
                std::set<int> mismatchSnap;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
                    mismatchSnap = g_SlotMismatchRecords;
                }

                // --- 上部コントロール ---
                // ★バッチ: 旧「Reload TXT」を「slotdata-Output.txt → Pending」に置換。
                //   読み込んだ後、Pending パネルの統合 Export（NIF/ESL/txt チェック）で一括生成する。
                if (ImGui::Button("Load slotdata -> Pending (batch)")) {
                    extern void LoadSlotdataIntoPending();
                    LoadSlotdataIntoPending();
                }
                ShowTooltip("Batch mode: loads slotdata-ChangeSet.json into the Pending list.\nThen choose outputs (NIF/ESL/txt) in the Pending panel and press Export to generate them all at once.");

                // ★移行: 旧 slotdata-Output.txt → slotdata-ChangeSet.json（per-mesh を NIF から復元）。
                ImGui::SameLine();
                if (ImGui::Button("Convert old TXT -> ChangeSet.json")) {
                    if (g_IsProcessing.load()) AddLog("Busy - please wait for the current operation to finish.", LogType::Warning);
                    else { extern void ConvertOldSlotdataToChangeSet(); g_IsProcessing = true; std::thread(ConvertOldSlotdataToChangeSet).detach(); }
                }
                ShowTooltip("One-time migration: reads the legacy slotdata-Output.txt and writes slotdata-ChangeSet.json (merged).\nPer-mesh slots are recovered by reading each record's NIF (set Gender / Game Data Path first, same as Import DB).\nRows whose NIF can't be read keep union-only. The old txt is left untouched.");

                // ★Step3/4: slottool（Mutagen CLI）連携。Synthesis 起動ボタンは撤去。
                if (ImGui::Button("Import DB (slottool)")) {
                    ImportDatabaseViaSlotTool();
                }
                ShowTooltip("Runs 'slottool export' and loads the result directly (no slotdata-*.txt).\nMust be launched via MO2 so the child process inherits the correct load order (USVFS).");
                // ★Export は Pending パネルの「Export Pending (selected outputs)」に一元化（NIF/ESL/txt をチェックで選択）。

                // ★#2: DB slot と NIF パーティションの不一致をスキャンし、ツリーで該当 NIF をオレンジ表示。
                ImGui::SameLine();
                if (ImGui::Button("Check NIF<->DB Slots")) {
                    if (g_AllRecords.empty()) AddLog("DB is empty. Import DB first.", LogType::Warning);
                    else if (g_IsProcessing.load()) AddLog("Busy - please wait for the current operation to finish.", LogType::Warning);
                    else { g_IsProcessing = true; std::thread(ScanNifSlotsWorker).detach(); }
                }
                ShowTooltip("Loads every DB record's NIF and compares its partition slots to the DB slots.\nMismatched NIFs are shown in orange in the tree below. NiSkin (no partitions) is excluded.\nHeavy on first run (loads many NIFs).");
                if (!mismatchSnap.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.0f, 1.0f), "Slot mismatches: %d (orange below)", (int)mismatchSnap.size());

                ImGui::Separator();
                g_SlotFilter.Draw("Filter");
                ShowTooltip("search for esp name, slot number, mesh name, etc.\nYou may find unexpected hits.");
				ImGui::Text("prefix esp:,slot:,mesh:=\nexample esp:mymod,slot:49,mesh:skirt,long\nDon't rely on it completely.");
				ImGui::Separator();

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

                // -----------------------------------------------------------------------------
                // 高度な検索フィルターロジック (AND検索 & プレフィックス対応)
                // -----------------------------------------------------------------------------
                // ★最適化2: recordLookup は DB 変更時のみ再構築（毎フレーム再構築を回避）。
                static std::unordered_map<int, SlotRecord*> recordLookup;
                static int s_lookupDbVersion = -1;
                if (s_lookupDbVersion != g_DbVersion) {
                    recordLookup.clear();
                    recordLookup.reserve(g_AllRecords.size());
                    for (auto& rec : g_AllRecords) recordLookup[rec.id] = &rec;
                    s_lookupDbVersion = g_DbVersion;
                }

                // 検索トークンの定義
                struct SearchToken {
                    enum Type { Generic, Esp, Slot, Mesh } type = Generic; // ★C26495: 既定値初期化
                    std::string value;
                };

                // 大文字小文字を無視して含まれているか確認するヘルパー
                auto StringContains = [](const std::string& haystack, const std::string& needle) -> bool {
                    if (needle.empty()) return true;
                    if (haystack.empty()) return false;
                    auto it = std::search(
                        haystack.begin(), haystack.end(),
                        needle.begin(), needle.end(),
                        [](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); }
                    );
                    return (it != haystack.end());
                    };

                // 入力文字列をトークンに分解する (例: "iron esp:Dawnguard" -> [Generic:iron, Esp:Dawnguard])
                auto ParseSearchTokens = [&](const char* input) -> std::vector<SearchToken> {
                    std::vector<SearchToken> tokens;
                    std::string text = input;
                    std::string item;
                    std::stringstream ss(text);

                    // ★変更: 区切りを ',' に指定
                    while (std::getline(ss, item, ',')) {
                        // 前後の空白を除去 (トリミング)
                        const size_t first = item.find_first_not_of(" \t");
                        if (first == std::string::npos) continue; // 空白のみならスキップ
                        const size_t last = item.find_last_not_of(" \t");
                        item = item.substr(first, (last - first + 1));

                        if (item.empty()) continue;

                        SearchToken token;
                        if (item.rfind("esp:", 0) == 0 && item.size() > 4) {
                            token.type = SearchToken::Esp;
                            token.value = item.substr(4);
                        }
                        else if (item.rfind("slot:", 0) == 0 && item.size() > 5) {
                            token.type = SearchToken::Slot;
                            token.value = item.substr(5);
                        }
                        else if (item.rfind("mesh:", 0) == 0 && item.size() > 5) {
                            token.type = SearchToken::Mesh;
                            token.value = item.substr(5);
                        }
                        else {
                            token.type = SearchToken::Generic;
                            token.value = item;
                        }
                        tokens.push_back(token);
                    }
                    return tokens;
                };

                // 現在のフィルター入力を解析
                // ImGuiTextFilterの内部バッファを直接参照
                std::vector<SearchToken> currentTokens = ParseSearchTokens(g_SlotFilter.InputBuf);
                bool isFilterActive = !currentTokens.empty();

                // ★A3: スロット CSV（"32,52"）に、指定スロット番号が「完全一致」で含まれるか。
                //   want が数値でなければ false。前後空白は無視。
                const auto SlotListHasExact = [](const std::string& slotCsv, const std::string& want) -> bool {
                    std::string w = want;
                    w.erase(0, w.find_first_not_of(" \t"));
                    if (auto p = w.find_last_not_of(" \t"); p != std::string::npos) w.erase(p + 1);
                    if (w.empty()) return false;
                    std::stringstream ss(slotCsv);
                    std::string tok;
                    while (std::getline(ss, tok, ',')) {
                        const size_t a = tok.find_first_not_of(" \t");
                        if (a == std::string::npos) continue;
                        const size_t b = tok.find_last_not_of(" \t");
                        if (tok.substr(a, b - a + 1) == w) return true;
                    }
                    return false;
                    };

                // レコード単体の判定 (すべてのトークンにマッチする必要がある = AND検索)
                const auto RecordMatchesFilter = [&](int recId) -> bool {
                    if (!isFilterActive) return true;

                    auto it = recordLookup.find(recId);
                    if (it == recordLookup.end()) return false;
                    const auto& rec = *it->second;

                    for (const auto& token : currentTokens) {
                        bool tokenMatch = false;

                        // ファイル名のみ抽出用
                        auto GetFileName = [](const std::string& p) { return fs::path(p).filename().string(); };

                        switch (token.type) {
                        case SearchToken::Esp:
                            // esp: 指定時はソースファイル名のみ検索
                            if (StringContains(rec.sourceFile, token.value)) tokenMatch = true;
                            break;

                        case SearchToken::Slot:
                            // ★A3: スロットは「番号の完全一致」で判定（部分一致だと slot:3 が 30〜39 に、
                            //   slot:5 が 52 等に誤ヒットする）。CSV を分解して厳密比較する。
                            if (SlotListHasExact(rec.armaSlots, token.value)) tokenMatch = true;
                            else if (SlotListHasExact(rec.armoSlots, token.value)) tokenMatch = true;
                            break;

                        case SearchToken::Mesh:
                            // mesh: 指定時はNIFパス(およびファイル名)を検索
                            if (StringContains(rec.nifPath, token.value)) tokenMatch = true;
                            else if (StringContains(rec.malePath, token.value)) tokenMatch = true;
                            else if (StringContains(rec.femalePath, token.value)) tokenMatch = true;
                            // パスが長い場合、ファイル名だけでもヒットするようにする
                            else if (StringContains(GetFileName(rec.nifPath), token.value)) tokenMatch = true;
                            break;

                        case SearchToken::Generic:
                            // 指定なし: すべてのフィールドを横断検索
                            if (StringContains(rec.displayText, token.value))  tokenMatch = true;
                            else if (StringContains(rec.sourceFile, token.value))   tokenMatch = true;
                            else if (StringContains(rec.armaEditorID, token.value)) tokenMatch = true;
                            else if (StringContains(rec.armoEditorID, token.value)) tokenMatch = true;
                            else if (StringContains(rec.armaSlots, token.value))    tokenMatch = true;
                            else if (StringContains(rec.armoSlots, token.value))    tokenMatch = true;
                            else if (StringContains(rec.nifPath, token.value))      tokenMatch = true;
                            else if (StringContains(GetFileName(rec.nifPath), token.value)) tokenMatch = true;
                            break;
                        }

                        // 一つでもマッチしないトークンがあれば、そのレコードは除外 (AND動作)
                        if (!tokenMatch) return false;
                    }
                    return true;
                    };

                // ★最適化1: 可視ID集合を「フィルタ or DB が変わった時だけ」再計算してキャッシュ。
                //   毎フレームの RecordMatchesFilter 全件走査（std::search / fs::path 構築）を排除する。
                static std::unordered_set<int> s_visibleIds;
                static std::string s_cachedFilter = "\x01"; // 不可能な初期値で必ず初回計算
                static int s_visibleDbVersion = -1;
                {
                    std::string curFilter = g_SlotFilter.InputBuf;
                    if (s_cachedFilter != curFilter || s_visibleDbVersion != g_DbVersion) {
                        s_visibleIds.clear();
                        if (isFilterActive) {
                            s_visibleIds.reserve(g_AllRecords.size());
                            for (auto& rec : g_AllRecords)
                                if (RecordMatchesFilter(rec.id)) s_visibleIds.insert(rec.id);
                        }
                        s_cachedFilter = curFilter;
                        s_visibleDbVersion = g_DbVersion;
                    }
                }

                // 以下、ツリー構造用の判定ロジック（キャッシュ済み s_visibleIds を参照）
                const auto NifHasMatches = [&](const std::string& nifName, const std::vector<int>& ids) {
                    if (!isFilterActive) return true;
                    // 子のIDが一つでも可視集合にあれば表示（AND判定は s_visibleIds 構築時に済）
                    for (int id : ids) if (s_visibleIds.count(id)) return true;
                    return false;
                    };
                const auto GenderHasMatches = [&](const std::string& genderName, const std::map<std::string, std::vector<int>>& nifMap) {
                    if (!isFilterActive) return true;
                    for (const auto& [nifName, ids] : nifMap) {
                        if (NifHasMatches(nifName, ids)) return true;
                    }
                    return false;
                    };
                const auto SourceHasMatches = [&](const std::string& espName, const std::map<std::string, std::map<std::string, std::vector<int>>>& genderMap) {
                    if (!isFilterActive) return true;
                    for (const auto& [genderName, nifMap] : genderMap) {
                        if (GenderHasMatches(genderName, nifMap)) return true;
                    }
                    return false;
                    };
                // --- 追加ここまで ---

                // ★A3 修正: 個別レコードの可視判定。フィルタ有効時は s_visibleIds に
                //   含まれるものだけを「表示・全選択・チェック操作」の対象にする。
                //   これが無いと、NIF グループ内に 1 件でも一致があると配下の全レコード
                //   （別スロットの兄弟＝カラバリ等）まで表示・選択されてしまう。
                const auto vis = [&](int id) { return !isFilterActive || s_visibleIds.count(id) != 0; };

                // ツリー描画
                for (auto& [espName, genderMap] : g_DisplayTree) {
                    // 古い条件: if (!g_SlotFilter.PassFilter(espName.c_str())) continue;
                    // 新しい条件:
                    if (!SourceHasMatches(espName, genderMap)) continue;

                    bool isBlocked = false;
                    for (const auto& bl : g_SourceBlockedList) if (espName == bl) isBlocked = true;
                    if (isBlocked) continue;

                    bool allInESP = true;
                    for (auto& [g, nm] : genderMap) for (auto& [n, ids] : nm) for (int id : ids) if (vis(id) && !g_RecordSelectionMap[id]) allInESP = false;
                    bool checkESP = allInESP;

                    if (ImGui::Checkbox(("##" + espName).c_str(), &checkESP)) {
                        for (auto& [g, nm] : genderMap) for (auto& [n, ids] : nm) for (int id : ids) if (vis(id)) g_RecordSelectionMap[id] = checkESP;
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
                            if (!GenderHasMatches(genderName, nifMap)) continue;
                            bool allInGen = true;
                            for (auto& [n, ids] : nifMap) for (int id : ids) if (vis(id) && !g_RecordSelectionMap[id]) allInGen = false;
                            bool checkGen = allInGen;
                            if (ImGui::Checkbox(("##" + espName + genderName).c_str(), &checkGen)) {
                                for (auto& [n, ids] : nifMap) for (int id : ids) if (vis(id)) g_RecordSelectionMap[id] = checkGen;
                            }
                            ImGui::SameLine();
                            if (ImGui::TreeNode(genderName.c_str())) {
                                for (auto& [nifName, ids] : nifMap) {
                                    if (!NifHasMatches(nifName, ids)) continue;
                                    bool allInNif = true;
                                    for (int id : ids) if (vis(id) && !g_RecordSelectionMap[id]) allInNif = false;
                                    bool checkNif = allInNif;
                                    if (ImGui::Checkbox(("##" + nifName).c_str(), &checkNif)) {
                                        for (int id : ids) if (vis(id)) g_RecordSelectionMap[id] = checkNif;
                                    }
                                    ImGui::SameLine();
                                    // ★#2: DB slot と NIF パーティションが不一致のレコードを含む NIF はオレンジ表示。
                                    bool nifSlotMismatch = false;
                                    if (!mismatchSnap.empty())
                                        for (int id : ids) if (vis(id) && mismatchSnap.count(id)) { nifSlotMismatch = true; break; }
                                    if (nifSlotMismatch) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.0f, 1.0f));
                                    bool nifNodeOpen = ImGui::TreeNode(nifName.c_str());
                                    if (nifSlotMismatch) ImGui::PopStyleColor();
                                    if (nifSlotMismatch && ImGui::IsItemHovered())
                                        ShowTooltip("DB slot != NIF partition slot (mismatch). Review this NIF's partitions vs the record's slots.");
                                    if (nifNodeOpen) {
                                        // ダブルクリックは「最初の可視レコード」を読み込む
                                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                            for (int id : ids) if (vis(id)) { LoadAction(id); break; }
                                        }
                                        for (int id : ids) {
                                            if (!vis(id)) continue; // ★A3: 非一致レコードは隠す
                                            // ★最適化2: 線形探索を O(1) マップ参照に置換
                                            auto _lit = recordLookup.find(id);
                                            SlotRecord* r = (_lit != recordLookup.end()) ? _lit->second : nullptr;
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

                // ★削除（要望③）: 旧「Export Selected NIF」ボタンは撤去。出力は Pending パネルの統合 Export
                //   （NIF/ESL/Json チェック）に一元化した。ツリーの選択（g_RecordSelectionMap）は KID Generator
                //   などで引き続き使うので残す。これに伴い BatchExportWorker は未使用（DEAD）。

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
                    // ★#2: 二重起動防止。起動側で g_IsProcessing を立ててから detach する
                    //   （ワーカー内で立てるとガードとの間に競合窓ができ、g_OspFiles を並行 clear し得た）。
                    if (g_IsProcessing.load()) AddLog("Busy - please wait for the current operation to finish.", LogType::Warning);
                    else { g_IsProcessing = true; std::thread(ScanOSPWorker).detach(); }
                }
                ShowTooltip("Scans the 'SliderSets' folder for .osp files.");
                ImGui::SameLine();
                static ImGuiTextFilter ospFilter;
                ospFilter.Draw("Filter OSP");
                ShowTooltip("search for mesh name, etc.\nYou may find unexpected hits.");
                ImGui::Text("prefix m:=mesh");

                // ※ SlotList側と同じヘルパー関数を再定義 (スコープが異なるため)
                struct SearchToken { enum Type { Generic, Mesh } type = Generic; std::string value; }; // ★C26495: 既定値初期化

                auto StringContains = [](const std::string& haystack, const std::string& needle) {
                    if (needle.empty()) return true;
                    if (haystack.empty()) return false;
                    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char c1, char c2) { return std::toupper(c1) == std::toupper(c2); });
                    return (it != haystack.end());
                    };

                auto ParseOspTokens = [&](const char* input) {
                    std::vector<SearchToken> tokens;
                    std::string text = input;
                    std::string item;
                    std::stringstream ss(text);

                    // ★変更: 区切りを ',' に指定
                    while (std::getline(ss, item, ',')) {
                        // 前後の空白を除去 (トリミング)
                        const size_t first = item.find_first_not_of(" \t");
                        if (first == std::string::npos) continue;
                        const size_t last = item.find_last_not_of(" \t");
                        item = item.substr(first, (last - first + 1));

                        if (item.empty()) continue;

                        SearchToken token;
                        if (item.rfind("mesh:", 0) == 0 && item.size() > 5) {
                            token.type = SearchToken::Mesh;
                            token.value = item.substr(5);
                        }
                        else {
                            token.type = SearchToken::Generic;
                            token.value = item;
                        }
                        tokens.push_back(token);
                    }
                    return tokens;
                };

                std::vector<SearchToken> ospTokens = ParseOspTokens(ospFilter.InputBuf);
                bool isOspFilterActive = !ospTokens.empty();

                const auto OspMatches = [&](const std::string& ospName, const OSPFile& data) {
                    if (!isOspFilterActive) return true;

                    for (const auto& token : ospTokens) {
                        bool tokenMatch = false;
                        if (token.type == SearchToken::Generic) {
                            // ファイル名を検索
                            if (StringContains(ospName, token.value)) tokenMatch = true;
                            // 通常検索でも中身(meshパス)を検索対象にするなら以下を有効化
                            // else { for(const auto& s : data.sets) if(StringContains(s.sourceNifPath, token.value)) { tokenMatch=true; break; } }
                        }
                        else if (token.type == SearchToken::Mesh) {
                            // mesh: 指定時は中身のパスやOutput名を検索
                            for (const auto& s : data.sets) {
                                if (StringContains(s.sourceNifPath, token.value)) { tokenMatch = true; break; }
                                if (StringContains(s.outputName, token.value)) { tokenMatch = true; break; }
                            }
                        }
                        if (!tokenMatch) return false; // AND条件不一致
                    }
                    return true;
                 };

                // Columns
                float footerH = ImGui::GetFrameHeightWithSpacing() + 10;
                ImGui::Columns(2, "OspCols", true);

                ImGui::BeginChild("OspList", ImVec2(0, -footerH), true);
                for (const auto& [name, ospData] : g_OspFiles) {
                    //if (!ospFilter.PassFilter(name.c_str())) continue;
                    if (!OspMatches(name, ospData)) continue;
                    // ブロックリストに含まれているかチェックして、あればスキップ
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
                        // ★#2: 二重起動防止（起動側で g_IsProcessing を立ててから detach）。
                        if (g_IsProcessing.load()) AddLog("Busy - please wait for the current operation to finish.", LogType::Warning);
                        else { g_IsProcessing = true; std::thread(ExportOSPWorker).detach(); }
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