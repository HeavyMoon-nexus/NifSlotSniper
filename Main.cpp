// ============================================================
// NIF Slot Sniper v1.0.0
// ============================================================

#pragma warning(disable: 26454) 

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <map> 
#include <cctype> 
#include <fstream> 
#include <set> 

#include "tinyxml2.h"

#ifdef _WIN32
#define NOMINMAX
#undef APIENTRY
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h> 
#include <shlobj.h> 

std::string OpenFileDialog(const char* filter);
std::string SelectFolderDialog();
#endif

// fs は Globals.h でも使っているので、Globals.h の後だと重複定義エラーになる可能性があります。
// Globals.h 内で namespace fs = std::filesystem; しているので、
// ここでは消すか、そのままにするなら Globals.h の方を調整します。
// 今回は Globals.h に任せるので削除してもOKですが、念のため残すならそのままでも動くことが多いです。

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <NifFile.hpp>
#include <Skin.hpp> 

#include "BoneAnalyzer.hpp"
#include "SlotDictionary.hpp"

// ★ここが変更点：Globals.h と　UI_Settings.hを読み込む
#include "Globals.h"
#include "UI_Settings.h"
#include "UI_ControlPanel.h"
#include "UI_PendingArea.h"
#include "UI_Database.h"
#include "UI_KidGenerator.h"
#include "UI_Rules.h"
#include "UI_AnalysisDetails.h"
#include "nlohmann/json.hpp"
using json = nlohmann::json;
const char* KEYWORDS_JSON_FILE = "keywords.json";
//#include "globals.cpp"//消し忘れ？要確認
//#include "OSP_Logic.h"// ここではGlobals.cppをインクルードしない。main.cppに直書きならコメントアウトを外す

// 関数プロトタイプ（前方宣言）は残しておく
std::vector<std::string> SplitString(const std::string& s, char delimiter);
std::vector<int> ParseSlotString(const std::string& slotStr);

// ============================================================
// シェーダー
// ============================================================
const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
out vec3 Normal;
void main() {
    gl_Position = projection * view * model * vec4(aPos, 1.0);
    Normal = mat3(transpose(inverse(model))) * aNormal; 
}
)";

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
in vec3 Normal;
uniform vec3 lightDir;
uniform vec3 objectColor;
void main() {
    vec3 norm = normalize(Normal);
    if (!gl_FrontFacing) norm = -norm; 
    vec3 light = normalize(lightDir);
    float diff = max(dot(norm, light), 0.4);
    FragColor = vec4(objectColor * diff, 1.0);
}
)";



// ============================================================
// ヘルパー
// ============================================================

// fs::path のコンポーネントを走査して、特定のフォルダ名以降のパスを抽出するヘルパー
fs::path GetPathFromFolder(const fs::path& fullPath, const std::string& folderName) {
    // パスを構成要素（フォルダ単位）で走査
    auto it = std::find_if(fullPath.begin(), fullPath.end(), [&](const fs::path& p) {
        // 大文字小文字を無視して比較
        std::string s = p.string();
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s == folderName;
        });

    if (it != fullPath.end()) {
        // 見つかったフォルダ以降を結合して新しいパスを作る
        fs::path rel;
        for (; it != fullPath.end(); ++it) {
            rel /= *it;
        }
        return rel;
    }
    // 見つからなければファイル名だけを返す（またはそのまま返す）
    return fullPath.filename();
}




fs::path ConstructSafePath(const std::string& rootStr, const std::string& relStr) {
    // ルートの準備
    fs::path root(rootStr);
    if (root.empty() && strlen(g_GameDataPath) > 0) {
        root = fs::path(g_GameDataPath) / "meshes";
    }

    fs::path rel(relStr);

    // relStr が絶対パスなら、そのまま返す（または root からの相対化を試みる）
    if (rel.is_absolute()) return rel;

    // パスの先頭にある不要な区切り文字などをクリーニング
    // fs::path は自動的に正規化してくれますが、"meshes" の重複チェックを行う

    // root の末尾が "meshes" か？
    bool rootEndsMeshes = (!root.empty() && root.filename().string() == "meshes"); // 大文字小文字厳密比較ならこれ。緩くするならtolowerが必要

    // rel の先頭が "meshes" か？
    bool relStartsMeshes = (!rel.empty() && rel.begin()->string() == "meshes");

    if (rootEndsMeshes && relStartsMeshes) {
        // rel の先頭の "meshes" を取り除く (lexically_relative の応用)
        // rel = "meshes/armor/..." -> "armor/..."
        rel = rel.lexically_relative("meshes");
    }

    // 安全に結合 (operator/ が区切り文字を適切に処理します)
    return root / rel;
}

std::string GetBaseNifKey(const std::string& path) {
    fs::path p(path);
    std::string stem = p.stem().string();
    if (stem.length() >= 2) {
        std::string suffix = stem.substr(stem.length() - 2);
        if (suffix == "_0" || suffix == "_1") stem = stem.substr(0, stem.length() - 2);
    }
    return (p.parent_path() / (stem + p.extension().string())).string();
}
static std::string NormalizePathForComparison(const std::string& p) {
    std::string r = p;
    // trim 前後空白
    size_t start = r.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    size_t end = r.find_last_not_of(" \t\r\n");
    r = r.substr(start, end - start + 1);
    // 小文字化
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    // \ を / に統一
    std::replace(r.begin(), r.end(), '\\', '/');
    // 末尾のスラッシュ除去
    while (!r.empty() && r.back() == '/') r.pop_back();
    return r;
}

std::string GetRelativeMeshesPath(const fs::path& fullPath) {
    std::string pathStr = fullPath.string();
    std::string lowerPath = pathStr;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
    size_t pos = lowerPath.find("meshes");
    if (pos != std::string::npos) return pathStr.substr(pos);
    return pathStr;
}

fs::path FindFileInBodySlide(const std::string& filename) {
    // BodySlide の検索は g_GameDataPath を起点に行い、親フォルダ探索は不要になったため削除
    if (strlen(g_GameDataPath) == 0) return "";
    fs::path root(g_GameDataPath);
    fs::path searchRoot = root / "CalienteTools" / "BodySlide" / "ShapeData";

    if (!fs::exists(searchRoot)) return "";

    for (auto& p : fs::recursive_directory_iterator(searchRoot)) {
        if (p.is_regular_file()) {
            if (p.path().filename().string() == filename) {
                return p.path();
            }
        }
    }
    return "";
}


