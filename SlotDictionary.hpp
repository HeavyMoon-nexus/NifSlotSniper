#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>

// ルール構造体
struct SlotRule {
    std::string keyword;
    int slotID;
    int score;
    bool enabled = true;
};
// コンビネーションルール構造体
struct SlotComboRule {
    std::vector<std::string> requiredKeywords; // すべて含まれている必要があるキーワード
    int slotID;
    int bonusScore;
    bool enabled = true;
    std::string description; // デバッグ・ログ用
};
struct MatchReason {
    std::string type;    // "NAME", "BONE", "COMBO"
    std::string match;   // マッチしたキーワードやボーン名
    int slotID;
    int score;
};

struct DetailedAnalysis {
    std::vector<std::pair<int, int>> topSlots; // {slot, totalScore}
    std::vector<MatchReason> reasons;          // 判定根拠のリスト
};


class SlotDictionary {
public:
    static DetailedAnalysis AnalyzeDetailed(const std::vector<std::string>& boneList, const std::string& meshName, const std::string& editorID) {
        DetailedAnalysis result;
        std::map<int, int> scores;
        std::string meshLower = ToLower(meshName);
        std::string eidLower = ToLower(editorID);

        // 1. Name Rules の判定
        for (const auto& rule : nameRules) {
            if (!rule.enabled) continue;
            std::string kw = ToLower(rule.keyword);
            if (Contains(eidLower, kw) || Contains(meshLower, kw)) {
                scores[rule.slotID] += rule.score;
                result.reasons.push_back({ "NAME", rule.keyword, rule.slotID, rule.score });
            }
        }

        // 2. Bone Rules の判定
        for (const auto& bone : boneList) {
            for (const auto& rule : boneRules) {
                if (!rule.enabled) continue;
                if (Contains(bone, rule.keyword)) {
                    scores[rule.slotID] += rule.score;
                    result.reasons.push_back({ "BONE", bone + " (" + rule.keyword + ")", rule.slotID, rule.score });
                }
            }
        }

        // 3. Combo Rules の判定
        for (const auto& rule : comboRules) {
            if (!rule.enabled) continue;
            bool allMatch = true;
            for (const auto& kw : rule.requiredKeywords) {
                bool found = false;
                for (const auto& b : boneList) { if (Contains(b, kw)) { found = true; break; } }
                if (!found) { allMatch = false; break; }
            }
            if (allMatch) {
                scores[rule.slotID] += rule.bonusScore;
                result.reasons.push_back({ "COMBO", "All keywords match", rule.slotID, rule.bonusScore });
            }
        }

        // スコア順にソートして上位3つを格納
        for (const auto& [slot, score] : scores) if (score >= 5) result.topSlots.push_back({ slot, score });
        std::sort(result.topSlots.begin(), result.topSlots.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (result.topSlots.size() > 3) result.topSlots.resize(3);

        return result;
}
public:
    static std::vector<SlotRule> nameRules;
    static std::vector<SlotRule> boneRules;
    static std::vector<SlotComboRule> comboRules;

    // スロット定義マップ (ID -> 名前)
    static std::map<int, std::string> slotMap;

    // 設定読み込み
    static void LoadRules(const std::string& filename = "rules.ini") {
        // --- ルール読み込み ---
        nameRules.clear();
        boneRules.clear();
        comboRules.clear();

        std::ifstream file(filename);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == ';') continue;
                auto parts = Split(line, '|');
                if (parts.size() >= 4) {
                    if (parts[0] == "NAME") {
                        nameRules.push_back({ parts[1], std::stoi(parts[2]), std::stoi(parts[3]) });
                    }
                    else if (parts[0] == "BONE") {
                        boneRules.push_back({ parts[1], std::stoi(parts[2]), std::stoi(parts[3]) });
                    }
                    // ★追加: COMBO|L Foot,R Foot|37|100 の形式を解析
                    else if (parts[0] == "COMBO") {
                        SlotComboRule cRule;
                        cRule.requiredKeywords = Split(parts[1], ','); // カンマでボーン名を分離
                        cRule.slotID = std::stoi(parts[2]);
                        cRule.bonusScore = std::stoi(parts[3]);
                        comboRules.push_back(cRule);
                    }
                }
            }
        }
        if (nameRules.empty() && boneRules.empty() && comboRules.empty()) {
            InitDefaultRules();
            SaveRules(filename);
        }

        // --- スロット定義読み込み (config.ini から) ---
        // ※Main.cppのLoadUnifiedConfigと役割が被らないよう、
        //   ここではデフォルト値の初期化のみ行い、
        //   実際の読み込みはMain.cppのLoadUnifiedConfigで
        //   SlotDictionary::slotMap に値をセットする形を取ります。
        if (slotMap.empty()) {
            InitDefaultSlots();
        }
    }
    static void InitDefaultComboRules() {
        comboRules = {
            {{"L Foot", "R Foot"}, 37, 100}, // 両足があれば Feet
            {{"L Hand", "R Hand"}, 33, 100}, // 両手があれば Hands
            {{"L Calf", "R Calf"}, 38, 80},  // 両ふくらはぎがあれば Calves
            {{"Breast", "Breast"}, 57, 60}   // 胸ボーンが複数あれば Underwear Top
        };
    }

    // 設定保存
    static void SaveRules(const std::string& filename = "rules.ini") {
        std::ofstream file(filename);
        if (file.is_open()) {
            file << "; Format: TYPE|Keyword(s)|Slot|Score\n";
            file << "; For COMBO, separate keywords with commas (e.g., L Foot,R Foot)\n\n";

            for (const auto& r : nameRules) file << "NAME|" << r.keyword << "|" << r.slotID << "|" << r.score << "\n";
            file << "\n";
            for (const auto& r : boneRules) file << "BONE|" << r.keyword << "|" << r.slotID << "|" << r.score << "\n";

            // ★追加: COMBOルールの書き出し
            file << "\n";
            for (const auto& c : comboRules) {
                file << "COMBO|";
                for (size_t i = 0; i < c.requiredKeywords.size(); ++i) {
                    file << c.requiredKeywords[i] << (i == c.requiredKeywords.size() - 1 ? "" : ",");
                }
                file << "|" << c.slotID << "|" << c.bonusScore << "\n";
            }
        }
    }

    // ★変更: マップから名前を取得
    static std::string GetSlotName(int slotID) {
        if (slotMap.count(slotID)) {
            return slotMap[slotID];
        }
        return "Slot " + std::to_string(slotID);
    }

    // 提案ロジック
    static int SuggestSlot(const std::vector<std::string>& boneList, const std::string& meshName, const std::string& editorID) {
        std::map<int, int> scores;
        std::string meshLower = ToLower(meshName);
        std::string eidLower = ToLower(editorID);

        for (const auto& rule : nameRules) {
            if (!rule.enabled) continue;
            std::string kw = ToLower(rule.keyword);
            if (Contains(eidLower, kw) || Contains(meshLower, kw)) scores[rule.slotID] += rule.score;
        }

        for (const auto& bone : boneList) {
            for (const auto& rule : boneRules) {
                if (!rule.enabled) continue;
                if (Contains(bone, rule.keyword)) scores[rule.slotID] += rule.score;
            }
        }

        int bestSlot = -1;
        int maxScore = 0;
        for (const auto& [slot, score] : scores) {
            if (score > maxScore) {
                maxScore = score;
                bestSlot = slot;
            }
        }
        if (maxScore < 5) return -1;
        return bestSlot;
    }

    // ★追加: 上位3件の候補を提案する
    static std::vector<std::pair<int, int>> SuggestTopSlots(const std::vector<std::string>& boneList, const std::string& meshName, const std::string& editorID) {
        std::map<int, int> scores;
        std::string meshLower = ToLower(meshName);
        std::string eidLower = ToLower(editorID);

        for (const auto& rule : nameRules) {
            if (!rule.enabled) continue;
            std::string kw = ToLower(rule.keyword);
            if (Contains(eidLower, kw) || Contains(meshLower, kw)) scores[rule.slotID] += rule.score;
        }

        for (const auto& bone : boneList) {
            for (const auto& rule : boneRules) {
                if (!rule.enabled) continue;
                if (Contains(bone, rule.keyword)) scores[rule.slotID] += rule.score;
            }
        }
        // ★追加: コンビネーションルールの判定
        for (const auto& rule : comboRules) {
            if (!rule.enabled) continue;

            bool allMatch = true;
            for (const auto& kw : rule.requiredKeywords) {
                bool found = false;
                for (const auto& bone : boneList) {
                    if (Contains(bone, kw)) {
                        found = true;
                        break;
                    }
                }
                if (!found) { allMatch = false; break; }
            }

            if (allMatch) {
                scores[rule.slotID] += rule.bonusScore;
            }
        }
        // スコア順にソートして上位3つを返す
        std::vector<std::pair<int, int>> ranked;
        for (const auto& [slot, score] : scores) {
            if (score >= 5) ranked.push_back({ slot, score });
        }
        std::sort(ranked.begin(), ranked.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            return a.second > b.second;
            });

        if (ranked.size() > 3) ranked.resize(3);
        return ranked;
    }

    // ★追加: デフォルトスロット定義
    static void InitDefaultSlots() {
        slotMap.clear();
        slotMap[30] = "Head"; slotMap[31] = "Hair"; slotMap[32] = "Body";
        slotMap[33] = "Hands"; slotMap[34] = "Forearms"; slotMap[35] = "Amulet";
        slotMap[36] = "Ring"; slotMap[37] = "Feet"; slotMap[38] = "Calves";
        slotMap[39] = "Shield"; slotMap[40] = "Tail"; slotMap[41] = "LongHair";
        slotMap[42] = "Circlet"; slotMap[43] = "Ears"; slotMap[44] = "Face/Mouth";
        slotMap[45] = "Neck"; slotMap[46] = "Chest/Cape"; slotMap[47] = "Back";
        slotMap[48] = "Misc/Bowl"; slotMap[49] = "Pelvis/Secondary";
        slotMap[50] = "Decapitated Head"; slotMap[51] = "Decapitated";
        slotMap[52] = "Pelvis/Skirt"; slotMap[53] = "Leg/Secondary";
        slotMap[54] = "Leg/Tertiary"; slotMap[55] = "Loincloth";
        slotMap[56] = "Underwear/Bottom"; slotMap[57] = "Underwear/Top";
        slotMap[58] = "Pauldrons"; slotMap[59] = "Shield/Back";
        slotMap[60] = "Misc"; slotMap[61] = "FX01";
    }

