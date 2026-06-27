#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>

// ���[���\����
struct SlotRule {
    std::string keyword;
    int slotID = 0;   // ★C26495: 既定値初期化（未初期化メンバの読み出し防止）
    int score = 0;
    bool enabled = true;
};
// �R���r�l�[�V�������[���\����
struct SlotComboRule {
    std::vector<std::string> requiredKeywords; // ���ׂĊ܂܂�Ă���K�v������L�[���[�h
    int slotID = 0;      // ★C26495: 既定値初期化
    int bonusScore = 0;  // ★C26495: 既定値初期化
    bool enabled = true;
    std::string description; // �f�o�b�O�E���O�p
};

struct MatchReason {
    std::string type;    // "NAME", "BONE", "COMBO"
    std::string match;   // �}�b�`�����L�[���[�h��{�[����
    int slotID = 0;      // ★C26495: 既定値初期化
    int score = 0;
};

struct DetailedAnalysis {
    std::vector<std::pair<int, int>> topSlots; // {slot, totalScore}
    std::vector<MatchReason> reasons;          // ���荪���̃��X�g
};

class SlotDictionary {
public:
    // 詳細解析: スコアと、その根拠(reasons)を返す
    // ★⑥: スコア計算は ComputeScores / RankTopSlots に集約（3重実装を解消）
    static DetailedAnalysis AnalyzeDetailed(const std::vector<std::string>& boneList, const std::string& meshName, const std::string& editorID) {
        DetailedAnalysis result;
        std::map<int, int> scores = ComputeScores(boneList, meshName, editorID, &result.reasons);
        result.topSlots = RankTopSlots(scores);
        return result;
    }
public:
    static std::vector<SlotRule> nameRules;
    static std::vector<SlotRule> boneRules;
    static std::vector<SlotComboRule> comboRules;

    // �X���b�g��`�}�b�v (ID -> ���O)
    static std::map<int, std::string> slotMap;