// --------------------------------------------------------------------------
// JSON 読み込み関数 (文字列形式の slots に対応)
// --------------------------------------------------------------------------
void LoadKeywordsJSON() {
    std::ifstream file(KEYWORDS_JSON_FILE);
    if (!file.is_open()) {
        // ファイルがない場合はデフォルト値を設定
        g_KeywordList = {
            { "AND_ChestFlashRisk", {}, {} },
            { "AND_ChestFlashRiskLow", {}, {} },
            { "AND_ChestFlashRiskHigh", {}, {} },
            { "AND_ChestFlashRiskExtreme", {}, {} }
        };
        AddLog("Keywords JSON not found. Created defaults.", LogType::Warning);
        return;
    }

    try {
        json j;
        file >> j;
        g_KeywordList.clear();

        for (const auto& item : j) {
            KidKeyword kd;

            // "keyword"
            if (item.contains("keyword") && !item["keyword"].is_null()) {
                kd.keyword = item["keyword"].get<std::string>();
            }

            // "slots" (文字列配列 ["32", "50"] として読み込み -> int変換)
            if (item.contains("slots") && !item["slots"].is_null()) {
                try {
                    // 文字列の配列として取得
                    std::vector<std::string> slotStrs = item["slots"].get<std::vector<std::string>>();
                    for (const auto& s : slotStrs) {
                        try {
                            kd.targetSlots.push_back(std::stoi(s));
                        }
                        catch (...) {} // 数値変換できないものはスキップ
                    }
                }
                catch (...) {
                    // 万が一、古い形式(数値配列)だった場合のフォールバックを入れるならここ
                    // ですが、ご要望により「""で囲ってないとダメ」にするため、あえて入れません
                    AddLog("Error parsing slots for keyword: " + kd.keyword, LogType::Warning);
                }
            }

            // "match_words"
            if (item.contains("match_words") && !item["match_words"].is_null()) {
                kd.matchWords = item["match_words"].get<std::vector<std::string>>();
            }

            g_KeywordList.push_back(kd);
        }
        AddLog("Loaded keywords from JSON.", LogType::Info);
    }
    catch (const std::exception& e) {
        AddLog("JSON Load Error: " + std::string(e.what()), LogType::Error);
    }
}

// --------------------------------------------------------------------------
// JSON 保存関数 (文字列形式 ["32"] で保存)
// --------------------------------------------------------------------------
void SaveKeywordsJSON() {
    try {
        json j = json::array();
        for (const auto& k : g_KeywordList) {

            // int配列を string配列に変換
            std::vector<std::string> slotStrs;
            for (int s : k.targetSlots) {
                slotStrs.push_back(std::to_string(s));
            }

            j.push_back({
                {"keyword", k.keyword},
                {"slots", slotStrs}, // string配列を渡すと ["32", "45"] になる
                {"match_words", k.matchWords}
                });
        }

        std::ofstream file(KEYWORDS_JSON_FILE);
        if (file.is_open()) {
            file << j.dump(4); // 4スペースインデント
            AddLog("Saved keywords to JSON.", LogType::Success);
        }
        else {
            AddLog("Failed to save keywords JSON.", LogType::Error);
        }
    }
    catch (const std::exception& e) {
        AddLog("JSON Save Error: " + std::string(e.what()), LogType::Error);
    }
}

// ============================================================
// Config I/O の修正 (Main.cpp 内)
// ============================================================

void LoadUnifiedConfig() {
    SlotDictionary::InitDefaultSlots();

    // ★削除: ここでの g_KeywordList 初期化は削除（JSONロードに任せる）
    /*
    g_KeywordList = { ... };
    */

    g_SourceBlockedList = { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
    g_KeywordBlockedList = { };

    // 1. まず INI設定を読み込む
    std::ifstream file(CONFIG_FILENAME);
    if (file.is_open()) {
        std::string line;
        int currentSection = 0;
        SlotDictionary::slotMap.clear();

        while (std::getline(file, line)) {
            line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
            line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
            if (line.empty() || line[0] == ';') continue;

            if (line == "[General]") { currentSection = 1; continue; }
            if (line == "[Slots]") { currentSection = 2; continue; }
            // if (line == "[Keywords]") { currentSection = 3; g_KeywordList.clear(); continue; } // ★廃止
            if (line == "[BlockedList]") { currentSection = 4; g_SourceBlockedList.clear(); continue; }
            if (line == "[KwBlockedList]") { currentSection = 5; g_KeywordBlockedList.clear(); continue; }

            if (currentSection == 1) {
                // ... (General設定処理：変更なし) ...
                size_t delimiterPos = line.find('=');
                if (delimiterPos != std::string::npos) {
                    std::string key = line.substr(0, delimiterPos);
                    std::string value = line.substr(delimiterPos + 1);
                    if (key == "InputRoot") strncpy_s(g_InputRootPath, value.c_str(), _TRUNCATE);
                    else if (key == "OutputRoot") strncpy_s(g_OutputRootPath, value.c_str(), _TRUNCATE);
                    else if (key == "GameDataPath") strncpy_s(g_GameDataPath, value.c_str(), _TRUNCATE);
                    else if (key == "SynthesisPath") strncpy_s(g_SynthesisPath, value.c_str(), _TRUNCATE);
                    else if (key == "Gender") g_TargetGender = std::stoi(value);
                    else if (key == "SlotDataPath") strncpy_s(g_SlotDataPath, value.c_str(), _TRUNCATE);
                }
            }
            else if (currentSection == 2) {
                // ... (Slots設定処理：変更なし) ...
                size_t delimiterPos = line.find('=');
                if (delimiterPos != std::string::npos) {
                    try {
                        int id = std::stoi(line.substr(0, delimiterPos));
                        SlotDictionary::slotMap[id] = line.substr(delimiterPos + 1);
                    }
                    catch (...) {}
                }
            }
            else if (currentSection == 3) {
                // ★削除: [Keywords] セクションの読み込み処理を削除
            }
            else if (currentSection == 4) g_SourceBlockedList.push_back(line);
            else if (currentSection == 5) g_KeywordBlockedList.push_back(line);
        }
        if (SlotDictionary::slotMap.empty()) SlotDictionary::InitDefaultSlots();
        AddLog("Unified Config Loaded (INI).", LogType::Info);
    }

    // 2. ★追加: JSONからキーワードを読み込む
    LoadKeywordsJSON();

    // GameDataPath 処理 (変更なし)
    if (strlen(g_GameDataPath) > 0) {
        std::string inRoot = (fs::path(g_GameDataPath) / "meshes").string();
        strncpy_s(g_InputRootPath, inRoot.c_str(), _TRUNCATE);
    }
    else {
        if (g_InputRootPath[0] != '\0') g_InputRootPath[0] = '\0';
    }
}

extern void SaveUnifiedConfig() {
    // 1. INI設定を保存
    std::ofstream file(CONFIG_FILENAME);
    if (file.is_open()) {
        file << "[General]\n";
        file << "OutputRoot=" << g_OutputRootPath << "\n";
        file << "GameDataPath=" << g_GameDataPath << "\n";
        file << "SynthesisPath=" << g_SynthesisPath << "\n";
        file << "SlotDataPath=" << g_SlotDataPath << "\n";
        file << "Gender=" << g_TargetGender << "\n\n";

        file << "[Slots]\n";
        for (const auto& pair : SlotDictionary::slotMap) file << pair.first << "=" << pair.second << "\n";

        // ★削除: [Keywords] セクションの書き込みを削除
        /*
        file << "\n[Keywords]\n";
        for (const auto& k : g_KeywordList) { ... }
        */

        file << "\n[BlockedList]\n";
        for (const auto& bl : g_SourceBlockedList) file << bl << "\n";

        file << "\n[KwBlockedList]\n";
        for (const auto& kbl : g_KeywordBlockedList) file << kbl << "\n";

        AddLog("Unified Config Saved (INI).", LogType::Success);
    }

    // 2. ★追加: JSONへキーワードを保存
    SaveKeywordsJSON();
}



// ★追加: メッシュ更新リクエスト用フラグ
// ワーカースレッドやUIから「表示を更新して！」と頼むときに true にします
bool g_RequestMeshUpdate = false;


// ============================================================
// コアロジック
// ============================================================

void UpdateMeshList(nifly::NifFile& targetNif, std::vector<RenderMesh>& outMeshes, bool isRef) {
    outMeshes.clear();

    glm::vec3 minBounds(99999.0f), maxBounds(-99999.0f);
    bool hasVertices = false;
    auto shapes = targetNif.GetShapes();

    for (size_t k = 0; k < shapes.size(); ++k) {
        auto shape = shapes[k];
        if (!shape) continue;
        auto bsShape = dynamic_cast<nifly::BSTriShape*>(shape);
        if (!bsShape) continue;

        std::vector<nifly::Vector3> rawVerts;
        for (const auto& v : bsShape->vertData) rawVerts.push_back({ v.vert.x, v.vert.y, v.vert.z });
        if (rawVerts.empty()) continue;

        for (const auto& v : rawVerts) {
            minBounds = glm::min(minBounds, glm::vec3(v.x, v.y, v.z));
            maxBounds = glm::max(maxBounds, glm::vec3(v.x, v.y, v.z));
        }
        hasVertices = true;

        std::string currentSlots = "None";
        std::vector<int> slotsFound;
        auto skinRef = bsShape->SkinInstanceRef();
        if (!skinRef->IsEmpty()) {
            auto skinObj = targetNif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
            if (auto dismemberSkin = dynamic_cast<nifly::BSDismemberSkinInstance*>(skinObj)) {
                std::stringstream ss;
                for (size_t i = 0; i < dismemberSkin->partitions.size(); ++i) {
                    if (i > 0) ss << ", ";
                    int slotID = dismemberSkin->partitions[i].partID;
                    slotsFound.push_back(slotID);
                    ss << slotID << " (" << SlotDictionary::GetSlotName(slotID) << ")";
                }
                currentSlots = ss.str();
            }
            else currentSlots = "NiSkin";
        }

        struct Vertex { float x, y, z; float nx, ny, nz; };
        std::vector<Vertex> gpuVerts;
        std::vector<unsigned int> indices;
        for (const auto& v : rawVerts) gpuVerts.push_back({ v.x, v.y, v.z, 0,0,1 });
        for (const auto& t : bsShape->triangles) { indices.push_back(t.p1); indices.push_back(t.p2); indices.push_back(t.p3); }

        RenderMesh mesh;
        mesh.indexCount = indices.size();
        mesh.color = isRef ? glm::vec3(0.5f) : GetColorFromIndex((int)outMeshes.size());
        mesh.name = shape->name.get();
        mesh.slotInfo = currentSlots;
        mesh.activeSlots = slotsFound;
        mesh.shapeIndex = (int)k;

        glGenVertexArrays(1, &mesh.VAO); glGenBuffers(1, &mesh.VBO); glGenBuffers(1, &mesh.EBO);
        glBindVertexArray(mesh.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.VBO); glBufferData(GL_ARRAY_BUFFER, gpuVerts.size() * sizeof(Vertex), gpuVerts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.EBO); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx)); glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        outMeshes.push_back(std::move(mesh));
    }
    if (!isRef && hasVertices) g_BodyCenter = (minBounds + maxBounds) * 0.5f;
    if (hasVertices) {
        glm::vec3 center = (minBounds + maxBounds) * 0.5f;
        if (isRef) {
            g_RefBodyCenter = center;
        }
        else {
            g_BodyCenter = center;
        }
    }
}