private:
    static std::string ToLower(const std::string& str) {
        std::string s = str;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }
    static bool Contains(const std::string& target, const std::string& key) {
        std::string t = ToLower(target);
        std::string k = ToLower(key);
        return t.find(k) != std::string::npos;
    }
    static std::vector<std::string> Split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) tokens.push_back(token);
        return tokens;
    }

    static void InitDefaultRules() {
        // (前回と同じ内容。長いので省略します。前回のコードのまま保存されています)
        nameRules = {
            {"skirt", 52, 50}, {"cape", 46, 50}, {"cloak", 46, 50},
            {"earring", 43, 50}, {"glasses", 44, 50}, {"mask", 44, 50},
            {"choker", 35, 50}, {"necklace", 35, 50}, {"ring", 36, 50},
            {"helmet", 30, 30}, {"hood", 31, 30}, {"circlet", 42, 50},
            {"gloves", 33, 30}, {"gauntlets", 33, 30}, {"boots", 37, 30},
            {"shoes", 37, 30}, {"cuirass", 32, 20}, {"armor", 32, 20},
            {"panty", 56, 50}, {"underwear", 56, 50}, {"bra", 57, 50},
            {"bikini", 57, 50}
        };
        boneRules = {
            {"Head", 30, 10}, {"Hair", 31, 15}, {"Spine", 32, 5},
            {"Pelvis", 32, 5}, {"Hand", 33, 15}, {"Forearm", 34, 10},
            {"Foot", 37, 15}, {"Calf", 38, 10}, {"Clavicle", 32, 3},
            {"Breast", 57, 10}, {"Boob", 57, 10}
        };
    }
};

// 実体定義
inline std::vector<SlotRule> SlotDictionary::nameRules;
inline std::vector<SlotRule> SlotDictionary::boneRules;
inline std::vector<SlotComboRule> SlotDictionary::comboRules; // ★追加
inline std::map<int, std::string> SlotDictionary::slotMap;