#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
#include <set>

#include <NifFile.hpp>
#include <Skin.hpp> 

class BoneAnalyzer {
public:
    struct AnalysisResult {
        std::string meshName;
        std::vector<std::string> influentialBones;
        std::string suggestedSlot;
        std::vector<std::pair<int, int>> suggestions; // ★追加: 上位候補 {slot, score}
    };

    // NIFファイル全体を解析し、メッシュごとのボーン情報を返す
    static std::vector<AnalysisResult> AnalyzeNif(nifly::NifFile& nif) {
        std::vector<AnalysisResult> results;
        auto shapes = nif.GetShapes();

        for (auto& shape : shapes) {
            if (!shape) continue;

            // すべてのNiShape系を対象にする
            AnalysisResult res;
            res.meshName = shape->name.get();

            // niflyの提供する SkinInstanceRef は NiShape 全般で利用可能
            auto skinRef = shape->SkinInstanceRef();
            if (skinRef && !skinRef->IsEmpty()) {
                auto skinObj = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);

                if (auto skinInst = dynamic_cast<nifly::NiSkinInstance*>(skinObj)) {
                    for (const auto& boneRef : skinInst->boneRefs) {
                        if (boneRef.IsEmpty()) continue;

                        auto boneNode = nif.GetHeader().GetBlock<nifly::NiNode>(boneRef.index);
                        if (boneNode) {
                            std::string bName = boneNode->name.get();
                            if (IsValidBone(bName)) {
                                res.influentialBones.push_back(bName);
                            }
                        }
                    }
                }
            }

            // 重複削除
            std::sort(res.influentialBones.begin(), res.influentialBones.end());
            res.influentialBones.erase(std::unique(res.influentialBones.begin(), res.influentialBones.end()), res.influentialBones.end());

            results.push_back(res);
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