void LoadNifFileCore(const std::string& path) {
    g_HasPairedFile = false;

    // --- ★変更ここから★ ---
    // Mode 3: Bodyslide Source NIF (Direct Load)
    if (g_NifLoadMode == 3) {
        if (g_NifData.Load(path) == 0) {
            g_CurrentNifPath = path;
            //UpdateMeshList();
            g_RequestMeshUpdate = true;
            AddLog("Loaded (Source): " + fs::path(path).filename().string(), LogType::Info);
        }
        else {
            AddLog("Load Failed (Source): " + path, LogType::Error);
        }
        return;
    }

    // Mode 1: Single (BodySlide ShapeData) - No _0/_1 logic
    if (g_NifLoadMode == 1) {
        fs::path target(path);
        if (!fs::exists(target)) {
            AddLog("Searching BodySlide ShapeData...", LogType::Warning);
            fs::path found = FindFileInBodySlide(target.filename().string());
            if (!found.empty()) {
                target = found;
                AddLog("Found: " + target.string(), LogType::Info);
            }
        }
        if (g_NifData.Load(target.string()) == 0) {
            g_CurrentNifPath = target;
            //UpdateMeshList();
            g_RequestMeshUpdate = true;
            AddLog("Loaded (Single BS): " + target.filename().string(), LogType::Info);
        }
        else {
            AddLog("Load Failed (Single): " + target.string(), LogType::Error);
        }
        return;
    }

    // Mode 0 (Both) or 2 (Pair Only) - Try to load pair
    if (g_NifData.Load(path) == 0) {
        g_CurrentNifPath = path;
        //UpdateMeshList();
        g_RequestMeshUpdate = true;
        AddLog("Loaded: " + GetRelativeMeshesPath(g_CurrentNifPath), LogType::Info);

        // Pair Logic
        fs::path p(path);
        std::string stem = p.stem().string();
        std::string suffix = (stem.length() >= 2) ? stem.substr(stem.length() - 2) : "";
        std::string pairName = "";

        if (suffix == "_0") pairName = stem.substr(0, stem.length() - 2) + "_1.nif";
        else if (suffix == "_1") pairName = stem.substr(0, stem.length() - 2) + "_0.nif";

        if (!pairName.empty()) {
            fs::path pairPath = p.parent_path() / pairName;
            if (fs::exists(pairPath)) {
                if (g_PairedNifData.Load(pairPath.string()) == 0) {
                    g_HasPairedFile = true;
                    g_PairedNifPath = pairPath;
                    AddLog(" + Pair Loaded: " + pairPath.filename().string(), LogType::Info);
                }
            }
            else if (g_NifLoadMode == 2) {
                AddLog("Warning: Pair file not found for Pair Only mode.", LogType::Warning);
            }
        }
    }
    else {
        AddLog("Load Failed: " + path, LogType::Error);
    }
}