    // �ݒ�ǂݍ���
    static void LoadRules(const std::string& filename = "rules.ini") {
        // --- ���[���ǂݍ��� ---
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
                    // ���ǉ�: COMBO|L Foot,R Foot|37|100 �̌`�������
                    else if (parts[0] == "COMBO") {
                        SlotComboRule cRule;
                        cRule.requiredKeywords = Split(parts[1], ','); // �J���}�Ń{�[�����𕪗�
                        cRule.slotID = std::stoi(parts[2]);
                        cRule.bonusScore = std::stoi(parts[3]);
                        comboRules.push_back(cRule);
                    }
                }
            }
        }
        if (nameRules.empty() && boneRules.empty() && comboRules.empty()) {
            // ★修正: 初回はコンボもデフォルト初期化して rules.ini に永続化する
            InitDefaultRules();
            InitDefaultComboRules();
            SaveRules(filename);
        }
        else if (comboRules.empty()) {
            // ★修正: 既存の rules.ini に COMBO 行が無い場合のみデフォルトを補う
            // （ユーザー定義の COMBO 行があればそれを尊重し、上書きしない）
            InitDefaultComboRules();
        }

        // --- �X���b�g��`�ǂݍ��� (config.ini ����) ---
        // ��Main.cpp��LoadUnifiedConfig�Ɩ��������Ȃ��悤�A
        //   �����ł̓f�t�H���g�l�̏������̂ݍs���A
        //   ���ۂ̓ǂݍ��݂�Main.cpp��LoadUnifiedConfig��
        //   SlotDictionary::slotMap �ɒl���Z�b�g����`�����܂��B
        if (slotMap.empty()) {
            InitDefaultSlots();
        }
    }
    static void InitDefaultComboRules() {
        comboRules = {
            {{"L Foot", "R Foot"}, 37, 100}, // ����������� Feet
            {{"L Hand", "R Hand"}, 33, 100}, // ���肪����� Hands
            {{"L Calf", "R Calf"}, 38, 80},  // ���ӂ���͂�������� Calves
            // ★修正: 同一キーワード2回では意味が無いため、左右の胸ボーン両方を要求する形にする
            {{"bone:L Breast", "bone:R Breast"}, 57, 60}   // 左右の胸ボーンがあれば Underwear Top
        };
    }

    // �ݒ�ۑ�
    static void SaveRules(const std::string& filename = "rules.ini") {
        std::ofstream file(filename);
        if (file.is_open()) {
            file << "; Format: TYPE|Keyword(s)|Slot|Score\n";
            file << "; For COMBO, separate keywords with commas (e.g., L Foot,R Foot)\n\n";

            for (const auto& r : nameRules) file << "NAME|" << r.keyword << "|" << r.slotID << "|" << r.score << "\n";
            file << "\n";
            for (const auto& r : boneRules) file << "BONE|" << r.keyword << "|" << r.slotID << "|" << r.score << "\n";

            // ���ǉ�: COMBO���[���̏����o��
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

    // ���ύX: �}�b�v���疼�O���擾
    static std::string GetSlotName(int slotID) {
        if (slotMap.count(slotID)) {
            return slotMap[slotID];
        }
        return "Slot " + std::to_string(slotID);
    }

    // ��ă��W�b�N
    // 最有力スロットを1つ返す（該当なしは -1）
    static int SuggestSlot(const std::vector<std::string>& boneList, const std::string& meshName, const std::string& editorID) {
        std::vector<std::pair<int, int>> ranked = RankTopSlots(ComputeScores(boneList, meshName, editorID));
        return ranked.empty() ? -1 : ranked.front().first;
    }

    // ���ǉ�: ���3���̌����Ă���
    // 上位スロット候補（最大 kMaxSuggestions 件）を返す
    static std::vector<std::pair<int, int>> SuggestTopSlots(const std::vector<std::string>& boneList, const std::string& meshName, const std::string& editorID) {
        return RankTopSlots(ComputeScores(boneList, meshName, editorID));
    }

    // ���ǉ�: �f�t�H���g�X���b�g��`
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
    // ====== ★⑥ スコアリング共通処理（AnalyzeDetailed / SuggestSlot / SuggestTopSlots の集約先） ======
    static constexpr int    kMinSuggestScore = 5;   // 提案として採用する最小スコア
    static constexpr size_t kMaxSuggestions  = 3;   // 提案候補の最大件数

    // name/bone/combo ルールを評価し、スロット別スコアを返す。
    // outReasons が非 null の場合はマッチ根拠を記録する（Analysis Details 表示用）。
    static std::map<int, int> ComputeScores(
        const std::vector<std::string>& boneList,
        const std::string& meshName,
        const std::string& editorID,
        std::vector<MatchReason>* outReasons = nullptr)
    {
        std::map<int, int> scores;
        std::string meshLower = ToLower(meshName);
        std::string eidLower = ToLower(editorID);

        // 1. Name ルール: EditorID / メッシュ名にキーワードが含まれるか
        for (const auto& rule : nameRules) {
            if (!rule.enabled) continue;
            std::string kw = ToLower(rule.keyword);
            if (Contains(eidLower, kw) || Contains(meshLower, kw)) {
                scores[rule.slotID] += rule.score;
                if (outReasons) outReasons->push_back({ "NAME", rule.keyword, rule.slotID, rule.score });
            }
        }

        // 2. Bone ルール: 影響ボーン名にキーワードが含まれるか
        for (const auto& bone : boneList) {
            for (const auto& rule : boneRules) {
                if (!rule.enabled) continue;
                if (Contains(bone, rule.keyword)) {
                    scores[rule.slotID] += rule.score;
                    if (outReasons) outReasons->push_back({ "BONE", bone + " (" + rule.keyword + ")", rule.slotID, rule.score });
                }
            }
        }

        // 3. Combo ルール: 必須キーワードがすべて揃ったか
        for (const auto& rule : comboRules) {
            if (!rule.enabled) continue;
            std::vector<std::string> matchedKeywords;
            bool allMatch = true;
            for (const auto& kw : rule.requiredKeywords) {
                if (ComboKeywordMatches(kw, boneList, meshLower, eidLower)) {
                    matchedKeywords.push_back(kw);
                    continue;
                }
                allMatch = false;
                break;
            }
            if (allMatch) {
                scores[rule.slotID] += rule.bonusScore;
                if (outReasons) {
                    std::string match = matchedKeywords.empty()
                        ? std::string("All keywords match")
                        : ("Match: " + std::accumulate(
                            std::next(matchedKeywords.begin()), matchedKeywords.end(), matchedKeywords.front(),
                            [](std::string acc, const std::string& s) { return acc + ", " + s; }));
                    outReasons->push_back({ "COMBO", match, rule.slotID, rule.bonusScore });
                }
            }
        }
        return scores;
    }

    // スコアマップを降順ソートし、閾値以上を最大 kMaxSuggestions 件に絞る
    static std::vector<std::pair<int, int>> RankTopSlots(const std::map<int, int>& scores) {
        std::vector<std::pair<int, int>> ranked;
        for (const auto& [slot, score] : scores)
            if (score >= kMinSuggestScore) ranked.push_back({ slot, score });
        std::sort(ranked.begin(), ranked.end(),
            [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.second > b.second; });
        if (ranked.size() > kMaxSuggestions) ranked.resize(kMaxSuggestions);
        return ranked;
    }

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
    static bool HasBoneKeyword(const std::vector<std::string>& bones, const std::string& keyword) {
        for (const auto& bone : bones) {
            if (Contains(bone, keyword)) return true;
        }
        return false;
    }
    static bool ComboKeywordMatches(const std::string& rawKeyword,
        const std::vector<std::string>& bones,
        const std::string& meshLower,
        const std::string& eidLower) {

        auto trim = [](std::string s) {
            auto isSpace = [](unsigned char c) { return std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !isSpace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), s.end());
            return s;
            };

        std::string keyword = ToLower(trim(rawKeyword));
        if (keyword.empty()) return false;

        auto matchesBone = [&](const std::string& k) { return HasBoneKeyword(bones, k); };
        auto matchesMesh = [&](const std::string& k) { return Contains(meshLower, k) || Contains(eidLower, k); };

        if (keyword.rfind("bone:", 0) == 0)  return matchesBone(keyword.substr(5));
        if (keyword.rfind("mesh:", 0) == 0)  return matchesMesh(keyword.substr(5));
        if (keyword.rfind("name:", 0) == 0)  return matchesMesh(keyword.substr(5));

        // �f�t�H���g: �{�[�� �� ���b�V����/EditorID �̏��Ŕ���
        return matchesBone(keyword) || matchesMesh(keyword);
    }
    static std::vector<std::string> Split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) tokens.push_back(token);
        return tokens;
    }

    static void InitDefaultRules() {
        // (�O��Ɠ������e�B�����̂ŏȗ����܂��B�O��̃R�[�h�̂܂ܕۑ�����Ă��܂�)
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

// ���̒�`
inline std::vector<SlotRule> SlotDictionary::nameRules;
inline std::vector<SlotRule> SlotDictionary::boneRules;
inline std::vector<SlotComboRule> SlotDictionary::comboRules; // ���ǉ�
inline std::map<int, std::string> SlotDictionary::slotMap;