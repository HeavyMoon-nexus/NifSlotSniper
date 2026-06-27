#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>

#include <NifFile.hpp>
#include <Skin.hpp>

class BoneAnalyzer {
public:
    struct AnalysisResult {
        std::string meshName;
        std::vector<std::string> influentialBones;            // ウェイト降順・ノイズ除去済み
        std::vector<std::pair<std::string, float>> boneWeights; // ★⑤: {ボーン名, 合計ウェイト}（降順）
        std::string suggestedSlot;
        std::vector<std::pair<int, int>> suggestions; // ★追加: 上位候補 {slot, score}
    };

    // ★⑤: 最大ウェイトに対する相対閾値。最大の influence に対しこの割合未満の
    //      ボーンはノイズとみなして除外する（ボディに丸ごとスキンされた装備で
    //      Head/Hand/Foot 等が混入し提案を汚すのを防ぐ）。
    static constexpr float kRelWeightThreshold = 0.05f;

    // NIFファイル全体を解析し、メッシュごとに「実際にウェイトが乗っている」ボーンを返す
    static std::vector<AnalysisResult> AnalyzeNif(nifly::NifFile& nif) {
        std::vector<AnalysisResult> results;
        auto shapes = nif.GetShapes();

        for (auto& shape : shapes) {
            if (!shape) continue;

            AnalysisResult res;
            res.meshName = shape->name.get();

            // ★⑤: スキンインスタンスの種別（NiSkinInstance / BSDismember / BSTriShape）を
            //      問わず、nifly の API でボーン一覧と頂点ウェイトを取得する。
            std::vector<std::string> boneNames;
            nif.GetShapeBoneList(shape, boneNames);

            std::vector<std::pair<std::string, float>> infl; // {name, totalWeight}
            float maxW = 0.0f;
            for (uint32_t bi = 0; bi < boneNames.size(); ++bi) {
                if (!IsValidBone(boneNames[bi])) continue;

                std::unordered_map<uint16_t, float> weights;
                nif.GetShapeBoneWeights(shape, bi, weights);

                float sum = 0.0f;
                for (const auto& [vidx, w] : weights) sum += w;
                if (sum <= 0.0f) continue; // ウェイト0のボーンは無視

                infl.push_back({ boneNames[bi], sum });
                maxW = std::max(maxW, sum);
            }

            // ウェイト降順に並べ、相対閾値未満を切り捨てる
            std::sort(infl.begin(), infl.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });

            const float threshold = maxW * kRelWeightThreshold;
            for (const auto& [name, w] : infl) {
                if (w >= threshold) {
                    res.influentialBones.push_back(name);
                    res.boneWeights.push_back({ name, w });
                }
            }

            // ★⑤ フォールバック: ウェイト情報が取得できない（スキン無しや旧形式で
            //      読めない）場合は、従来どおりボーン一覧をそのまま採用する。
            if (res.influentialBones.empty()) {
                for (const auto& name : boneNames) {
                    if (IsValidBone(name)) res.influentialBones.push_back(name);
                }
                std::sort(res.influentialBones.begin(), res.influentialBones.end());
                res.influentialBones.erase(
                    std::unique(res.influentialBones.begin(), res.influentialBones.end()),
                    res.influentialBones.end());
            }

            results.push_back(std::move(res));
        }
        return results;
    }

private:
    // 解析に意味のないボーンを除外するフィルタ
    static bool IsValidBone(const std::string& name) {
        if (name.empty()) return false;
        if (name == "NPC Root [Root]") return false;
        if (name == "NPC COM [COM ]") return false;
        if (name.find("Camera") != std::string::npos) return false;
        return true;
    }
};