// フォルダからのロード (修正版: export -> output の順序保証)
void LoadSlotDataFolder(const std::string& folderPath) {
    if (!fs::exists(folderPath)) { AddLog("Folder not found: " + folderPath, LogType::Error); return; }

    g_DisplayTree.clear();
    g_AllRecords.clear();
    g_RecordSelectionMap.clear(); // リロード時は選択状態もクリア（IDが変わる可能性があるため）

    std::map<std::string, SlotRecord> uniqueRecords;

    // --- ファイル解析用ヘルパー関数 (DRY原則) ---
    auto ParseFile = [&](const fs::path& path) {
        std::ifstream file(path);
        std::string line;
        int count = 0;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            auto cols = SplitString(line, ';');
            if (cols.size() >= 9) {
                SlotRecord rec;
                rec.sourceFile = cols[0];
                rec.armaFormID = cols[1];
                rec.armaEditorID = cols[2];
                rec.armoFormID = cols[3];
                rec.armoEditorID = cols[4];
                rec.malePath = cols[5];
                rec.femalePath = cols[6];
                rec.armoSlots = cols[7];
                rec.armaSlots = cols[8];

                // 新: g_TargetGender に従って優先パスを決定（0=Male, 1=Female）
                if (g_TargetGender == 0) { // Male 優先
                    rec.nifPath = !rec.malePath.empty() ? rec.malePath : rec.femalePath;
                }
                else { // Female 優先
                    rec.nifPath = !rec.femalePath.empty() ? rec.femalePath : rec.malePath;
                }

                // ここで male / female 両方から base key を生成して保持する
                rec.baseNifKeyMale = rec.malePath.empty() ? "" : GetBaseNifKey(rec.malePath);
                rec.baseNifKeyFemale = rec.femalePath.empty() ? "" : GetBaseNifKey(rec.femalePath);

                // 既存の baseNifKey は後方互換で male 優先（従来の振る舞いを維持）
                if (!rec.baseNifKeyMale.empty()) rec.baseNifKey = rec.baseNifKeyMale;
                else rec.baseNifKey = rec.baseNifKeyFemale;

                rec.displayText = rec.armaEditorID;

                // ★ここで上書き発生: 同じEditorIDなら後勝ち
                uniqueRecords[rec.armaEditorID] = rec;
                count++;
            }
        }
        return count;
        };


    AddLog("Loading Slot Data from: " + folderPath, LogType::Info);

    // 1. まず "slotdata-Output.txt" 以外の全てのファイルを読み込む (export等)
    int baseCount = 0;
    for (const auto& entry : fs::directory_iterator(folderPath)) {
        if (entry.path().extension() == ".txt") {
            std::string fname = entry.path().filename().string();
            // ファイル名比較 (大文字小文字を区別せず "slotdata-output.txt" を除外)
            std::string lowerName = fname;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

            if (lowerName != "slotdata-output.txt") {
                baseCount += ParseFile(entry.path());
            }
        }
    }
    // AddLog("Base records loaded: " + std::to_string(baseCount));

    // 2. 最後に "slotdata-Output.txt" を読み込んで上書きする (Override)
    fs::path outPath = fs::path(folderPath) / "slotdata-Output.txt";
    if (fs::exists(outPath)) {
        int overrideCount = ParseFile(outPath);
        AddLog("Applied overrides from slotdata-Output.txt (" + std::to_string(overrideCount) + " records).", LogType::Success);
    }
    else {
        // 大文字小文字の違いで見つからない場合の保険
        fs::path outPathLower = fs::path(folderPath) / "slotdata-output.txt";
        if (fs::exists(outPathLower)) {
            int overrideCount = ParseFile(outPathLower);
            AddLog("Applied overrides from slotdata-output.txt (" + std::to_string(overrideCount) + " records).", LogType::Success);
        }
    }

    // マップからリスト・ツリーへ変換
    int idCounter = 0;
    for (auto& [key, rec] : uniqueRecords) {
        rec.id = ++idCounter;
        g_AllRecords.push_back(rec);
        g_RecordSelectionMap[rec.id] = false;

        // Build Display Tree
        if (!rec.femalePath.empty()) {
            std::string fName = fs::path(rec.femalePath).filename().string();
            g_DisplayTree[rec.sourceFile]["Female NIF"][fName].push_back(rec.id);
        }
        if (!rec.malePath.empty()) {
            std::string mName = fs::path(rec.malePath).filename().string();
            g_DisplayTree[rec.sourceFile]["Male NIF"][mName].push_back(rec.id);
        }
    }

    g_SelectedRecordID = -1;
    AddLog("Total records loaded: " + std::to_string(g_AllRecords.size()), LogType::Success);
}


//ApplySlotChanges　エリア

// ヘルパー: パスの末尾一致判定（絶対パス vs 相対パスの比較用）
bool PathEndsWith(const std::string& fullPath, const std::string& suffix) {
    if (suffix.empty() || fullPath.empty()) return false;

    // 両方を正規化（小文字化 ＆ \ を / に統一）して比較
    std::string s1 = fullPath;
    std::string s2 = suffix;
    auto Normalize = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        std::replace(s.begin(), s.end(), '\\', '/');
        };
    Normalize(s1);
    Normalize(s2);

    if (s2.length() > s1.length()) return false;
    return s1.compare(s1.length() - s2.length(), s2.length(), s2) == 0;
}
// スロット変更適用ロジック
void ApplySlotChanges(int meshIndex, const std::string& slotStr) {
    if (meshIndex < 0 || meshIndex >= g_RenderMeshes.size()) return;

    std::vector<int> newSlots = ParseSlotString(slotStr);
    if (newSlots.empty()) return;

    // 1. Before情報の保存
    if (g_RenderMeshes[meshIndex].beforeSlotInfo.empty()) {
        g_RenderMeshes[meshIndex].beforeSlotInfo = g_RenderMeshes[meshIndex].slotInfo;
    }

    // 2. 状態の退避
    struct StateData { std::string before; std::vector<std::pair<int, int>> sugg; };
    std::map<std::string, StateData> stateBackup;
    for (const auto& m : g_RenderMeshes) {
        stateBackup[m.name] = { m.beforeSlotInfo, m.suggestions };
    }

    // 3. NIF書き換え実行
    int shapeIdx = g_RenderMeshes[meshIndex].shapeIndex;
    std::string targetMeshName = g_RenderMeshes[meshIndex].name;

    auto Apply = [&](nifly::NifFile& nif, int idx) {
        auto shapes = nif.GetShapes();
        if (idx >= shapes.size()) return false;
        auto bs = dynamic_cast<nifly::BSTriShape*>(shapes[idx]);
        if (!bs) return false;
        auto skinRef = bs->SkinInstanceRef();
        if (!skinRef->IsEmpty()) {
            auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
            if (auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin)) {
                for (size_t i = 0; i < dis->partitions.size(); ++i)
                    if (i < newSlots.size()) dis->partitions[i].partID = (uint16_t)newSlots[i];
                return true;
            }
        }
        return false;
        };

    bool mainApplied = Apply(g_NifData, shapeIdx);
    bool pairApplied = false;
    if (g_HasPairedFile) {
        auto pShapes = g_PairedNifData.GetShapes();
        for (size_t k = 0; k < pShapes.size(); ++k) {
            if (pShapes[k]->name.get() == targetMeshName) {
                pairApplied = Apply(g_PairedNifData, (int)k); break;
            }
        }
    }

    if (mainApplied) {
        AddLog("Applied Changes" + std::string(pairApplied ? " (+Pair)" : ""), LogType::Success);

        //UpdateMeshList();
        g_RequestMeshUpdate = true;

        // 退避データの復元
        for (auto& m : g_RenderMeshes) {
            if (stateBackup.count(m.name)) {
                m.beforeSlotInfo = stateBackup[m.name].before;
                m.suggestions = stateBackup[m.name].sugg;
            }
        }

        // --- Pending Save への登録ロジック (OSP対応版) ---

        std::set<int> all;
        for (const auto& m : g_RenderMeshes) for (int sl : m.activeSlots) all.insert(sl);
        std::stringstream css; bool f = true; for (int sl : all) { if (!f)css << ","; css << sl; f = false; }
        std::string newSlotStr = css.str();

        std::string currentNifPath = g_CurrentNifPath.string();

        // ★★★ OSPのOutputパスを事前計算 ★★★
        // 現在のNIFがOSPのソースとして使われている場合、その出力先パス(Meshフォルダ以下)をリストアップ
        std::vector<std::string> validGamePaths;

        // 通常のNIFパスも検索候補に入れる
        validGamePaths.push_back(currentNifPath);

        // OSP情報の検索
        // g_OspFiles は Globals.h で宣言されている前提
        for (const auto& [ospName, ospData] : g_OspFiles) {
            for (const auto& set : ospData.sets) {
                // ソースパスが一致するか？ (std::filesystem::equivalent推奨だが、簡易的に文字列比較)
                // パス区切り文字の違いを吸収するため NormalizePath 等を使うのがベスト
                std::string sPath = set.sourceNifPath;
                // 簡易正規化比較
                if (PathEndsWith(currentNifPath, fs::path(sPath).filename().string())) {
                    // Outputパスを構築: OutputPath + OutputFile
                    // 例: meshes\baku\vdfem + \ + collar
                    fs::path outBase = fs::path(set.outputPath) / set.outputName;

                    // _0.nif と _1.nif の両方を候補に追加
                    validGamePaths.push_back((outBase.string() + "_0.nif"));
                    validGamePaths.push_back((outBase.string() + "_1.nif"));
                    // 接尾辞なしの場合も念のため
                    validGamePaths.push_back((outBase.string() + ".nif"));
                }
            }
        }

        int updatedCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_DataMutex);

            for (auto& r : g_AllRecords) {
                bool match = false;
                bool isOspMatch = false; // ★追加: OSP経由でマッチしたか？

                for (size_t i = 0; i < validGamePaths.size(); ++i) {
                    if (PathEndsWith(validGamePaths[i], r.malePath) || PathEndsWith(validGamePaths[i], r.femalePath)) {
                        match = true;
                        // [0]以外（予測パス）でヒットした＝今開いているのはSourceファイルである
                        if (i > 0) isOspMatch = true;
                        break;
                    }
                }

                if (match) {
                    r.armaSlots = newSlotStr;
                    r.armoSlots = newSlotStr;

                    // ★追加: 由来情報の記録
                    if (isOspMatch) {
                        r.isOspSource = true;
                        r.originalNifPath = currentNifPath; // ShapeData内のパスを記録
                    }
                    else {
                        r.isOspSource = false;
                        r.originalNifPath = currentNifPath; // meshes内のパス
                    }

                    std::string key = r.sourceFile + "_" + r.armaFormID;
                    g_SessionChanges[key] = r;

                    updatedCount++;
                }
            }
        }

        if (updatedCount > 0) {
            AddLog("Updated " + std::to_string(updatedCount) + " linked records in Pending List.", LogType::Info);
        }
        else {
            // デバッグ用に検索したパスを表示しても良い
            AddLog("Note: Changes applied to NIF, but no linked ESP records found.", LogType::Warning);
            AddLog("Debug: Searched for paths like: " + (validGamePaths.size() > 1 ? validGamePaths[1] : "None"), LogType::Info);
        }
    }
}

