#include "UI_Rules.h"
#include "Globals.h"
#include "SlotDictionary.hpp"
#include <imgui.h>
#include <string>
#include <vector>
#include <set>
#include <cstring> // strncpy_s, memset

// =========================================================================
// 外部関数の呼び出し準備
// =========================================================================
extern void SaveUnifiedConfig(); // main.cpp にある設定保存関数

// =========================================================================
// Rules Window の実装
// =========================================================================
void RenderRulesWindow() {
    // 表示フラグチェック
    if (!g_ShowRulesWindow) return;

    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);

    // ウィンドウ開始
    if (ImGui::Begin("Auto-Fix Rules & Definitions", &g_ShowRulesWindow)) {

        if (ImGui::BeginTabBar("RuleTabs")) {

            // --------------------------------------------------------
            // Tab 1: Auto-Fix Rules
            // --------------------------------------------------------
            if (ImGui::BeginTabItem("Auto-Fix Rules")) {
                static char newKw[128] = "";
                static int newSlot = 32;
                static int newScore = 50;
                static int newType = 0; // 0:Name, 1:Bone, 2:Combo

                ImGui::Separator();
                ImGui::Text("Add New Rule:");

                // 1. タイプ選択 (Comboを追加)
                ImGui::SetNextItemWidth(140);
                ImGui::Combo("##Type", &newType, "Target: Name\0Target: Bone\0Target: Combo\0");

                // 2. ヒントテキストをモードによって変える
                const char* hintText = "Search word (e.g. skirt)";
                if (newType == 2) hintText = "prefix=bone:,mesh:,name:";

                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputTextWithHint("##Kw", hintText, newKw, 128);

                ImGui::SameLine();
                ImGui::Text("has");

                ImGui::SameLine();
                ImGui::SetNextItemWidth(30);
                ImGui::InputInt("Slot##RuleS", &newSlot, 0); // ID重複回避のため ##RuleS に変更

                ImGui::SameLine();
                ImGui::SetNextItemWidth(30);
                ImGui::InputInt("Score##RuleC", &newScore, 0);

                ImGui::SameLine();
                if (ImGui::Button("Add")) {
                    if (strlen(newKw) > 0) {
                        // --- Combo Rule の場合 ---
                        if (newType == 2) {
                            SlotComboRule cr;
                            // カンマ区切りで分割して登録
                            // (SplitStringはUtilsあるいは以前のコードにある前提)
                            cr.requiredKeywords = SplitString(newKw, ',');

                            // 前後の空白除去処理
                            for (auto& s : cr.requiredKeywords) {
                                s.erase(0, s.find_first_not_of(" "));
                                size_t last = s.find_last_not_of(" ");
                                if (last != std::string::npos) s.erase(last + 1);
                            }

                            cr.slotID = newSlot;
                            cr.bonusScore = newScore;
                            SlotDictionary::comboRules.push_back(cr);
                        }
                        // --- Name / Bone Rule の場合 ---
                        else {
                            SlotRule r;
                            r.keyword = newKw;
                            r.slotID = newSlot;
                            r.score = newScore;

                            if (newType == 0) SlotDictionary::nameRules.push_back(r);
                            else SlotDictionary::boneRules.push_back(r);
                        }

                        // 入力欄クリア
                        memset(newKw, 0, 128);
                    }
                }

                ImGui::Separator();
                ImGui::BeginChild("RulesList", ImVec2(0, 300), true);

                auto DrawRules = [&](std::vector<SlotRule>& R, const char* T) {
                    if (ImGui::TreeNode(T)) {
                        for (int i = 0; i < R.size(); ++i) {
                            ImGui::PushID(i);
                            if (ImGui::Button("X")) {
                                R.erase(R.begin() + i);
                                i--;
                                ImGui::PopID();
                                continue;
                            }
                            ImGui::SameLine();
                            std::string label = R[i].keyword + " -> Slot " + std::to_string(R[i].slotID) + " (Score: " + std::to_string(R[i].score) + ")";
                            ImGui::Selectable(label.c_str(), false);

                            if (ImGui::IsItemClicked(1)) {
                                strncpy_s(newKw, R[i].keyword.c_str(), _TRUNCATE);
                                newSlot = R[i].slotID;
                                newScore = R[i].score;
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Right Click to Edit (Copy to top form)");
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    };

                DrawRules(SlotDictionary::nameRules, "Name Rules");
                DrawRules(SlotDictionary::boneRules, "Bone Rules");

                if (ImGui::TreeNode("Combination Rules")) {
                    for (int i = 0; i < SlotDictionary::comboRules.size(); ++i) {
                        auto& cr = SlotDictionary::comboRules[i];
                        ImGui::PushID(i + 1000);
                        if (ImGui::Button("X")) {
                            SlotDictionary::comboRules.erase(SlotDictionary::comboRules.begin() + i);
                            i--;
                            ImGui::PopID();
                            continue;
                        }
                        ImGui::SameLine();
                        std::string displayStr;
                        for (size_t k = 0; k < cr.requiredKeywords.size(); k++) {
                            displayStr += cr.requiredKeywords[k] + (k == cr.requiredKeywords.size() - 1 ? "" : " + ");
                        }
                        displayStr += " -> Slot " + std::to_string(cr.slotID) + " (Bonus: " + std::to_string(cr.bonusScore) + ")";
                        ImGui::Selectable(displayStr.c_str(), false);

                        if (ImGui::IsItemClicked(1)) {
                            std::string kwJoined;
                            for (size_t k = 0; k < cr.requiredKeywords.size(); ++k) {
                                kwJoined += cr.requiredKeywords[k] + (k == cr.requiredKeywords.size() - 1 ? "" : ", ");
                            }
                            strncpy_s(newKw, kwJoined.c_str(), _TRUNCATE);
                            newSlot = cr.slotID;
                            newScore = cr.bonusScore;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Right Click to Edit");
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // --------------------------------------------------------
            // Tab 2: Slot Definitions
            // --------------------------------------------------------
            if (ImGui::BeginTabItem("Slot Definitions")) {
                static int defID = 32;
                static char defName[64] = "";

                std::set<int> usedInNif;
                for (const auto& mesh : g_RenderMeshes) {
                    for (int slotID : mesh.activeSlots) usedInNif.insert(slotID);
                }

                ImGui::Text("Add or Change Definition:");
                ImGui::SetNextItemWidth(40);
                ImGui::InputInt("Slots is", &defID, 0);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputTextWithHint("##DefName", "Slot Name (e.g. Body)", defName, 64);
                ImGui::SameLine();
                if (ImGui::Button("Add/Change")) {
                    if (strlen(defName) > 0) SlotDictionary::slotMap[defID] = defName;
                }

                ImGui::Separator();
                ImGui::Text("Defined Slots:");
                ImGui::BeginChild("SlotList", ImVec2(0, 300), true);

                int rm = -1;
                for (const auto& [id, n] : SlotDictionary::slotMap) {
                    ImGui::PushID(id);
                    bool isUsed = usedInNif.count(id) > 0;
                    if (ImGui::Button("X")) rm = id;
                    ImGui::SameLine();

                    std::string label = std::to_string(id) + ": " + n + (isUsed ? " (*Used)" : "");
                    ImGui::Selectable(label.c_str(), false);

                    if (ImGui::IsItemClicked(1)) {
                        defID = id;
                        strncpy_s(defName, n.c_str(), _TRUNCATE);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Right-click to copy this to edit form");

                    if (isUsed) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%d: %s (*Used in NIF)", id, n.c_str());
                    ImGui::PopID();
                }
                if (rm != -1) SlotDictionary::slotMap.erase(rm);

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // --------------------------------------------------------
            // Tab 3: KID Keywords List
            // --------------------------------------------------------
            if (ImGui::BeginTabItem("KID Keywords List")) {
                static char kidKw[64] = "";
                static char kidSlotsStr[64] = "";
                static char kidMatchStr[128] = "";

                bool formHasData = (strlen(kidKw) > 0 || strlen(kidSlotsStr) > 0 || strlen(kidMatchStr) > 0);

                ImGui::SetNextItemWidth(150);
                ImGui::InputTextWithHint("##keyeords", "Keyword", kidKw, 64);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(50);
                ImGui::InputTextWithHint("Slots", "32, 52", kidSlotsStr, 64);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::InputTextWithHint("Words", "e.g. skirt, mini", kidMatchStr, 128);
                ImGui::SameLine();

                if (ImGui::Button("Add/Change") && strlen(kidKw) > 0) {
                    std::vector<std::string> mWords;
                    if (strlen(kidMatchStr) > 0) {
                        mWords = SplitString(kidMatchStr, ',');
                        for (auto& w : mWords) {
                            w.erase(0, w.find_first_not_of(" "));
                            size_t last = w.find_last_not_of(" ");
                            if (last != std::string::npos) w.erase(last + 1);
                        }
                    }
                    g_KeywordList.push_back({ kidKw, ParseSlotString(kidSlotsStr), mWords });
                    memset(kidKw, 0, 64);
                    memset(kidSlotsStr, 0, 64);
                    memset(kidMatchStr, 0, 128);
                }

                ImGui::SameLine();
                if (ImGui::Button("Clear Form")) {
                    memset(kidKw, 0, 64);
                    memset(kidSlotsStr, 0, 64);
                    memset(kidMatchStr, 0, 128);
                }
                ShowTooltip("Clears the input fields without adding.");

                ImGui::BeginChild("KIDList", ImVec2(0, 300), true);
                for (int i = 0; i < g_KeywordList.size(); ++i) {
                    auto& item = g_KeywordList[i];
                    ImGui::PushID(i);

                    if (ImGui::Button("X")) {
                        g_KeywordList.erase(g_KeywordList.begin() + i);
                        i--;
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::SameLine();

                    std::string label = item.keyword;
                    if (!item.targetSlots.empty()) {
                        label += " [Slots:";
                        for (size_t k = 0; k < item.targetSlots.size(); ++k) label += (k > 0 ? "," : "") + std::to_string(item.targetSlots[k]);
                        label += "]";
                    }
                    if (!item.matchWords.empty()) {
                        label += " [Match:";
                        for (size_t k = 0; k < item.matchWords.size(); ++k) label += (k > 0 ? "," : "") + item.matchWords[k];
                        label += "]";
                    }

                    ImGui::Selectable(label.c_str(), false);

                    if (ImGui::IsItemClicked(1)) {
                        if (formHasData) {
                            AddLog("Pending changes in form! Press 'Add' or 'Clear Form' before editing another item.", LogType::Warning);
                        }
                        else {
                            strncpy_s(kidKw, item.keyword.c_str(), _TRUNCATE);

                            std::string sStr = "";
                            for (size_t k = 0; k < item.targetSlots.size(); ++k) sStr += (k > 0 ? "," : "") + std::to_string(item.targetSlots[k]);
                            strncpy_s(kidSlotsStr, sStr.c_str(), _TRUNCATE);

                            std::string mStr = "";
                            for (size_t k = 0; k < item.matchWords.size(); ++k) mStr += (k > 0 ? "," : "") + item.matchWords[k];
                            strncpy_s(kidMatchStr, mStr.c_str(), _TRUNCATE);

                            g_KeywordList.erase(g_KeywordList.begin() + i);
                            i--;
                            AddLog("Moved to form: " + item.keyword, LogType::Info);
                        }
                    }

                    if (ImGui::IsItemHovered()) {
                        if (formHasData) ImGui::SetTooltip("!! Finish editing (Add/Change/Clear) before editing this item");
                        else ImGui::SetTooltip("Right Click to Edit (Move to top form)");
                    }

                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Separator();
        // 保存ボタン
        if (ImGui::Button("Save All Rules", ImVec2(-1, 30))) {
            SaveUnifiedConfig();
            SlotDictionary::SaveRules();
        }

    } // <--- if (Begin) の終わり

    ImGui::End();
}