// ============================================================
// 変更: 重複チェックを行い、8-9列目のみを更新するロジック
// ============================================================

void SaveSessionChangesToFile() {
    if (g_SessionChanges.empty()) return;
    if (strlen(g_SlotDataPath) == 0) return;

    fs::create_directories(g_SlotDataPath);
    fs::path outP = fs::path(g_SlotDataPath) / "slotdata-Output.txt";

    // 1. 既存のファイルをすべてメモリに読み込む
    std::vector<std::string> fileLines;
    if (fs::exists(outP)) {
        std::ifstream inFile(outP);
        std::string line;
        while (std::getline(inFile, line)) {
            if (!line.empty()) {
                // 行末の改行コード除去（念のため）
                if (line.back() == '\r') line.pop_back();
                fileLines.push_back(line);
            }
        }
        inFile.close();
    }

    int updatedCount = 0;
    int newCount = 0;

    // ヘルパー: 行を再構築するラムダ関数
    auto JoinLine = [](const std::vector<std::string>& tokens) {
        std::string res;
        for (size_t i = 0; i < tokens.size(); i++) {
            res += tokens[i];
            if (i < tokens.size() - 1) res += ";";
        }
        return res;
        };

    // 2. セッションの変更分を反映
    for (const auto& pair : g_SessionChanges) {
        const auto& newRec = pair.second;
        bool found = false;

        for (auto& line : fileLines) {
            std::vector<std::string> tokens = SplitString(line, ';');

            // データ破損などの不正行はスキップ
            if (tokens.size() < 9) continue;

            // 1～7カラム目 (Index 0～6) の一致確認
            // sourceFile, armaFormID, armaEditorID, armoFormID, armoEditorID, malePath, femalePath
            // 新: パス比較は正規化して大文字小文字を無視
            auto norm = [](const std::string& s) { return NormalizePathForComparison(s); };

            if (tokens[0] == newRec.sourceFile &&
                tokens[1] == newRec.armaFormID &&
                tokens[2] == newRec.armaEditorID &&
                tokens[3] == newRec.armoFormID &&
                tokens[4] == newRec.armoEditorID &&
                norm(tokens[5]) == norm(newRec.malePath) &&
                norm(tokens[6]) == norm(newRec.femalePath)) {

                // 一致した場合：8カラム目と9カラム目 (Index 7, 8) を上書き
                tokens[7] = newRec.armoSlots;
                tokens[8] = newRec.armaSlots;

                // 行を再構築して更新
                line = JoinLine(tokens);

                found = true;
                updatedCount++;
                break; // 1行見つかればOK
            }
        }

        // 一致する行がなかった場合、新規行として追加
        if (!found) {
            std::stringstream ss;
            ss << newRec.sourceFile << ";" << newRec.armaFormID << ";" << newRec.armaEditorID << ";"
                << newRec.armoFormID << ";" << newRec.armoEditorID << ";"
                << newRec.malePath << ";" << newRec.femalePath << ";"
                << newRec.armoSlots << ";" << newRec.armaSlots;
            fileLines.push_back(ss.str());
            newCount++;
        }
    }

    // 3. ファイルに書き戻し (上書きモード)
    std::ofstream outFile(outP); // std::ios::app は指定しない
    if (!outFile.is_open()) { AddLog("Write failed: " + outP.string(), LogType::Error); return; }

    for (const auto& line : fileLines) {
        outFile << line << "\n";
    }
    outFile.close();

    g_SessionChanges.clear();

    // ログ表示
    std::string msg = "Saved: " + std::to_string(updatedCount) + " updated, " + std::to_string(newCount) + " new.";
    AddLog(msg, LogType::Success);
}
// =========================================================================
// 統合エクスポートワーカー (Text保存 + NIF一括出力)
// =========================================================================
void SaveAndExportAllWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;

    AddLog("Starting Save & Export All...", LogType::Info);

    // 1. 処理前にデータをコピー (SaveSessionChangesToFileがクリアしてしまうため)
    std::map<std::string, SlotRecord> pendingCopy;
    {
        std::lock_guard<std::mutex> lock(g_DataMutex);
        pendingCopy = g_SessionChanges;
    }

    if (pendingCopy.empty()) {
        AddLog("No pending changes to export.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    // 2. テキストファイル保存 (slotdata-output.txt)
    SaveSessionChangesToFile();

    // 3. NIF出力
    int success = 0;
    int fail = 0;
    int current = 0;
    int total = (int)pendingCopy.size();

    // ★重複処理防止用セット
    std::set<std::string> processedPaths;

    // 単一ファイルの保存を行う内部関数 (std::filesystem 版)
    auto ExportSingleNif = [&](const std::string& inPathStr, const std::string& slotStr, bool isOsp) {
        fs::path inPath(inPathStr);

        // 重複チェック: fs::path::make_preferred() でセパレータを統一し、generic_string() で比較
        // または fs::equivalent で実体比較も可能ですが、出力前なので文字列比較が安全
        std::string lowerPath = inPath.generic_string(); // "/" 区切りに統一される
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);

        if (processedPaths.count(lowerPath)) return;
        processedPaths.insert(lowerPath);

        fs::path outPath;

        if (strlen(g_OutputRootPath) > 0) {
            fs::path outRoot(g_OutputRootPath);

            if (isOsp) {
                // [OSPモード] ShapeData フォルダ以降を抽出して結合
                // ヘルパー関数を活用！
                fs::path rel = GetPathFromFolder(inPath, "shapedata");

                // もし "shapedata" がパスに含まれていなければ、CalienteTools... の構成を強制付与するなどの調整可
                // ここでは ShapeData/... として取得できたものを結合
                outPath = outRoot / "CalienteTools" / "BodySlide" / rel;
            }
            else {
                // [通常モード] meshes フォルダ以降を抽出して結合
                fs::path rel = GetPathFromFolder(inPath, "meshes");
                outPath = outRoot / rel;
            }
        }
        else {
            outPath = inPath; // 上書き
        }

        {
            std::lock_guard<std::mutex> lock(g_ProgressMutex);
            g_CurrentProcessItem = "Exporting: " + outPath.filename().string();
        }

        try {
            // 親ディレクトリの作成 (fs::path なら関数一発です)
            if (outPath.has_parent_path()) {
                fs::create_directories(outPath.parent_path());
            }

            nifly::NifFile nif;
            if (nif.Load(inPath.string()) == 0) { // NifFileライブラリは string を要求することが多い
                // ... (スロット変更ロジックは変更なし) ...
                std::vector<int> newSlots = ParseSlotString(slotStr);
                if (!newSlots.empty()) {
                    auto shapes = nif.GetShapes();
                    for (auto s : shapes) {
                        if (auto bs = dynamic_cast<nifly::BSTriShape*>(s)) {
                            auto skinRef = bs->SkinInstanceRef();
                            if (!skinRef->IsEmpty()) {
                                auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
                                if (auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin)) {
                                    for (size_t i = 0; i < dis->partitions.size(); ++i) {
                                        if (i < newSlots.size()) dis->partitions[i].partID = (uint16_t)newSlots[i];
                                    }
                                }
                            }
                        }
                    }
                }
                nif.Save(outPath.string());
                success++;
            }
            else {
                fail++;
                AddLog("Failed load: " + inPath.filename().string(), LogType::Error);
            }
        }
        catch (...) {
            fail++;
        }
    };

    // メインループ
    for (const auto& [key, rec] : pendingCopy) {
        if (g_CancelRequested) break;
        current++;
        g_Progress = (float)current / (float)total;

        std::string targetPath = rec.originalNifPath;

        // フォールバック (通常モードでパス未記録の場合)
        if (targetPath.empty() && !rec.isOspSource) {
            targetPath = (g_TargetGender == 0) ? rec.malePath : rec.femalePath;
            if (targetPath.empty()) targetPath = (g_TargetGender == 0) ? rec.femalePath : rec.malePath;
            targetPath = ConstructSafePath(g_InputRootPath, targetPath).string();
        }

        if (targetPath.empty()) continue;

        // 1. 本体のエクスポート
        ExportSingleNif(targetPath, rec.armaSlots, rec.isOspSource);

        // 2. ★ペアファイルの自動検出とエクスポート (OSPでない場合のみ)
        if (!rec.isOspSource) {
            fs::path p(targetPath);
            std::string stem = p.stem().string();
            std::string ext = p.extension().string();
            std::string pairName = "";

            // 末尾が _0 なら _1 を、_1 なら _0 を探す
            if (stem.size() >= 2) {
                if (stem.substr(stem.size() - 2) == "_0") {
                    pairName = stem.substr(0, stem.size() - 2) + "_1" + ext;
                }
                else if (stem.substr(stem.size() - 2) == "_1") {
                    pairName = stem.substr(0, stem.size() - 2) + "_0" + ext;
                }
            }

            if (!pairName.empty()) {
                fs::path pairPath = p.parent_path() / pairName;
                if (fs::exists(pairPath)) {
                    // ペアも見つかれば同じスロット設定でエクスポート
                    ExportSingleNif(pairPath.string(), rec.armaSlots, false);
                }
            }
        }
    }

    if (!g_CancelRequested) {
        AddLog("Export All: " + std::to_string(success) + " files saved.", LogType::Success);
    }
    else {
        AddLog("Export Cancelled.", LogType::Warning);
    }
    g_IsProcessing = false;
}




// BATCH EXPORT LOGIC (最適化版: 自動保存 + 性別優先 + 重複スキップ)
// --- [Step 4] ExecuteBatchExport の置き換え (Worker関数) ---

// 重い処理を行う実体
void BatchExportWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;

    AddLog("Starting Batch Export...", LogType::Info);

    int successCount = 0;
    int failCount = 0;

    // 重複防止用セット (正規化されたパスを記録)
    std::set<std::string> processedNifs;

    // 処理対象の総数を概算（プログレスバー用）
    // 正確にはNIF数ですが、レコード数で代用しても大きな問題はありません
    int totalRecords = 0;
    for (auto const& [id, selected] : g_RecordSelectionMap) {
        if (selected) totalRecords++;
    }

    int currentCount = 0;

    for (const auto& record : g_AllRecords) {
        if (g_CancelRequested) break;

        // 選択されていないレコードはスキップ
        if (g_RecordSelectionMap.find(record.id) == g_RecordSelectionMap.end() || !g_RecordSelectionMap[record.id]) {
            continue;
        }

        currentCount++;
        g_Progress = (float)currentCount / (float)(totalRecords > 0 ? totalRecords : 1);

        // ターゲットのパスを決定 (性別設定に従う)
        std::string targetPath = (g_TargetGender == 0) ? record.malePath : record.femalePath;
        // 片方しかない場合のフォールバック
        if (targetPath.empty()) targetPath = (g_TargetGender == 0) ? record.femalePath : record.malePath;

        if (targetPath.empty()) continue;

        // パスの正規化 (重複チェック用)
        fs::path fullPath = ConstructSafePath(g_InputRootPath, targetPath);
        std::string fullPathStr = fullPath.string();
        std::string lowerPath = fullPathStr;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);

        // ★★★ 修正ポイント: 既に処理済みのNIFならスキップ ★★★
        if (processedNifs.count(lowerPath)) {
            continue;
        }
        processedNifs.insert(lowerPath);
        // ★★★★★★★★★★★★★★★★★★★★★★★★★★★

        {
            std::lock_guard<std::mutex> lock(g_ProgressMutex);
            g_CurrentProcessItem = "Exporting: " + fullPath.filename().string();
        }

        // ファイル存在チェック
        if (!fs::exists(fullPath)) {
            AddLog("File not found: " + fullPathStr, LogType::Error);
            failCount++;
            continue;
        }

        // NIFロード & スロット適用 & 保存
        try {
            nifly::NifFile nif;
            if (nif.Load(fullPathStr) == 0) {
                bool modified = false;
                auto shapes = nif.GetShapes();

                // 適用するスロット情報 (現在のレコードの ARMA Slots を使用)
                std::vector<int> newSlots = ParseSlotString(record.armaSlots);

                if (!newSlots.empty()) {
                    for (auto shape : shapes) {
                        auto bs = dynamic_cast<nifly::BSTriShape*>(shape);
                        if (!bs) continue;

                        auto skinRef = bs->SkinInstanceRef();
                        if (!skinRef->IsEmpty()) {
                            auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
                            if (auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin)) {
                                // パーティション書き換え
                                for (size_t i = 0; i < dis->partitions.size(); ++i) {
                                    if (i < newSlots.size()) {
                                        if (dis->partitions[i].partID != (uint16_t)newSlots[i]) {
                                            dis->partitions[i].partID = (uint16_t)newSlots[i];
                                            modified = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // 保存 (変更がなくても、「出力ボタンを押した＝NIFを再生成したい」意図とみなし保存する)
                // ※出力先ルート設定がある場合はそちらへ、なければ上書き
                fs::path outPath = fullPath;
                if (strlen(g_OutputRootPath) > 0) {
                    std::string fullStr = fullPath.string();
                    std::string lowerStr = fullStr;
                    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);

                    // パスの中に "meshes" が含まれているか探す
                    size_t pos = lowerStr.find("meshes");
                    if (pos != std::string::npos) {
                        // "meshes" 以降を切り出す (例: "meshes/armor/cuirass.nif")
                        std::string relPath = fullStr.substr(pos);
                        outPath = fs::path(g_OutputRootPath) / relPath;
                    }
                    else {
                        // "meshes" が見つからない場合は OutputRoot/meshes 直下に置く
                        outPath = fs::path(g_OutputRootPath) / "meshes" / fullPath.filename();
                    }

                    // 出力先フォルダがない場合は作成
                    if (outPath.has_parent_path()) {
                        fs::create_directories(outPath.parent_path());
                    }
                }

                nif.Save(outPath.string());
                successCount++;
            }
            else {
                AddLog("Failed to load NIF: " + fullPathStr, LogType::Error);
                failCount++;
            }
        }
        catch (const std::exception& e) {
            AddLog("Exception exporting " + fullPath.filename().string() + ": " + e.what(), LogType::Error);
            failCount++;
        }
    }

    if (!g_CancelRequested) {
        AddLog("Batch Export Completed. Success: " + std::to_string(successCount) + ", Failed: " + std::to_string(failCount), LogType::Success);
    }
    else {
        AddLog("Batch Export Cancelled.", LogType::Warning);
    }

    g_IsProcessing = false;
}


// --- ★修正版v2: 深い階層に対応したエクスポート ---
extern void ExecuteSourceNifExport() {
    if (strlen(g_OutputRootPath) == 0) {
        AddLog("Output Root is not set!", LogType::Error);
        return;
    }
    // 入力元の基準パス設定
    fs::path inBase = fs::path(g_GameDataPath) / "CalienteTools" / "BodySlide" / "ShapeData";
    if (!fs::exists(inBase) && strlen(g_InputRootPath) > 0) {
        fs::path try1 = fs::path(g_InputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData";
        if (fs::exists(try1)) inBase = try1;
        else {
            fs::path try2 = fs::path(g_InputRootPath).parent_path() / "CalienteTools" / "BodySlide" / "ShapeData";
            if (fs::exists(try2)) inBase = try2;
        }
    }

    if (!fs::exists(inBase)) {
        AddLog("Source folder not found, cannot export.", LogType::Error);
        return;
    }

    int count = 0;
    int failCount = 0;

    for (const auto& [key, selected] : g_SourceSelectionMap) {
        if (!selected) continue;

        // ★変更点: 区切り文字を「最後のスラッシュ」で探す (階層が深くてもファイル名を分離できる)
        size_t lastSlash = key.find_last_of('/');
        if (lastSlash == std::string::npos) continue;

        std::string folderRel = key.substr(0, lastSlash); // 相対パス部分 (例: "Category/Set")
        std::string fileName = key.substr(lastSlash + 1); // ファイル名

        // 入力パス結合
        fs::path inPath = inBase / folderRel / fileName;

        // 出力パス: [OutputRoot]/CalienteTools/Bodyslide/Shapedata/[RelPath]/[File]
        fs::path outDir = fs::path(g_OutputRootPath) / "CalienteTools" / "BodySlide" / "ShapeData" / folderRel;
        fs::path outPath = outDir / fileName;

        if (fs::exists(inPath)) {
            try {
                fs::create_directories(outDir);
                nifly::NifFile nif;
                if (nif.Load(inPath.string()) == 0) {
                    nif.Save(outPath.string());
                    count++;
                }
                else {
                    AddLog("Failed to load: " + fileName, LogType::Error);
                    failCount++;
                }
            }
            catch (...) {
                AddLog("Exception during export: " + fileName, LogType::Error);
                failCount++;
            }
        }
        else {
            AddLog("Input file missing: " + fileName, LogType::Warning);
        }
    }

    if (count > 0 || failCount > 0) {
        AddLog("Source Export: " + std::to_string(count) + " success, " + std::to_string(failCount) + " failed.",
            (failCount == 0 ? LogType::Success : LogType::Warning));
    }
    else {
        AddLog("No files selected for export.", LogType::Info);
    }
}


// ============================================================
// メインGUI・ループ
// ============================================================

int main(int, char**) {
    glfwInit();
    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Nif Slot Sniper v1.0.0", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    g_ShaderProgram = CreateShader(vertexShaderSource, fragmentShaderSource);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini";

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    ImGui::StyleColorsDark();

    ImVec4 clear_color = ImVec4(0.2f, 0.2f, 0.25f, 1.00f);
    bool g_RequestInitialSetup = false;
    LoadUnifiedConfig();
    if (strlen(g_GameDataPath) == 0 || strlen(g_InputRootPath) == 0) {
        g_RequestInitialSetup = true;
    }

    SlotDictionary::LoadRules();
    SlotDictionary::InitDefaultComboRules();

    if (strlen(g_SlotDataPath) > 0 && fs::exists(g_SlotDataPath)) {
        LoadSlotDataFolder(g_SlotDataPath);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (!ImGui::GetIO().WantCaptureMouse) {
            // ★追加: window変数が見つからないエラーへの確実な対策
            GLFWwindow* window = glfwGetCurrentContext();

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            if (!g_MouseInitialized) { g_LastMouseX = mx; g_LastMouseY = my; g_MouseInitialized = true; }
            float dx = (float)(mx - g_LastMouseX);
            float dy = (float)(my - g_LastMouseY);
            g_LastMouseX = mx; g_LastMouseY = my;

            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
                    float s = g_CamDistance * 0.001f;
                    g_CamOffset.x += dx * s; g_CamOffset.y -= dy * s;
                }
                else {
                    g_ModelRotation[1] += dx * 0.5f;
                    g_ModelRotation[0] += dy * 0.5f;
                }
            }

            // Spaceキーでのリセット
            if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
                g_CamDistance = 100.0f;
                g_CamOffset = glm::vec3(0.0f);
                g_ModelRotation[0] = 0.0f; g_ModelRotation[1] = 0.0f; g_ModelRotation[2] = 0.0f;
            }
        }
        else {
            // ここでもwindowが必要なため取得 (念のため)
            GLFWwindow* window = glfwGetCurrentContext();
            glfwGetCursorPos(window, &g_LastMouseX, &g_LastMouseY);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Main Menu ---
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Settings")) g_ShowSettingsWindow = true;//true
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            // 各ウィンドウの表示切替
            if (ImGui::MenuItem("Auto-Fix Rules")) g_ShowRulesWindow = true;//true
                if (ImGui::MenuItem("Analysis Details")) g_ShowAnalysisDetailsLog = true;//true
            ImGui::EndMainMenuBar();
        }

        if (g_RequestInitialSetup) {
            ImGui::OpenPopup("Welcome / Initial Setup");
            g_RequestInitialSetup = false;
        }

        // 初期セットアップのモーダル
        if (ImGui::BeginPopupModal("Welcome / Initial Setup", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Welcome to Nif Slot Sniper Ultimate Edition!");
            ImGui::Spacing();
            ImGui::Text("It seems paths are not configured yet.");
            ImGui::Text("Please set the 'Game Root' and 'Input Root' to use the tool features.");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Go to Settings now?");
            ImGui::Separator();
            if (ImGui::Button("Yes, Open Settings", ImVec2(150, 40))) {
                g_ShowSettingsWindow = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No, Later", ImVec2(100, 40))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // --- Camera Info Overlay ---
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 200, 25), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.3f);
        ImGui::Begin("CamInfo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        ImGui::Text("Camera: L-Click=Rotate | Shift+L-Click=Pan | Space=Reset");
        ImGui::End();

        // =========================================================
        // ★ここからUI描画：すべて関数呼び出しに置き換え！
        // =========================================================

        // 1. 設定画面
        RenderSettingsWindow();

        // 2. ルール設定画面
        RenderRulesWindow();

        // 3. コントロールパネル (左側)
        RenderControlPanel();

        // 4. 保留リスト (Items Pending save) (右側)
        RenderPendingArea();

        // 5. データベースウィンドウ (右側)
        RenderDatabase();

        // 6. KID Generator
        RenderKidGenerator();

		// 7. デバッグログウィンドウ
        RenderAnalysisDetailsLog();

        // ---------------------------------------------------------
        // ログウィンドウ (短いのでここにあってもOKですが、必要なら分離可)
        // ---------------------------------------------------------
        {
            ImGui::SetNextWindowPos(ImVec2(10, 720), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(760, 170), ImGuiCond_FirstUseEver);
            ImGui::Begin("Log Console");
            if (ImGui::Button("Clear")) g_LogHistory.clear();
            ImGui::SameLine(); ImGui::Checkbox("Auto Scroll", &g_LogAutoScroll);
            ImGui::BeginChild("LogR", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& l : g_LogHistory) {
                ImVec4 c = (l.type == LogType::Success) ? ImVec4(0.4f, 1, 0.4f, 1) : (l.type == LogType::Error) ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 1, 1, 1);
                ImGui::TextColored(c, "%s", l.message.c_str());
            }
            if (g_LogAutoScroll && g_LogScrollToBottom) { ImGui::SetScrollHereY(1.0f); g_LogScrollToBottom = false; }
            ImGui::EndChild();
            ImGui::End();
        }
        // ---------------------------------------------------------
        // ★追加: 安全なメッシュ更新処理
        // ---------------------------------------------------------
        // UI処理やワーカーからの依頼でフラグが立っていたら、ここで一括更新する
        if (g_RequestMeshUpdate) {
            UpdateMeshList();
            g_RequestMeshUpdate = false; // フラグを下ろす
        }


        // プロセシング画面などの共通処理
        if (g_IsProcessing) {
            ImGui::OpenPopup("Processing");
        }

        // 画面中央に配置
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (ImGui::BeginPopupModal("Processing", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::Text("Processing... Please Wait");
            ImGui::Separator();

            // プログレスバー
            ImGui::ProgressBar(g_Progress, ImVec2(300, 20));

            // 現在の処理対象を表示 (排他制御)
            {
                std::lock_guard<std::mutex> lock(g_ProgressMutex);
                ImGui::TextWrapped("%s", g_CurrentProcessItem.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();

            // キャンセルボタン
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                g_CancelRequested = true;
                // ボタンを押したら即座に「キャンセル中...」に変えると親切
                {
                    std::lock_guard<std::mutex> lock(g_ProgressMutex);
                    g_CurrentProcessItem = "Canceling...";
                }
            }

            // 処理が終わったら閉じる
            if (!g_IsProcessing) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        // --- Rendering ---
        ImGui::Render();
        int dw, dh; glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(g_ShaderProgram); // ★シェーダーを有効化

        // --- Projection & View 行列 (変更なし) ---
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), (float)dw / dh, 0.1f, 10000.0f);
        glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(g_CamOffset.x, g_CamOffset.y, -g_CamDistance));

        // --- Model 行列と中心点の決定 ---
        glm::mat4 model = glm::mat4(1.0f);

        // 回転 (変更なし)
        model = glm::rotate(model, glm::radians(g_ModelRotation[0]), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(g_ModelRotation[1] + 180), glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(g_ModelRotation[2]), glm::vec3(0, 0, 1));
        model = glm::rotate(model, glm::radians(-90.0f), glm::vec3(1, 0, 0));

        // ★★★ 修正: カメラの中心点を決定 ★★★
        glm::vec3 targetCenter = g_BodyCenter; // デフォルトは読み込んだ装備の中心

        // リファレンスが表示されており、かつリファレンスのデータが存在する場合
        if (g_ShowRef && !g_RefRenderMeshes.empty()) {
            // 強制的にリファレンスボディの中心(へそ付近)を使う
            targetCenter = g_RefBodyCenter;
        }

        // 中心点分だけずらして、回転の中心を合わせる
        model = glm::translate(model, -targetCenter);

        glUniformMatrix4fv(glGetUniformLocation(g_ShaderProgram, "projection"), 1, GL_FALSE, glm::value_ptr(proj));
        glUniformMatrix4fv(glGetUniformLocation(g_ShaderProgram, "view"), 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(g_ShaderProgram, "model"), 1, GL_FALSE, glm::value_ptr(model));
        glUniform3f(glGetUniformLocation(g_ShaderProgram, "lightDir"), 0.5f, 1.0f, 0.3f);

        // 1. リファレンスボディの描画 (if文で囲まれている)
        if (g_ShowRef) {
            for (const auto& m : g_RefRenderMeshes) {
                glUniform3f(glGetUniformLocation(g_ShaderProgram, "objectColor"), m.color.r, m.color.g, m.color.b);
                glBindVertexArray(m.VAO); glDrawElements(GL_TRIANGLES, (GLsizei)m.indexCount, GL_UNSIGNED_INT, 0);
            }
        }

        // 2. ★ここが重要！通常のメッシュ描画ループ (if文の外にあること！)
        for (int i = 0; i < g_RenderMeshes.size(); ++i) {
            const auto& m = g_RenderMeshes[i];

            // ブロック判定 (非表示ロジック)
            bool isBlocked = false;
            for (const auto& bw : g_KeywordBlockedList) {
                if (!bw.empty() && m.name.find(bw) != std::string::npos) {
                    isBlocked = true;
                    break;
                }
            }
            if (isBlocked) continue; // ブロックされていればスキップ

            // 描画
            glm::vec3 c = m.color;
            if (i == g_SelectedMeshIndex) c = glm::vec3(1.2f, 1.2f, 1.2f); // 選択中は明るく

            glUniform3f(glGetUniformLocation(g_ShaderProgram, "objectColor"), c.r, c.g, c.b);
            glBindVertexArray(m.VAO);
            glDrawElements(GL_TRIANGLES, (GLsizei)m.indexCount, GL_UNSIGNED_INT, 0);
        }

        // 3. 後処理 (ImGuiの描画より前に置くこと)
        glBindVertexArray(0);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}