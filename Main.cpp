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
#include <regex>
#include <thread>   // ★A2: 終了時のワーカー完了待ち
#include <chrono>

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

#include "Globals.h"
#include "UI_Settings.h"
#include "UI_ControlPanel.h"
#include "UI_PendingArea.h"
#include "UI_Database.h"
#include "UI_KidGenerator.h"
#include "UI_Rules.h"
#include "UI_AnalysisDetails.h"
#include "OSP_Logic.h"
#include "nlohmann/json.hpp"
using json = nlohmann::json;
const char* KEYWORDS_JSON_FILE = "keywords.json";


// 関数プロトタイプ（前方宣言）は残しておく
std::vector<std::string> SplitString(const std::string& s, char delimiter);
std::vector<int> ParseSlotString(const std::string& slotStr);

// ★SEH ラッパーの前方宣言（実体はファイル下部）。定義より前の LoadNifFileCore /
//   ApplySlotChanges などで使うため、ここで宣言しておく。
static int SafeNifLoad(nifly::NifFile& nif, const fs::path& p);
static bool SafeNifSave(nifly::NifFile& nif, const fs::path& p);
static bool SafeWritePartitions(nifly::BSDismemberSkinInstance* dis, const int* slots, int count);

// ============================================================
// シェーダー
// ============================================================
const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aTangent; // ★#3: 接線xyz + ハンドネスw（B=cross(N,T)*w）
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
out vec3 Normal;
out vec3 Tangent;
out float TanW;
out vec3 WorldPos;
out vec2 UV;
void main() {
    gl_Position = projection * view * model * vec4(aPos, 1.0);
    mat3 nrmMat = mat3(transpose(inverse(model)));
    Normal = nrmMat * aNormal;
    Tangent = nrmMat * aTangent.xyz;    // ★#3
    TanW = aTangent.w;                  // ★#3-fix: 従法線ハンドネス
    WorldPos = vec3(model * vec4(aPos, 1.0)); // ★#3: スペキュラ用
    UV = aUV;
}
)";

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
in vec3 Normal;
in vec3 Tangent;
in float TanW;
in vec3 WorldPos;
in vec2 UV;
uniform vec3 lightDir;
uniform vec3 objectColor;
uniform sampler2D diffuseTex;
uniform sampler2D normalTex;   // ★#3: 法線マップ（slot1）
uniform int useTexture;  // 0=flat(色), 1=texture
uniform int useNormal;   // ★#3: 1=法線マップ有効（接線あり & マップ取得済み）
uniform int nmFlipGreen; // ★#3-debug: 1=グリーン反転（DirectX 規約）
uniform int nmFlipHand;  // ★#3-debug: 1=ハンドネス反転
uniform int useAlphaTest;     // ★#3-alpha: 1=アルファテスト discard 有効
uniform float alphaThreshold; // ★#3-alpha: この値未満の α を discard
uniform vec3 viewPos;    // ★#3: カメラのワールド座標（スペキュラ用）
uniform vec3 tint;       // テクスチャモード時の選択ハイライト等
void main() {
    vec3 norm = normalize(Normal);
    if (!gl_FrontFacing) norm = -norm;

    float gloss = 0.0;
    if (useNormal == 1) {
        // ★#3: 接空間法線をサンプルして摂動。BC5(RG)/BC7(RGB) 両対応のため Z を再構成。
        vec3 nt = texture(normalTex, UV).rgb;
        vec2 nxy = nt.xy * 2.0 - 1.0;
        if (nmFlipGreen == 1) nxy.y = -nxy.y; // ★#3-debug: DirectX 規約
        float nz = sqrt(clamp(1.0 - dot(nxy, nxy), 0.0, 1.0));
        vec3 tn = vec3(nxy, nz);
        // TBN を直交化して構築（Gram-Schmidt）。接線が無い形状では useNormal=0 のためここに来ない。
        float hand = (nmFlipHand == 1) ? -TanW : TanW; // ★#3-debug: ハンドネス反転
        vec3 T = normalize(Tangent - norm * dot(norm, Tangent));
        vec3 B = cross(norm, T) * hand; // ★#3-fix: ミラー UV のハンドネス補正
        norm = normalize(mat3(T, B, norm) * tn);
        gloss = texture(normalTex, UV).a; // 法線マップ α の光沢（BC5 は α=1）
    }

    vec3 light = normalize(lightDir);
    float diff = max(dot(norm, light), 0.4);

    if (useTexture == 1) {
        vec4 t = texture(diffuseTex, UV);
        // ★#3-alpha: アルファテスト。閾値未満は捨てる（完全透過部が黒く残るのを防ぐ）。
        if (useAlphaTest == 1 && t.a < alphaThreshold) discard;
        vec3 rgb = t.rgb * diff;
        // ★#3: 簡易 Blinn-Phong 鏡面。診断用途なので控えめに乗せる。
        if (useNormal == 1) {
            vec3 V = normalize(viewPos - WorldPos);
            vec3 H = normalize(light + V);
            float spec = pow(max(dot(norm, H), 0.0), 32.0) * gloss * 0.22;
            rgb += vec3(spec);
        }
        FragColor = vec4(rgb * tint, t.a); // αで半透明を可視化
    } else {
        FragColor = vec4(objectColor * diff, 1.0);
    }
}
)";

// ============================================================
// ★Viewport FBO: 3D をオフスクリーンに描き、ImGui ウィンドウ内に表示するための
//   フレームバッファ。これにより 3D が独立したパネルになり、拡縮等の入力を
//   そのパネルにスコープできる（背景描画＋入力競合だったズーム不能を解消）。
// ============================================================
static unsigned int g_ViewFBO = 0, g_ViewColor = 0, g_ViewDepth = 0;
static int g_ViewW = 0, g_ViewH = 0;
static int g_PanelW = 800, g_PanelH = 600;   // 3D Viewport パネルの内寸（毎フレーム更新）
static bool g_ViewportHovered = false;        // マウスが Viewport 上にあるか

static void EnsureViewportFBO(int w, int h) {
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (g_ViewFBO && g_ViewW == w && g_ViewH == h) return;
    if (!g_ViewFBO) glGenFramebuffers(1, &g_ViewFBO);
    if (!g_ViewColor) glGenTextures(1, &g_ViewColor);
    if (!g_ViewDepth) glGenRenderbuffers(1, &g_ViewDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, g_ViewFBO);
    glBindTexture(GL_TEXTURE_2D, g_ViewColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ViewColor, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, g_ViewDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_ViewDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_ViewW = w; g_ViewH = h;
}



// ============================================================
// ヘルパー
// ============================================================

// fs::path のコンポーネントを走査して、特定のフォルダ名以降のパスを抽出するヘルパー
fs::path GetPathFromFolder(const fs::path& fullPath, const std::string& folderName) {
    // パスを構成要素（フォルダ単位）で走査
    auto it = std::find_if(fullPath.begin(), fullPath.end(), [&](const fs::path& p) {
        // 大文字小文字を無視して比較
        std::string s = p.string();
        AsciiLowerInplace(s); // ★B5
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
    fs::path full = (root / rel).lexically_normal();

    // ★B3: パストラバーサル検証。rel は外部データ（ARMA WorldModel のモデルパス / .osp 由来）であり、
    //   "..\..\foo.nif" のような相対指定でルート外へ書込/上書きされ得る。結合結果を正規化し、
    //   root 配下に収まらなければ拒否（空 path を返す。呼び出し側は空/ロード失敗として安全に継続）。
    if (!root.empty()) {
        fs::path rootN = root.lexically_normal();
        auto mm = std::mismatch(rootN.begin(), rootN.end(), full.begin(), full.end());
        if (mm.first != rootN.end()) {
            AddLog("Refusing path outside root (traversal?): rel='" + relStr + "' -> " + full.string(), LogType::Error);
            return fs::path();
        }
    }
    return full;
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
    // 小文字化（★B5: パス比較用キーは ASCII 限定でマルチバイトを保持）
    AsciiLowerInplace(r);
    // \ を / に統一
    std::replace(r.begin(), r.end(), '\\', '/');
    // 末尾のスラッシュ除去
    while (!r.empty() && r.back() == '/') r.pop_back();
    return r;
}

std::string GetRelativeMeshesPath(const fs::path& fullPath) {
    std::string pathStr = fullPath.string();
    std::string lowerPath = pathStr;
    AsciiLowerInplace(lowerPath); // ★B5
    size_t pos = lowerPath.find("meshes");
    if (pos != std::string::npos) return pathStr.substr(pos);
    return pathStr;
}


// --------------------------------------------------------------------------
// ★B1/B7: アトミック書込ヘルパー。temp(.tmp) に書く→flush/close→rename で置換する。
//   書込中のクラッシュ/電源断/ディスクフルでも、置換が成立するまで既存ファイルは無傷
//   （0 バイト化・途中切れを防ぐ）。同一ボリューム内の rename は概ねアトミック。
//   戻り値: 成功 true（失敗時は本関数内でログ済み）。
// --------------------------------------------------------------------------
static bool AtomicWriteFile(const fs::path& finalP, const std::string& data) {
    fs::path tmpP = finalP; tmpP += ".tmp";
    {
        std::ofstream out(tmpP, std::ios::binary | std::ios::trunc);
        if (!out) { AddLog("Atomic write: cannot open temp file: " + tmpP.string(), LogType::Error); return false; }
        out.write(data.data(), (std::streamsize)data.size());
        out.flush();
        if (!out) {
            AddLog("Atomic write: write/flush failed (disk full?): " + tmpP.string(), LogType::Error);
            out.close(); std::error_code rec; fs::remove(tmpP, rec); return false;
        }
    }
    std::error_code ec;
    fs::rename(tmpP, finalP, ec); // MSVC は MOVEFILE_REPLACE_EXISTING 相当で既存を置換
    if (ec) {
        // フォールバック: 既存を消してから再リネーム（一部環境/ロック対策）。
        std::error_code ec2; fs::remove(finalP, ec2);
        ec.clear(); fs::rename(tmpP, finalP, ec);
        if (ec) {
            AddLog("Atomic write: rename failed: " + ec.message() + " (" + finalP.string() + ")", LogType::Error);
            std::error_code rec; fs::remove(tmpP, rec);
            return false;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// ★B6: 巨大入力による OOM を防ぐため、パース前にファイルサイズ上限で拒否する。
//   ユーザー手編集 JSON（ChangeSet/keywords/per-mesh cache）は通常せいぜい数 MB。
//   （深ネストによるスタックオーバーフローは別問題だが、サイズ上限が実用的な一次防御になる。）
// --------------------------------------------------------------------------
static const uintmax_t kMaxJsonBytes = 64ull * 1024 * 1024;
bool FileSizeOk(const fs::path& p, uintmax_t maxBytes, const char* what) {
    std::error_code ec;
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > maxBytes) {
        AddLog(std::string(what) + ": file too large, refusing to parse ("
            + std::to_string(sz) + " bytes): " + p.string(), LogType::Error);
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// JSON 読み込み関数 (文字列形式の slots に対応)
// --------------------------------------------------------------------------
void LoadKeywordsJSON() {
    std::ifstream file(AppPath(KEYWORDS_JSON_FILE)); // ★#6: 実行ファイルのディレクトリ基準
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
    if (!FileSizeOk(fs::path(AppPath(KEYWORDS_JSON_FILE)), kMaxJsonBytes, "keywords.json")) return;

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

        // ★B7: アトミック書込（書込中クラッシュで keywords.json を 0 バイトにしない）。
        if (AtomicWriteFile(fs::path(AppPath(KEYWORDS_JSON_FILE)), j.dump(4)))
            AddLog("Saved keywords to JSON.", LogType::Success);
        else
            AddLog("Failed to save keywords JSON.", LogType::Error);
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

    g_SourceBlockedList = { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
    g_KeywordBlockedList = { };

    // 1. まず INI設定を読み込む（★#6: 実行ファイルのディレクトリ基準）
    std::ifstream file(AppPath(CONFIG_FILENAME));
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
                    else if (key == "SlotToolPath") strncpy_s(g_SlotToolPath, value.c_str(), _TRUNCATE);
                    else if (key == "BsArchPath") strncpy_s(g_BsArchPath, value.c_str(), _TRUNCATE);
                    else if (key == "RefBodyPath") strncpy_s(g_RefBodyPath, value.c_str(), _TRUNCATE);
                    else if (key == "RefTexFolder") strncpy_s(g_RefTexFolder, value.c_str(), _TRUNCATE);
                    else if (key == "SkeletonPathFemale") strncpy_s(g_SkeletonPathFemale, value.c_str(), _TRUNCATE);
                    else if (key == "SkeletonPathMale") strncpy_s(g_SkeletonPathMale, value.c_str(), _TRUNCATE);
                    else if (key == "ExportCostumeSeed") g_ExportCostumeSeed = (value == "1" || value == "true");
                    else if (key == "Gender") {
                        // ★堅牢化: 壊れた Gender 値で起動時に std::stoi 例外→クラッシュしていた。
                        try { g_TargetGender = std::stoi(value); if (g_TargetGender != 0 && g_TargetGender != 1) g_TargetGender = 1; }
                        catch (...) { AddLog("Config: invalid Gender value '" + value + "' (ignored).", LogType::Warning); }
                    }
                    else if (key == "SlotDataPath") strncpy_s(g_SlotDataPath, value.c_str(), _TRUNCATE);
                    else if (key == "TexCacheNifs") { // ★① テクスチャキャッシュ上限（NIF数）
                        try { g_TexCacheNifLimit = std::stoi(value); if (g_TexCacheNifLimit < 0) g_TexCacheNifLimit = 0; }
                        catch (...) { AddLog("Config: invalid TexCacheNifs value '" + value + "' (ignored).", LogType::Warning); }
                    }
                    else if (key == "ExportNif") g_ExportNif = (value == "1" || value == "true");
                    else if (key == "ExportEsl") g_ExportEsl = (value == "1" || value == "true");
                    else if (key == "ExportTxt") g_ExportTxt = (value == "1" || value == "true");
                    else if (key == "ForceOverwrite") g_ForceOverwrite = (value == "1" || value == "true");
                    else if (key == "LogLevel") { // ★ログレベル 0..3
                        try { g_LogLevel = std::stoi(value); if (g_LogLevel < 0) g_LogLevel = 0; if (g_LogLevel > 3) g_LogLevel = 3; }
                        catch (...) {}
                    }
					//カメラの縦Zオフセット読み込み
                    else if (key == "RefCamZOffset") {
                        try { g_RefCamZOffset = std::stof(value); }
                        catch (...) { AddLog("Config: invalid RefCamZOffset value '" + value + "' (ignored).", LogType::Warning); }
                    }
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
    // ★B7: いったん文字列へ組み立て、アトミック書込で置換する（書込中クラッシュで config.ini を失わない）。
    std::ostringstream file;
    file << "[General]\n";
    file << "OutputRoot=" << g_OutputRootPath << "\n";
    file << "GameDataPath=" << g_GameDataPath << "\n";
    file << "SlotToolPath=" << g_SlotToolPath << "\n";
    file << "BsArchPath=" << g_BsArchPath << "\n";
    file << "RefBodyPath=" << g_RefBodyPath << "\n";
    file << "RefTexFolder=" << g_RefTexFolder << "\n";
    file << "SkeletonPathFemale=" << g_SkeletonPathFemale << "\n"; // ★スケルトン解決パス（女性）
    file << "SkeletonPathMale=" << g_SkeletonPathMale << "\n";     // ★スケルトン解決パス（男性）
    file << "ExportCostumeSeed=" << (g_ExportCostumeSeed ? 1 : 0) << "\n"; // ★衣装シード出力トグル
    file << "SlotDataPath=" << g_SlotDataPath << "\n";
    file << "Gender=" << g_TargetGender << "\n";
    file << "TexCacheNifs=" << g_TexCacheNifLimit << "\n"; // ★① テクスチャキャッシュ上限（NIF数）
    file << "ExportNif=" << (g_ExportNif ? 1 : 0) << "\n"; // ★統合 Export トグル
    file << "ExportEsl=" << (g_ExportEsl ? 1 : 0) << "\n";
    file << "ExportTxt=" << (g_ExportTxt ? 1 : 0) << "\n";
    file << "ForceOverwrite=" << (g_ForceOverwrite ? 1 : 0) << "\n"; // ★Force Overwrite
    file << "LogLevel=" << g_LogLevel << "\n"; // ★ログレベル
    file << "RefCamZOffset=" << g_RefCamZOffset << "\n\n";

    file << "[Slots]\n";
    for (const auto& pair : SlotDictionary::slotMap) {
        file << pair.first << "=" << pair.second << "\n";
    }

    file << "\n[BlockedList]\n";
    for (const auto& bl : g_SourceBlockedList) {
        file << bl << "\n";
    }

    file << "\n[KwBlockedList]\n";
    for (const auto& kbl : g_KeywordBlockedList) {
        file << kbl << "\n";
    }
    file << "\n";

    if (AtomicWriteFile(fs::path(AppPath(CONFIG_FILENAME)), file.str())) // ★#6: 実行ファイルのディレクトリ基準
        AddLog("Unified Config Saved (INI).", LogType::Success);
    else
        AddLog("Failed to save config (INI).", LogType::Error);

    SaveKeywordsJSON();
}

// ============================================================
// ★per-mesh: ChangeSet JSON（slotdata-ChangeSet.json）I/O
//   旧 slotdata-Output.txt（9列 CSV・union のみ）を置換。ユーザー編集（per-mesh meshes[] 込み）を
//   永続化する batch/編集履歴。pending の保存先・バッチ入力。旧 txt とは非互換（ハードブレーク）。
// ============================================================
static const char* CHANGESET_FILENAME = "slotdata-ChangeSet.json";

static std::string SlotsCsv(const std::vector<int>& v) {
    std::string s; for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); } return s;
}

static json SlotRecordToChangeJson(const SlotRecord& r) {
    json j;
    j["source"] = NormalizePatchSource(r.sourceFile); // ★要望①: 二重サフィックス集約（書込時）
    j["armaFormKey"] = r.armaFormKey;
    j["armoFormKey"] = r.armoFormKey;
    j["armaFormID"] = r.armaFormID;
    j["armoFormID"] = r.armoFormID;
    j["armaEditorId"] = r.armaEditorID;
    j["armoEditorId"] = r.armoEditorID;
    j["malePath"] = r.malePath;
    j["femalePath"] = r.femalePath;
    j["armaSlots"] = ParseSlotString(r.armaSlots); // int 配列で保存
    j["armoSlots"] = ParseSlotString(r.armoSlots);
    j["blockingSlots"] = r.blockingSlots;
    j["isOspSource"] = r.isOspSource;
    j["pendingOnly"] = r.pendingOnly;
    j["originalNifPath"] = r.originalNifPath;

    // shapeIndex → ShapeBone の索引（meshes[] にボーン情報を付加するため）。
    std::map<int, const ShapeBone*> sbByIdx;
    for (const auto& sb : r.shapeBones) sbByIdx[sb.shapeIndex] = &sb;

    json ms = json::array();
    for (const auto& m : r.meshes) {
        json mj{ {"shapeIndex", m.shapeIndex}, {"name", m.name}, {"slots", m.slots}, {"toNiSkin", m.toNiSkin} };
        // ★スキンボーン抽出: 既存フィールドは温存し hasSkin/bones/skeletonRoot を加算。
        auto it = sbByIdx.find(m.shapeIndex);
        if (it != sbByIdx.end()) {
            mj["hasSkin"] = it->second->hasSkin;
            mj["bones"] = it->second->bones;
            mj["skeletonRoot"] = it->second->skeletonRoot.empty() ? json(nullptr) : json(it->second->skeletonRoot);
        } else {
            // meshes[]（BSD）に対応する ShapeBone が無い稀ケースは BSD=skinned とみなす。
            mj["hasSkin"] = true;
            mj["bones"] = json::array();
            mj["skeletonRoot"] = nullptr;
        }
        ms.push_back(mj);
    }
    j["meshes"] = ms;

    // ★スキンボーン抽出: 全 shape を網羅する shapes[]（リジッド/NiSkin も含む）。
    json shapesArr = json::array();
    for (const auto& sb : r.shapeBones)
        shapesArr.push_back(json{
            {"shapeIndex", sb.shapeIndex}, {"name", sb.name}, {"hasSkin", sb.hasSkin},
            {"bones", sb.bones}, {"skeletonRoot", sb.skeletonRoot.empty() ? json(nullptr) : json(sb.skeletonRoot)} });
    j["shapes"] = shapesArr;

    // ★スケルトン解決パス: boneResolution（判定済みのときのみ出力）。
    if (r.boneRes.computed) {
        json br;
        br["skeleton"] = r.boneRes.skeleton;
        br["gender"] = r.boneRes.gender;
        br["resolved"] = (r.boneRes.resolved < 0) ? json(nullptr) : json(r.boneRes.resolved != 0);
        br["missingBones"] = r.boneRes.missingBones;
        br["unskinnedShapes"] = r.boneRes.unskinnedShapes;
        if (!r.boneRes.note.empty()) br["note"] = r.boneRes.note;
        j["boneResolution"] = br;
    }
    return j;
}

static SlotRecord ChangeJsonToSlotRecord(const json& j) {
    SlotRecord r;
    r.sourceFile = NormalizePatchSource(j.value("source", "")); // ★要望①: 二重サフィックス集約（読込時）
    r.armaFormKey = j.value("armaFormKey", "");
    r.armoFormKey = j.value("armoFormKey", "");
    r.armaFormID = j.value("armaFormID", "");
    r.armoFormID = j.value("armoFormID", "");
    r.armaEditorID = j.value("armaEditorId", "");
    r.armoEditorID = j.value("armoEditorId", "");
    r.malePath = j.value("malePath", "");
    r.femalePath = j.value("femalePath", "");
    if (j.contains("armaSlots") && j["armaSlots"].is_array()) r.armaSlots = SlotsCsv(j["armaSlots"].get<std::vector<int>>());
    if (j.contains("armoSlots") && j["armoSlots"].is_array()) r.armoSlots = SlotsCsv(j["armoSlots"].get<std::vector<int>>());
    if (j.contains("blockingSlots") && j["blockingSlots"].is_array()) r.blockingSlots = j["blockingSlots"].get<std::vector<int>>();
    r.isOspSource = j.value("isOspSource", false);
    r.pendingOnly = j.value("pendingOnly", false);
    r.originalNifPath = j.value("originalNifPath", "");
    if (j.contains("meshes") && j["meshes"].is_array()) {
        for (const auto& mj : j["meshes"]) {
            MeshSlot m;
            m.shapeIndex = mj.value("shapeIndex", -1);
            m.name = mj.value("name", "");
            if (mj.contains("slots") && mj["slots"].is_array()) m.slots = mj["slots"].get<std::vector<int>>();
            m.toNiSkin = mj.value("toNiSkin", false);
            r.meshes.push_back(m);
        }
    }
    // ★スキンボーン抽出: shapes[]（全 shape のボーン情報）を復元（無ければ空＝旧 /1 互換）。
    if (j.contains("shapes") && j["shapes"].is_array()) {
        for (const auto& sj : j["shapes"]) {
            ShapeBone sb;
            sb.shapeIndex = sj.value("shapeIndex", -1);
            sb.name = sj.value("name", "");
            sb.hasSkin = sj.value("hasSkin", false);
            if (sj.contains("bones") && sj["bones"].is_array()) sb.bones = sj["bones"].get<std::vector<std::string>>();
            if (sj.contains("skeletonRoot") && sj["skeletonRoot"].is_string()) sb.skeletonRoot = sj["skeletonRoot"].get<std::string>();
            r.shapeBones.push_back(std::move(sb));
        }
    }
    // ★スケルトン解決パス: boneResolution を復元（読込後の再判定は呼び出し側が必要なら上書きする）。
    if (j.contains("boneResolution") && j["boneResolution"].is_object()) {
        const auto& bj = j["boneResolution"];
        BoneResolution br;
        br.computed = true;
        br.skeleton = bj.value("skeleton", "");
        br.gender = bj.value("gender", "");
        if (bj.contains("resolved") && bj["resolved"].is_boolean()) br.resolved = bj["resolved"].get<bool>() ? 1 : 0;
        else br.resolved = -1; // null
        if (bj.contains("missingBones") && bj["missingBones"].is_array()) br.missingBones = bj["missingBones"].get<std::vector<std::string>>();
        if (bj.contains("unskinnedShapes") && bj["unskinnedShapes"].is_array()) br.unskinnedShapes = bj["unskinnedShapes"].get<std::vector<std::string>>();
        br.note = bj.value("note", "");
        r.boneRes = br;
    }
    // 表示・export 用の補完（nifPath は表示 gender 優先）
    if (g_TargetGender == 0) r.nifPath = !r.malePath.empty() ? r.malePath : r.femalePath;
    else r.nifPath = !r.femalePath.empty() ? r.femalePath : r.malePath;
    r.displayText = !r.armaEditorID.empty() ? r.armaEditorID
        : ("[" + (r.armaFormID.empty() ? "?" : r.armaFormID) + " " + r.sourceFile + "]");
    return r;
}

// ★衣装シード（利用側プロジェクトへの受け渡しシード）: ChangeSet と**独立**に costume_seed.json を出力。
//   changeset は編集履歴の意味論を持つため、未編集アクセサリ（ネイル等）を含む全件は seed 側で受け渡す。
//   呼び出し元は Import DB（全件＝uniqueRecords）／Convert（変換 recs）。トグル ON のときのみ。
//   各要素: { id, nif_path, gender, bones, resolved, default_enabled:null, strip_class:null }。
//   ポリシー項目（default_enabled / strip_class）は NIF から導出不能なので**必ず null**（境界: NSS は決めない）。
//   bones は全スキンドシェイプのボーン和集合。**スキンドシェイプを1つも持たないレコードは除外**
//   （リジッド専用＝アタッチ候補でない）。表示 gender 分のみ（NSS は表示 gender の NIF のみ読む）。
void WriteCostumeSeed(const std::map<std::string, SlotRecord>& records) {
    if (strlen(g_SlotDataPath) == 0) { AddLog("costume seed: SlotData folder not set; skipped.", LogType::Warning); return; }
    std::string genderLabel = (g_TargetGender == 0) ? "male" : "female";

    json arr = json::array();
    int skippedNoSkin = 0, skippedNoId = 0;
    for (const auto& [k, r] : records) {
        // ボーン和集合（全スキンドシェイプ）。挿入順を保ちつつ重複排除。
        std::vector<std::string> bones; std::set<std::string> seen;
        bool anySkin = false;
        for (const auto& sb : r.shapeBones) {
            if (!sb.hasSkin) continue;
            anySkin = true;
            for (const auto& bn : sb.bones) if (seen.insert(bn).second) bones.push_back(bn);
        }
        if (!anySkin) { ++skippedNoSkin; continue; } // スキンド shape 無し＝アタッチ候補でない → 除外

        // ★id は常に Mutagen FormKey 形 "XXXXXX:Plugin.esp" に統一する（changeset の armaFormKey と一致）。
        //   FormKey が空（txt 由来で未補完）なら armaFormID + ":" + 定義元 source で再構成。
        //   FormID も空なら安定 id を作れない＝アタッチ対象として解決不能なのでスキップ（ガード）。
        std::string id;
        if (!r.armaFormKey.empty()) id = r.armaFormKey;
        else if (!r.armaFormID.empty()) id = r.armaFormID + ":" + NormalizePatchSource(r.sourceFile);
        else {
            ++skippedNoId;
            AddLog("costume seed: skipped a record with no FormKey/FormID (no stable id): "
                + (r.armaEditorID.empty() ? r.sourceFile : r.armaEditorID), LogType::Warning);
            continue;
        }

        std::string nifPath = (g_TargetGender == 0)
            ? (!r.malePath.empty() ? r.malePath : r.femalePath)
            : (!r.femalePath.empty() ? r.femalePath : r.malePath);

        json e;
        e["id"] = id;
        e["nif_path"] = nifPath;
        e["gender"] = genderLabel;
        e["bones"] = bones;
        // resolved は boneResolution 由来の三値（未判定なら null）。
        e["resolved"] = (!r.boneRes.computed || r.boneRes.resolved < 0) ? json(nullptr) : json(r.boneRes.resolved != 0);
        e["default_enabled"] = nullptr; // ★境界: NSS は決めない（利用側が埋める）
        e["strip_class"] = nullptr;     // ★境界: NSS は決めない（利用側が埋める）
        arr.push_back(e);
    }

    json doc;
    doc["schema"] = "nifslot.costumeseed/1";
    doc["gender"] = genderLabel; // この seed がどの表示 gender 分か（別 gender は当該 gender で Import DB 再実行）
    doc["seeds"] = arr;

    fs::path outP = fs::path(g_SlotDataPath) / "costume_seed.json";
    // NIF 由来文字列（shape 名/ボーン名）を含むため replace モードで dump。
    if (!AtomicWriteFile(outP, doc.dump(2, ' ', false, json::error_handler_t::replace)))
        AddLog("costume_seed.json write failed: " + outP.string(), LogType::Warning);
    else
        AddLog("costume_seed.json: " + std::to_string(arr.size()) + " seed(s) written ("
            + std::to_string(skippedNoSkin) + " non-skinned, " + std::to_string(skippedNoId)
            + " no-id skipped).", LogType::Success);
}

// 既存 ChangeSet を読み、key(source_armaFormID) で merge してから書き戻す。
static void WriteChangeSet(const std::map<std::string, SlotRecord>& changes) {
    if (changes.empty()) return;
    if (strlen(g_SlotDataPath) == 0) return;
    fs::create_directories(g_SlotDataPath);
    fs::path outP = fs::path(g_SlotDataPath) / CHANGESET_FILENAME;

    std::map<std::string, json> byKey; // 既存をキーで保持し上書き merge
    if (fs::exists(outP) && FileSizeOk(outP, kMaxJsonBytes, "ChangeSet(existing)")) {
        std::ifstream in(outP);
        try {
            json existing; in >> existing;
            if (existing.contains("changes") && existing["changes"].is_array())
                for (const auto& cj : existing["changes"]) {
                    // ★要望①: 既存行の source も正規化して再キー（旧 *_SlotPatch_SlotPatch 行を集約・自己修復）。
                    json fixed = cj;
                    std::string ns = NormalizePatchSource(cj.value("source", ""));
                    fixed["source"] = ns;
                    byKey[ns + "_" + cj.value("armaFormID", "")] = fixed;
                }
        }
        catch (...) { AddLog("ChangeSet: existing JSON unreadable; rewriting from current pending.", LogType::Warning); }
    }

    int updated = 0, added = 0;
    for (const auto& [k, rec] : changes) {
        std::string fileKey = NormalizePatchSource(rec.sourceFile) + "_" + rec.armaFormID;
        if (byKey.count(fileKey)) ++updated; else ++added;
        byKey[fileKey] = SlotRecordToChangeJson(rec);
    }

    json doc;
    doc["schema"] = "nifslot.changeset/2"; // ★スキンボーン抽出: bones/shapes/boneResolution を加算（既存フィールドは温存）
    json arr = json::array();
    for (auto& [k, cj] : byKey) arr.push_back(cj);
    doc["changes"] = arr;
    // ★衣装シードは changeset とは独立（changeset は編集履歴の意味論を保つ）。
    //   seed は Import DB / Convert 完了時に全件 DB から WriteCostumeSeed で出力する。ここでは出さない。

    // ★NIF 由来の名前に不正 UTF-8 がありうるため replace モードで dump（strict だと例外で落ちる）。
    std::string outStr = doc.dump(2, ' ', false, json::error_handler_t::replace);

    // ★B1: 最重要データ（全編集履歴）なので、書込前に直近 1 世代を .bak へ退避（破損時の復旧用）。
    {
        std::error_code bec;
        if (fs::exists(outP, bec)) {
            fs::path bak = outP; bak += ".bak";
            fs::copy_file(outP, bak, fs::copy_options::overwrite_existing, bec);
            if (bec) AddLog("ChangeSet: .bak backup failed (continuing): " + bec.message(), LogType::Warning);
        }
    }

    // ★B1: アトミック書込（temp→rename）。書込中クラッシュ/電源断で全編集履歴を失わない。
    if (!AtomicWriteFile(outP, outStr)) { AddLog("ChangeSet write failed: " + outP.string(), LogType::Error); return; }
    AddLog(std::string(CHANGESET_FILENAME) + ": " + std::to_string(updated) + " updated, " + std::to_string(added) + " new.", LogType::Success);
}

// ChangeSet を読み out（key=source_armaFormID）に詰める。loaded/skipped を返す。存在/失敗で false。
static bool ReadChangeSet(std::map<std::string, SlotRecord>& out, int& loaded, int& skipped) {
    loaded = 0; skipped = 0;
    if (strlen(g_SlotDataPath) == 0) return false;
    fs::path inP = fs::path(g_SlotDataPath) / CHANGESET_FILENAME;
    if (!fs::exists(inP)) return false;
    if (!FileSizeOk(inP, kMaxJsonBytes, "ChangeSet")) return false;
    std::ifstream in(inP);
    json doc;
    try { in >> doc; }
    catch (const std::exception& e) { AddLog(std::string("ChangeSet parse error: ") + e.what(), LogType::Error); return false; }
    if (!doc.contains("changes") || !doc["changes"].is_array()) return false;
    for (const auto& cj : doc["changes"]) {
        SlotRecord r = ChangeJsonToSlotRecord(cj);
        if (r.sourceFile.empty() && r.armaFormID.empty()) { ++skipped; continue; }
        out[r.sourceFile + "_" + r.armaFormID] = r;
        ++loaded;
    }
    return true;
}

// txt 用の filtered 書き出し関数を ChangeSet JSON 書き出しに置換（呼び出し側 worker は変更不要）。
static void SaveSessionChangesToFileFiltered(const std::map<std::string, SlotRecord>& filtered) {
    WriteChangeSet(filtered);
}

// ============================================================
// ★per-mesh ディスクキャッシュ: NIF パス毎の meshes[] を mtime+size 検証付きで永続化する。
//   Import DB の最大コスト（全ユニーク NIF の読込・パース）を、内容が変わっていない NIF について
//   2 回の stat（file_size + last_write_time）に置き換えてスキップする。
//   ファイル: 実行ディレクトリの nss_permesh_cache.json（再生成可能なので消しても安全＝強制リビルド）。
//   単一ワーカー（ImportWorker）内でのみ使用するためロック不要（g_IsProcessing で排他）。
// ============================================================
static const char* PERMESH_CACHE_FILENAME = "nss_permesh_cache.json";
// ★スキンボーン抽出: schema を /2 に上げ、shapeBones（全 shape のボーン情報）も保存する。
//   旧 /1 キャッシュはボーンを持たないため、ロード時に破棄して NIF を1回だけ再読込させる（再生成可・安全）。
static const char* PERMESH_CACHE_SCHEMA = "nifslot.permeshcache/2";
struct PerMeshCacheEntry { long long mtime = 0; unsigned long long size = 0; std::vector<MeshSlot> meshes; std::vector<ShapeBone> shapeBones; };
static std::map<std::string, PerMeshCacheEntry> g_PerMeshCache;

static std::string PmcKey(const std::string& fullPath) {
    std::string k = fullPath;
    AsciiLowerInplace(k); // ★B5: per-mesh キャッシュキー（パス）
    return k;
}

void PerMeshCache_Load() {
    g_PerMeshCache.clear();
    fs::path p(AppPath(PERMESH_CACHE_FILENAME)); // ★#6: 実行ファイルのディレクトリ基準
    std::error_code ec;
    if (!fs::exists(p, ec)) return;
    if (!FileSizeOk(p, kMaxJsonBytes, "per-mesh cache")) return;
    std::ifstream in(p);
    json doc;
    try { in >> doc; }
    catch (...) { AddLog("per-mesh cache unreadable; will rebuild from NIFs.", LogType::Warning); return; }
    // ★スキンボーン抽出: schema が /2 でなければ（旧 /1 等）丸ごと破棄して NIF から再構築させる。
    if (doc.value("schema", std::string()) != PERMESH_CACHE_SCHEMA) {
        AddLog("per-mesh cache schema mismatch (expected /2); rebuilding from NIFs (one-time).", LogType::Info);
        return;
    }
    if (!doc.contains("entries") || !doc["entries"].is_array()) return;
    for (const auto& e : doc["entries"]) {
        std::string key = e.value("path", "");
        if (key.empty()) continue;
        PerMeshCacheEntry ent;
        ent.mtime = e.value("mtime", (long long)0);
        ent.size = e.value("size", (unsigned long long)0);
        if (e.contains("meshes") && e["meshes"].is_array()) {
            for (const auto& mj : e["meshes"]) {
                MeshSlot m;
                m.shapeIndex = mj.value("shapeIndex", -1);
                m.name = mj.value("name", "");
                if (mj.contains("slots") && mj["slots"].is_array()) m.slots = mj["slots"].get<std::vector<int>>();
                ent.meshes.push_back(m);
            }
        }
        if (e.contains("shapeBones") && e["shapeBones"].is_array()) {
            for (const auto& sj : e["shapeBones"]) {
                ShapeBone sb;
                sb.shapeIndex = sj.value("shapeIndex", -1);
                sb.name = sj.value("name", "");
                sb.hasSkin = sj.value("hasSkin", false);
                if (sj.contains("bones") && sj["bones"].is_array()) sb.bones = sj["bones"].get<std::vector<std::string>>();
                sb.skeletonRoot = sj.value("skeletonRoot", "");
                ent.shapeBones.push_back(std::move(sb));
            }
        }
        g_PerMeshCache[key] = std::move(ent);
    }
}

bool PerMeshCache_Get(const std::string& fullPath, std::vector<MeshSlot>& out, std::vector<ShapeBone>& outBones) {
    auto it = g_PerMeshCache.find(PmcKey(fullPath));
    if (it == g_PerMeshCache.end()) return false;
    std::error_code ec;
    auto sz = fs::file_size(fullPath, ec); if (ec) return false;
    auto wt = fs::last_write_time(fullPath, ec); if (ec) return false;
    long long wtc = (long long)wt.time_since_epoch().count();
    if (it->second.size != (unsigned long long)sz || it->second.mtime != wtc) return false; // 変更あり → 無効
    out = it->second.meshes;
    outBones = it->second.shapeBones;
    return true;
}

bool PerMeshCache_Get(const std::string& fullPath, std::vector<MeshSlot>& out) {
    std::vector<ShapeBone> dummy;
    return PerMeshCache_Get(fullPath, out, dummy);
}

void PerMeshCache_Put(const std::string& fullPath, const std::vector<MeshSlot>& meshes, const std::vector<ShapeBone>& bones) {
    std::error_code ec;
    auto sz = fs::file_size(fullPath, ec); if (ec) return;         // 存在しない/読めない → キャッシュしない
    auto wt = fs::last_write_time(fullPath, ec); if (ec) return;
    PerMeshCacheEntry ent;
    ent.mtime = (long long)wt.time_since_epoch().count();
    ent.size = (unsigned long long)sz;
    ent.meshes = meshes;
    ent.shapeBones = bones;
    g_PerMeshCache[PmcKey(fullPath)] = std::move(ent);
}

void PerMeshCache_Put(const std::string& fullPath, const std::vector<MeshSlot>& meshes) {
    PerMeshCache_Put(fullPath, meshes, std::vector<ShapeBone>{});
}

void PerMeshCache_Save() {
    json doc;
    doc["schema"] = PERMESH_CACHE_SCHEMA;
    json arr = json::array();
    for (const auto& [k, ent] : g_PerMeshCache) {
        json e;
        e["path"] = k;
        e["mtime"] = ent.mtime;
        e["size"] = ent.size;
        json ms = json::array();
        for (const auto& m : ent.meshes)
            ms.push_back(json{ {"shapeIndex", m.shapeIndex}, {"name", m.name}, {"slots", m.slots} });
        e["meshes"] = ms;
        // ★スキンボーン抽出: 全 shape のボーン情報も保存（次回 Import DB でボーン込み即復元）。
        json sbArr = json::array();
        for (const auto& sb : ent.shapeBones)
            sbArr.push_back(json{ {"shapeIndex", sb.shapeIndex}, {"name", sb.name}, {"hasSkin", sb.hasSkin},
                                  {"bones", sb.bones}, {"skeletonRoot", sb.skeletonRoot} });
        e["shapeBones"] = sbArr;
        arr.push_back(e);
    }
    doc["entries"] = arr;
    // ★NIF の shape 名は不正 UTF-8 を含みうる。strict dump だと type_error 例外で落ちるため replace モード
    //   （不正バイトを U+FFFD に置換）で書き出す。コンパクト（内部キャッシュなので最小サイズ優先）。
    // ★B7: アトミック書込（破損しても再生成可能だが、0 バイト残骸を避ける）。
    if (!AtomicWriteFile(fs::path(AppPath(PERMESH_CACHE_FILENAME)), doc.dump(-1, ' ', false, json::error_handler_t::replace)))
        AddLog("per-mesh cache write failed.", LogType::Warning);
}

// ============================================================
// ★移行ユーティリティ: 旧 slotdata-Output.txt（9列CSV・union のみ）→ slotdata-ChangeSet.json
//   ハードブレークで失った旧編集を、各レコードの NIF を読んで per-mesh として復元する
//   （Import DB の per-mesh フェーズと同手順・同キャッシュ）。NIF が読めない行は union のみで書き出し、
//   export 時に union フォールバックする。スレッドで起動する（進捗・キャンセル可）。
// ============================================================
void ConvertOldSlotdataToChangeSet() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_ProcessCancelable = true;
    g_Progress = 0.0f;
    struct Guard { ~Guard() { g_ProcessCancelable = true; g_IsProcessing = false; } } guard;
    try { // ★A1: 本体全体を例外境界で保護（txt パース/WriteChangeSet 段も含める）。

    if (strlen(g_SlotDataPath) == 0) { AddLog("Convert: SlotData folder is not set (Settings).", LogType::Warning); return; }

    // 旧 txt を探す（大文字小文字どちらも）
    fs::path inP = fs::path(g_SlotDataPath) / "slotdata-Output.txt";
    if (!fs::exists(inP)) {
        fs::path lower = fs::path(g_SlotDataPath) / "slotdata-output.txt";
        if (fs::exists(lower)) inP = lower;
        else { AddLog("Convert: slotdata-Output.txt not found in " + std::string(g_SlotDataPath), LogType::Warning); return; }
    }

    // 1) 旧 txt をパース（key = source_armaFormID）
    std::map<std::string, SlotRecord> recs;
    {
        std::ifstream f(inP);
        if (!f) { AddLog("Convert: failed to open " + inP.string(), LogType::Error); return; }
        std::string line; int parsed = 0, skipped = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto cols = SplitString(line, ';');
            if (cols.size() < 9) { ++skipped; continue; }
            SlotRecord r;
            r.sourceFile = NormalizePatchSource(cols[0]); r.armaFormID = cols[1]; r.armaEditorID = cols[2]; // ★要望①
            r.armoFormID = cols[3]; r.armoEditorID = cols[4];
            r.malePath = cols[5]; r.femalePath = cols[6];
            r.armoSlots = cols[7]; r.armaSlots = cols[8];
            r.displayText = !r.armaEditorID.empty() ? r.armaEditorID
                : ("[" + (r.armaFormID.empty() ? "?" : r.armaFormID) + " " + r.sourceFile + "]");
            recs[r.sourceFile + "_" + r.armaFormID] = r;
            ++parsed;
        }
        AddLog("Convert: parsed " + std::to_string(parsed) + " row(s) from " + inP.filename().string()
            + (skipped ? (" (skipped " + std::to_string(skipped) + " malformed)") : ""), LogType::Info);
    }
    if (recs.empty()) { AddLog("Convert: nothing to convert.", LogType::Warning); return; }

    // ★#7(A): 旧 txt は full FormKey を持たず、Export 時の FormKey 再構成（FormID + winner source）が
    //   定義元 mod と一致しないため不確実。ここで live DB（Import DB の slottool 由来＝full FormKey 付き）と
    //   ARMA/ARMO EditorID で突合し、real FormKey を補完する。突合できた行は再構成不要になり安全。
    //   前提: 先に「Import DB (slottool)」を実行して g_AllRecords を構築しておくこと。
    {
        // EditorID(小文字) -> FormKey の索引を live DB から構築（g_DataMutex 下でスナップショット）。
        std::map<std::string, std::string> armaByEid, armoByEid;
        size_t dbSize = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
            dbSize = g_AllRecords.size();
            auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
            for (const auto& r : g_AllRecords) {
                if (!r.armaEditorID.empty() && !r.armaFormKey.empty()) armaByEid.emplace(lower(r.armaEditorID), r.armaFormKey);
                if (!r.armoEditorID.empty() && !r.armoFormKey.empty()) armoByEid.emplace(lower(r.armoEditorID), r.armoFormKey);
            }
        }
        if (dbSize == 0) {
            AddLog("Convert: live DB is empty - FormKeys will NOT be backfilled. "
                   "Run 'Import DB (slottool)' first so converted records get real FormKeys "
                   "(otherwise ESL export relies on unreliable FormKey reconstruction).", LogType::Warning);
        }
        else {
            auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
            int filledArma = 0, filledArmo = 0, missArma = 0, missArmo = 0;
            for (auto& [k, r] : recs) {
                if (r.armaFormKey.empty() && !r.armaEditorID.empty()) {
                    auto it = armaByEid.find(lower(r.armaEditorID));
                    if (it != armaByEid.end()) { r.armaFormKey = it->second; ++filledArma; }
                    else ++missArma;
                }
                if (r.armoFormKey.empty() && !r.armoEditorID.empty()) {
                    auto it = armoByEid.find(lower(r.armoEditorID));
                    if (it != armoByEid.end()) { r.armoFormKey = it->second; ++filledArmo; }
                    else ++missArmo;
                }
            }
            AddLog("Convert: FormKey backfill via DB EditorID match: ARMA " + std::to_string(filledArma)
                + " filled / " + std::to_string(missArma) + " unmatched; ARMO " + std::to_string(filledArmo)
                + " filled / " + std::to_string(missArmo) + " unmatched.",
                (missArma + missArmo > 0) ? LogType::Warning : LogType::Success);
            if (missArma + missArmo > 0)
                AddLog("Convert: unmatched rows keep empty FormKey and will fall back to FormID+source "
                       "reconstruction at ESL export (may resolve to the wrong plugin). "
                       "Re-run Import DB or verify those records manually.", LogType::Warning);
        }
    }

    // ★スケルトン解決パス: 表示 gender のスケルトン名集合を一度だけ構築（複数 NIF 可・';' 区切り）。
    std::set<std::string> skelSet; std::string skelId;
    {
        const char* skelCfg = (g_TargetGender == 0) ? g_SkeletonPathMale : g_SkeletonPathFemale;
        if (skelCfg && strlen(skelCfg) > 0) {
            if (BuildSkeletonBoneSet(skelCfg, skelSet, skelId))
                AddLog("Convert: skeleton bone set built from '" + skelId + "' (" + std::to_string(skelSet.size()) + " nodes).", LogType::Info);
            else
                AddLog("Convert: skeleton NIF(s) unreadable; boneResolution will be null.", LogType::Warning);
        }
    }
    std::string genderLabel = (g_TargetGender == 0) ? "male" : "female";

    // 2) per-mesh 復元（Import DB と同手順・キャッシュ利用）。例外でも DB は壊さない（union のみで継続）。
    try {
        PerMeshCache_Load();
        std::map<std::string, std::vector<MeshSlot>> nifMeshCache;
        std::map<std::string, std::vector<ShapeBone>> nifBoneCache; // ★スキンボーン抽出
        int total = (int)recs.size(), cur = 0, withMesh = 0, cacheHit = 0, nifRead = 0;
        for (auto& [k, rec] : recs) {
            if (g_CancelRequested) { AddLog("Convert: canceled (nothing written).", LogType::Warning); return; }
            ++cur; g_Progress = total > 0 ? (float)cur / (float)total : 1.0f;
            std::string rel = (g_TargetGender == 0) ? rec.malePath : rec.femalePath;
            if (rel.empty()) rel = (g_TargetGender == 0) ? rec.femalePath : rec.malePath;
            if (rel.empty()) continue;
            std::string full = ConstructSafePath(g_InputRootPath, rel).string();
            std::string lower = full; AsciiLowerInplace(lower); // ★B5
            std::vector<MeshSlot> meshes;
            std::vector<ShapeBone> shapeBones;
            auto it = nifMeshCache.find(lower);
            if (it != nifMeshCache.end()) { meshes = it->second; shapeBones = nifBoneCache[lower]; }
            else {
                { std::lock_guard<std::mutex> lk(g_ProgressMutex); g_CurrentProcessItem = "Per-mesh: " + fs::path(full).filename().string(); }
                if (PerMeshCache_Get(full, meshes, shapeBones)) ++cacheHit;
                else { ReadNifPerMeshSlotsAndBones(full, meshes, shapeBones); PerMeshCache_Put(full, meshes, shapeBones); ++nifRead; }
                nifMeshCache[lower] = meshes;
                nifBoneCache[lower] = shapeBones;
            }
            // ★スキンボーン抽出: ボーン情報とスケルトン解決は meshes の有無に関わらず付与する
            //   （リジッド/NiSkin のみの NIF でもボーンと unskinnedShapes を出す）。
            rec.shapeBones = shapeBones;
            ComputeBoneResolution(rec, skelSet, skelId, genderLabel);
            if (meshes.empty()) continue; // NIF 読めない/static → union のみ（export フォールバック）
            rec.meshes = meshes;
            std::set<int> meshUnion; for (const auto& m : meshes) for (int s : m.slots) meshUnion.insert(s);
            std::vector<int> arma = ParseSlotString(rec.armaSlots);
            rec.blockingSlots.clear();
            for (int s : arma) if (!meshUnion.count(s)) rec.blockingSlots.push_back(s);
            ++withMesh;
        }
        PerMeshCache_Save();
        AddLog("Convert: per-mesh recovered for " + std::to_string(withMesh) + "/" + std::to_string(total)
            + " records (cache hit " + std::to_string(cacheHit) + ", read " + std::to_string(nifRead)
            + "). Rows without a readable NIF are union-only.", LogType::Info);
    }
    catch (const std::exception& e) { AddLog(std::string("Convert: per-mesh phase error: ") + e.what() + " (writing union-only).", LogType::Error); }
    catch (...) { AddLog("Convert: per-mesh phase unknown error (writing union-only).", LogType::Error); }

    // ★衣装シード: トグル ON のとき、変換した全件 recs から costume_seed.json を出力（changeset とは独立）。
    if (g_ExportCostumeSeed) {
        try { WriteCostumeSeed(recs); }
        catch (...) { AddLog("Convert: costume seed write error (skipped).", LogType::Warning); }
    }

    // 3) ChangeSet へ書き出し（既存があれば key 単位で merge。旧 txt はそのまま残す）。
    WriteChangeSet(recs);
    AddLog("Convert done -> slotdata-ChangeSet.json. (Legacy slotdata-Output.txt left untouched.)", LogType::Success);
    }
    catch (const std::exception& e) { AddLog(std::string("Convert worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("Convert worker unknown error.", LogType::Error); }
}

// ★追加: メッシュ更新リクエスト用フラグ
// ワーカースレッドやUIから「表示を更新して！」と頼むときに true にします
bool g_RequestMeshUpdate = false;


// ============================================================
// コアロジック
// ============================================================

// 既存の引数付き UpdateMeshList は中央実装へ委譲する薄いラッパーに差し替えます。
// これにより実体は Globals.cpp の UpdateMeshListInternal に一本化されます。
void UpdateMeshList(nifly::NifFile& targetNif, std::vector<RenderMesh>& outMeshes, bool isRef) {
    // 直接内部実装へ委譲（解放ロジックは Globals.cpp の UpdateMeshListInternal が担う）
    UpdateMeshListInternal(targetNif, outMeshes, isRef);
}

void LoadNifFileCore(const std::string& path) {
    g_HasPairedFile = false;
    g_ConvertedNiSkinShapes.clear(); // ★別 NIF をロードしたら NiSkin 化指定をリセット

    // --- ★変更ここから★ ---
    // Mode 3: Bodyslide Source NIF (Direct Load)
    if (g_NifLoadMode == 3) {
        if (SafeNifLoad(g_NifData, fs::path(path)) == 0) {
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
        if (SafeNifLoad(g_NifData, target) == 0) {
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
    if (SafeNifLoad(g_NifData, fs::path(path)) == 0) {
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
                if (SafeNifLoad(g_PairedNifData, pairPath) == 0) {
                    g_HasPairedFile = true;
                    g_PairedNifPath = pairPath;
                    AddLog(" + Pair Loaded: " + pairPath.filename().string(), LogType::Info);
                }
                else {
                    AddLog("Pair file exists but failed to load: " + pairPath.filename().string(), LogType::Warning);
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
// ★バッチモード: slotdata-Output.txt を「ブラウズ DB」ではなく "pending" に読み込む。
//   読み込み後、ユーザーが Pending パネルの統合 Export（NIF/ESL/txt）で一括生成する。
//   esl は armaFormKey 空でも armaFormID+source から FormKey 再構成されるため動作する。
void LoadSlotdataIntoPending() {
    if (strlen(g_SlotDataPath) == 0) { AddLog("Batch: SlotData folder is not set (Settings).", LogType::Warning); return; }
    // ★pending に既に何かあれば展開を中止（取り違え防止）。
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        if (!g_SessionChanges.empty()) {
            AddLog("Batching Canceled: Pending is not empty (" + std::to_string(g_SessionChanges.size())
                + " item(s)). Export or clear Pending first.", LogType::Warning);
            return;
        }
    }
    // ★per-mesh: 旧 slotdata-Output.txt（union のみ）→ slotdata-ChangeSet.json（per-mesh meshes[] 込み）。
    fs::path inP = fs::path(g_SlotDataPath) / CHANGESET_FILENAME;
    if (!fs::exists(inP)) {
        AddLog(std::string("Batch: ") + CHANGESET_FILENAME + " not found in " + std::string(g_SlotDataPath)
            + ". (Old slotdata-Output.txt is no longer read in this version.)", LogType::Warning);
        return;
    }

    std::map<std::string, SlotRecord> loadedMap;
    int loaded = 0, skipped = 0;
    if (!ReadChangeSet(loadedMap, loaded, skipped)) {
        AddLog(std::string("Batch: failed to read ") + CHANGESET_FILENAME + ".", LogType::Error);
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        for (auto& [k, r] : loadedMap) g_SessionChanges[k] = r;
    }
    // ★誤解防止: バッチ読込直後は出力チェック（NIF/ESL/txt）を外す。これらは「この
    //   セッションで実際に編集した結果」ではないため、ユーザーが意図的に選び直してから Export させる。
    g_ExportNif = false;
    g_ExportEsl = false;
    g_ExportTxt = false;

    AddLog(std::string("Batch: loaded ") + std::to_string(loaded) + " record(s) into pending from " + CHANGESET_FILENAME
        + (skipped ? (" (skipped " + std::to_string(skipped) + " malformed)") : "")
        + ". Output checkboxes were cleared - tick NIF/ESL/txt deliberately, then press Export.", LogType::Success);
}


// ★Step3: uniqueRecords（armaEditorID -> SlotRecord）から g_AllRecords / g_DisplayTree /
//   g_RecordSelectionMap を再構築する。txt ローダと JSON ローダの両方が使う共有処理。
void BuildRecordsAndTree(std::map<std::string, SlotRecord>& uniqueRecords) {
    ++g_DbVersion; // ★DB が作り直されたことを通知（フィルタキャッシュ無効化）
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
}

//ApplySlotChanges　エリア

// ヘルパー: パスの末尾一致判定（絶対パス vs 相対パスの比較用）
bool PathEndsWith(const std::string& fullPath, const std::string& suffix) {
    if (suffix.empty() || fullPath.empty()) return false;

    // 両方を正規化（小文字化 ＆ \ を / に統一）して比較
    std::string s1 = fullPath;
    std::string s2 = suffix;
    auto Normalize = [](std::string& s) {
        AsciiLowerInplace(s); // ★B5
        std::replace(s.begin(), s.end(), '\\', '/');
        };
    Normalize(s1);
    Normalize(s2);

    if (s2.length() > s1.length()) return false;
    return s1.compare(s1.length() - s2.length(), s2.length(), s2) == 0;
}

// ★per-mesh 記録の共有: 現在表示中の g_RenderMeshes から meshes[] スナップショット（toNiSkin 反映）を作り、
//   currentNif（＋OSP 出力候補）に一致する g_AllRecords を更新して g_SessionChanges に登録する。
//   ApplySlotChanges（slot 編集後）と ConvertSelectedMeshToNiSkin（NiSkin 化後）が共用。
//   armaSlots は union(非 NiSkin の slots) ∪ blockingSlots（NiSkin 化メッシュは union から除外＝ESP からも外れる）。
// ★C26110/C26117 抑制: 本関数は g_DataMutex(recursive_mutex) を lock_guard で全スコープ RAII 保持しており
//   早期 return も手動 unlock も無く正しい。だが SAL 並行性チェッカは recursive_mutex の再入を
//   モデル化できず「保持されていないロックを解放」と誤検出するため、ここだけ抑制する（誤検出）。
#pragma warning(push)
#pragma warning(disable: 26110 26117)
static void RecordCurrentMeshesToPending() {
    std::vector<MeshSlot> meshSnapshot;
    std::set<int> unionSet;
    for (const auto& m : g_RenderMeshes) {
        MeshSlot ms; ms.shapeIndex = m.shapeIndex; ms.name = m.name; ms.slots = m.activeSlots;
        ms.toNiSkin = (g_ConvertedNiSkinShapes.count(m.shapeIndex) > 0);
        if (ms.toNiSkin) ms.slots.clear();
        meshSnapshot.push_back(ms);
        if (!ms.toNiSkin) for (int sl : m.activeSlots) unionSet.insert(sl);
    }

    std::string currentNifPath = g_CurrentNifPath.string();
    int updatedCount = 0;
    {
        // ★#1: g_OspFiles / g_AllRecords / g_SessionChanges への一連のアクセスを 1 ロックで保護。
        //   （旧コードは g_OspFiles 反復をロック外で行っており、OSP ワーカーの挿入と競合し得た。）
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);

        std::vector<std::string> validGamePaths;
        validGamePaths.push_back(currentNifPath);
        for (const auto& [ospName, ospData] : g_OspFiles) {
            for (const auto& set : ospData.sets) {
                std::string sPath = set.sourceNifPath;
                if (PathEndsWith(currentNifPath, fs::path(sPath).filename().string())) {
                    fs::path outBase = fs::path(set.outputPath) / set.outputName;
                    validGamePaths.push_back((outBase.string() + "_0.nif"));
                    validGamePaths.push_back((outBase.string() + "_1.nif"));
                    validGamePaths.push_back((outBase.string() + ".nif"));
                }
            }
        }

        for (auto& r : g_AllRecords) {
            bool match = false, isOspMatch = false;
            for (size_t i = 0; i < validGamePaths.size(); ++i) {
                if (PathEndsWith(validGamePaths[i], r.malePath) || PathEndsWith(validGamePaths[i], r.femalePath)) {
                    match = true; if (i > 0) isOspMatch = true; break;
                }
            }
            if (!match) continue;
            r.meshes = meshSnapshot;
            std::set<int> armaSet = unionSet;
            for (int b : r.blockingSlots) armaSet.insert(b);
            std::string armaStr; { bool f2 = true; for (int sl : armaSet) { if (!f2) armaStr += ","; armaStr += std::to_string(sl); f2 = false; } }
            r.armaSlots = armaStr; r.armoSlots = armaStr;
            r.isOspSource = isOspMatch; r.originalNifPath = currentNifPath;
            std::string key = r.sourceFile + "_" + r.armaFormID;
            g_SessionChanges[key] = r;
            updatedCount++;
        }

        if (updatedCount > 0) AddLog("Updated " + std::to_string(updatedCount) + " linked records in Pending List.", LogType::Info);
        else {
            AddLog("Note: Changes applied to NIF, but no linked ESP records found.", LogType::Warning);
            AddLog("Debug: Searched for paths like: " + (validGamePaths.size() > 1 ? validGamePaths[1] : "None"), LogType::Info);
        }
    }
}
#pragma warning(pop)

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
                // 変更: partitions 数に応じて左から書き込めるだけ書き込む（超過分はスキップ）
                size_t partCount = dis->partitions.size();
                size_t writeCount = std::min(partCount, newSlots.size());

                if (writeCount == 0) {
                    AddLog(std::string("Apply skipped: no writable partitions for mesh: ") + bs->name.get(), LogType::Warning);
                    return false;
                }

                // ★SEH 保護: パーティション書込はアクセス違反でアプリ全体を落としうるため
                //   __try ラッパー経由で行う（GetShapes/dynamic_cast は本ラムダ＝__try 外）。
                if (!SafeWritePartitions(dis, newSlots.data(), (int)writeCount)) {
                    AddLog(std::string("Apply crashed during partition write (skipped): ") + bs->name.get(), LogType::Error);
                    return false;
                }

                if (newSlots.size() > writeCount) {
                    // スキップされたスロットをログに出す
                    std::stringstream ss;
                    for (size_t i = writeCount; i < newSlots.size(); ++i) {
                        if (i > writeCount) ss << ", ";
                        ss << newSlots[i];
                    }
                    AddLog(std::string("Apply partial: wrote ") + std::to_string(writeCount)
                        + " slots for mesh: " + bs->name.get()
                        + ". Skipped: " + ss.str(), LogType::Info);
                }

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

        // UI更新依頼
        g_RequestMeshUpdate = true;

        // 退避データの復元（before情報とsuggestionsのみ）
        for (auto& m : g_RenderMeshes) {
            if (stateBackup.count(m.name)) {
                m.beforeSlotInfo = stateBackup[m.name].before;
                m.suggestions = stateBackup[m.name].sugg;
            }
        }

        // --- ここで該当メッシュの表示状態を即座に更新 ---
        // 入力された newSlots から、実際に書き込めた個数だけを mesh の slotInfo / activeSlots に反映する。
        // 実際に書き込めた数は g_RenderMeshes の元情報（activeSlots.size()）に依存するのでそれに合わせる。
        size_t maxWrite = g_RenderMeshes[meshIndex].activeSlots.size();
        size_t writeCount = std::min(maxWrite, newSlots.size());
        std::vector<int> writtenSlots;
        std::stringstream ssLocal; bool firstLocal = true;
        for (size_t i = 0; i < writeCount; ++i) {
            writtenSlots.push_back(newSlots[i]);
            if (!firstLocal) ssLocal << ",";
            ssLocal << newSlots[i];
            firstLocal = false;
        }
        std::string newSlotStrLocal = ssLocal.str();

        if (meshIndex >= 0 && meshIndex < g_RenderMeshes.size()) {
            g_RenderMeshes[meshIndex].slotInfo = newSlotStrLocal;
            g_RenderMeshes[meshIndex].activeSlots = writtenSlots;
        }
        // --- 即時UI反映ここまで ---

        // --- Pending Save への登録（per-mesh 記録 + 置換維持型 armaSlots）。共有関数に委譲。---
        RecordCurrentMeshesToPending();
    }
}

// ★選択中メッシュを NiSkin 化（BSD→NiSkin）し、pending に記録する。
//   g_NifData（＋ペア）を即変換して表示に反映し、g_ConvertedNiSkinShapes に shapeIndex を登録、
//   その後 RecordCurrentMeshesToPending で ESP からもスロットを外して記録する。export 時に原本へ再適用される。
void ConvertSelectedMeshToNiSkin() {
    int sel = g_SelectedMeshIndex;
    if (sel < 0 || sel >= (int)g_RenderMeshes.size()) { AddLog("Convert: no mesh selected.", LogType::Warning); return; }
    auto& m = g_RenderMeshes[sel];
    if (m.slotInfo == "NiSkin") { AddLog("Convert: '" + m.name + "' is already NiSkin.", LogType::Info); return; }
    int shapeIdx = m.shapeIndex;

    if (!ConvertShapeToNiSkin(g_NifData, shapeIdx)) {
        AddLog("Convert to NiSkin failed (no BSDismemberSkinInstance?): " + m.name, LogType::Error);
        return;
    }
    // ペアも同名 shape を変換
    if (g_HasPairedFile) {
        auto pShapes = g_PairedNifData.GetShapes();
        for (size_t k = 0; k < pShapes.size(); ++k)
            if (pShapes[k] && pShapes[k]->name.get() == m.name) { ConvertShapeToNiSkin(g_PairedNifData, (int)k); break; }
    }

    g_ConvertedNiSkinShapes.insert(shapeIdx);
    // 即時 UI 反映（次フレームの再構築前に表示を NiSkin に）
    m.slotInfo = "NiSkin"; m.activeSlots.clear();
    AddLog("Converted to NiSkin: " + m.name + " (its slot is removed from the ESP too).", LogType::Success);

    RecordCurrentMeshesToPending();
    g_RequestMeshUpdate = true;
}

// ★選択中 NiSkin メッシュを BSD 化（既定 slot 32・1 パーティション）して pending に記録する。
//   その後 per-partition エディタでスロットを編集できる。BSD→NiSkin の逆操作（連続テスト用）。
void ConvertSelectedMeshToBSD() {
    int sel = g_SelectedMeshIndex;
    if (sel < 0 || sel >= (int)g_RenderMeshes.size()) { AddLog("Convert: no mesh selected.", LogType::Warning); return; }
    auto& m = g_RenderMeshes[sel];
    if (m.slotInfo != "NiSkin") { AddLog("Convert: '" + m.name + "' already has partitions (not NiSkin).", LogType::Info); return; }
    int shapeIdx = m.shapeIndex;
    const int defSlot = 32; // 既定 Body。変換後に per-partition エディタで変更可能。

    if (!ConvertShapeToBSD(g_NifData, shapeIdx, defSlot)) {
        AddLog("Convert to BSD failed: " + m.name, LogType::Error);
        return;
    }
    if (g_HasPairedFile) {
        auto pShapes = g_PairedNifData.GetShapes();
        for (size_t k = 0; k < pShapes.size(); ++k)
            if (pShapes[k] && pShapes[k]->name.get() == m.name) { ConvertShapeToBSD(g_PairedNifData, (int)k, defSlot); break; }
    }

    g_ConvertedNiSkinShapes.erase(shapeIdx); // NiSkin 指定を解除（再び BSD）
    // 即時 UI 反映
    m.slotInfo = std::to_string(defSlot);
    m.activeSlots = { defSlot };
    AddLog("Converted to BSD: " + m.name + " (1 partition, slot " + std::to_string(defSlot) + " - edit it below).", LogType::Success);

    RecordCurrentMeshesToPending();
    g_RequestMeshUpdate = true;
}

// ============================================================
// 変更: 重複チェックを行い、8-9列目のみを更新するロジック
// ============================================================

void SaveSessionChangesToFile() {
    // ★スレッド安全: g_SessionChanges の読み出し・clear を保護（detach ワーカーの erase/write と競合するため）。
    //   呼び出し元（UI ボタン・ExportOSPWorker 末尾）はいずれも g_DataMutex 非保持なので再帰ロックにはならない。
    std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
    if (g_SessionChanges.empty()) return;
    if (strlen(g_SlotDataPath) == 0) return;

    // ★per-mesh: slotdata-ChangeSet.json に merge して書き出し、pending をクリアする。
    //   （旧 9 列 txt 経路は撤去。union だけでなく meshes[]/blockingSlots も永続化される。）
    WriteChangeSet(g_SessionChanges);
    g_SessionChanges.clear();
}
// =========================================================================
// 統合エクスポートワーカー (Text保存 + NIF一括出力)
// =========================================================================
// --- 変更: SaveAndExportAllWorker の ExportSingleNif の戻り値を bool にして成功分のみ保存する ---
// ★NIF 出力の結果分類。Duplicate / NoPartition は「成功扱い」（pending クリア対象）だが
//   内訳として別カウントする。Mismatch は手動チェック要（pending に残す）。Failed は真の失敗。
enum class NifExportStatus { Exported, DupSkip, NoPartition, PendingOnly, Mismatch, Failed };

// ★バッチ堅牢化: nifly の Load/Save をSEH(__try/__except)でラップ。特定 NIF での
//   アクセス違反（C++ try/catch では捕捉不能なネイティブ例外）でアプリ全体が落ちるのを防ぎ、
//   その NIF をスキップして継続させる。__try を使うため、これらの関数フレームには
//   デストラクタを要する C++ ローカルを置かない（C2712 回避）。引数は参照で受ける。
// ★B2: SEH（アクセス違反等）専用の最内ラッパ。C++ 例外（bad_alloc 等）はここでは捕捉しない。
//   /EHsc では __except が C++ 例外をすり抜けるため、C++ try/catch は外側ラッパに置く。
//   __try を含む関数にはデストラクタを要する C++ 局所を置けない（C2712）ので分離している。
static int SafeNifLoadSEH(nifly::NifFile& nif, const fs::path& p) {
    __try { return nif.Load(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -9999; }
}
static bool SafeNifSaveSEH(nifly::NifFile& nif, const fs::path& p) {
    __try { nif.Save(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ★B6: 破損 NIF がヘッダで巨大なブロック数/頂点数を主張すると nifly が巨大 vector を確保し bad_alloc。
//   読込前にファイルサイズ上限で弾く（通常の装備 NIF は数十 MB 以内。余裕をみて 512MB を上限）。
static const uintmax_t kMaxNifBytes = 512ull * 1024 * 1024;

// ★B2/A1: SEH に加えて C++ 例外（bad_alloc 等）も捕捉する外側ラッパ。
//   これにより detach ワーカーだけでなく UI スレッド経路（LoadNifFileCore/LoadReferenceBody 等）でも
//   破損 NIF による std::terminate を防げる。戻り値: 0=成功、非0=失敗（負値）。
static int SafeNifLoad(nifly::NifFile& nif, const fs::path& p) {
    std::error_code ec;
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > kMaxNifBytes) {
        AddLog("NIF too large, refusing to load (" + std::to_string(sz) + " bytes): " + p.string(), LogType::Error);
        return -9998;
    }
    try { return SafeNifLoadSEH(nif, p); }
    catch (const std::exception& e) { AddLog(std::string("NIF load exception: ") + e.what() + " (" + p.string() + ")", LogType::Error); return -9997; }
    catch (...) { AddLog("NIF load unknown exception (" + p.string() + ")", LogType::Error); return -9996; }
}
static bool SafeNifSave(nifly::NifFile& nif, const fs::path& p) {
    try { return SafeNifSaveSEH(nif, p); }
    catch (const std::exception& e) { AddLog(std::string("NIF save exception: ") + e.what() + " (" + p.string() + ")", LogType::Error); return false; }
    catch (...) { AddLog("NIF save unknown exception (" + p.string() + ")", LogType::Error); return false; }
}

// ★単発ロード系SEH: パーティション書込（GetShapes 等の C++ 一時を持たない生ポインタ操作のみ）を
//   __try で保護する。失敗（アクセス違反）で false。呼び出し側は GetShapes/dynamic_cast を __try の
//   外（通常 C++）で行い、得た dis ポインタと slot 配列だけを渡すこと（C2712 回避のため本関数内に
//   デストラクタを要する C++ ローカルを置かない）。
#ifdef _WIN32
static bool SafeWritePartitions(nifly::BSDismemberSkinInstance* dis, const int* slots, int count) {
    __try {
        for (int i = 0; i < count; ++i) dis->partitions[i].partID = (uint16_t)slots[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
static bool SafeWritePartitions(nifly::BSDismemberSkinInstance* dis, const int* slots, int count) {
    for (int i = 0; i < count; ++i) dis->partitions[i].partID = (uint16_t)slots[i];
    return true;
}
#endif

// ★per-mesh: NIF を読み、shape 毎の BSDismemberSkinInstance パーティション slot を取得する。
//   ScanNifSlotsWorker の内ループと同要領。shapeIndex は GetShapes() の並び順。
//   GetShapes() 等は SafeNifLoad の __try 外（通常 C++）で行う。dismember を1つでも持てば true。
// ★スキンボーン抽出: meshes[]（BSD のみ・従来挙動）と shapeBones（全 shape を網羅）を NIF 1 回読みで構築する。
//   - bones は nifly の GetShapeBoneList（NiSkin/BSD を問わず参照ボーン NiNode 名を返す）で取得。厳密一致・大小区別を保持。
//   - skeletonRoot は skin instance（NiSkinInstance 派生＝BSD 含む）の targetRef → NiNode 名。取得不可なら空。
//   - skin instance を持たないリジッド shape は hasSkin=false / bones=[]（利用側で変形しない＝区別が要る）。
//   GetShapes()/dynamic_cast は SafeNifLoad の __try 外（通常 C++）で行う。dismember を1つでも持てば true。
bool ReadNifPerMeshSlotsAndBones(const std::string& nifFullPath, std::vector<MeshSlot>& outMeshes,
                                 std::vector<ShapeBone>& outShapeBones) {
    outMeshes.clear();
    outShapeBones.clear();
    nifly::NifFile nif;
    if (SafeNifLoad(nif, fs::path(nifFullPath)) != 0) return false;
    bool hadDismember = false;
    auto shapes = nif.GetShapes();
    for (size_t k = 0; k < shapes.size(); ++k) {
        auto* shape = shapes[k];
        if (!shape) continue;

        // --- 全 shape を網羅する ShapeBone ---
        ShapeBone sb;
        sb.shapeIndex = (int)k;
        sb.name = shape->name.get();
        sb.hasSkin = shape->HasSkinInstance();
        if (sb.hasSkin) {
            // 参照ボーン名（NiSkin/BSD 共通）。nifly が boneRefs → NiNode 名を解決する。
            nif.GetShapeBoneList(shape, sb.bones);
            // ★エッジケース 6-1: skin instance はあるが Bones が空/null → hasSkin=true のまま bones=[] で残し、警告。
            //   利用側の解決判定で「スキンドなのにボーン 0＝アタッチできない」を検出できるようにする。
            if (sb.bones.empty())
                AddLog("Bone extract: shape '" + sb.name + "' has a skin instance but no bones (hasSkin=true, bones=[]): "
                    + fs::path(nifFullPath).filename().string(), LogType::Warning);
            // Skeleton Root（診断用）: skin instance の targetRef → NiNode 名。
            auto* skinRef = shape->SkinInstanceRef();
            if (skinRef && !skinRef->IsEmpty()) {
                auto* skin = nif.GetHeader().GetBlock<nifly::NiSkinInstance>(skinRef->index);
                if (skin) {
                    auto* root = nif.GetHeader().GetBlock<nifly::NiNode>(skin->targetRef.index);
                    if (root) sb.skeletonRoot = root->name.get();
                }
            }
        }
        outShapeBones.push_back(std::move(sb));

        // --- meshes[]（BSD のみ・従来どおり。export のパーティション書込キー） ---
        auto* bs = dynamic_cast<nifly::BSTriShape*>(shape);
        if (!bs) continue;
        auto skinRef = bs->SkinInstanceRef();
        if (skinRef->IsEmpty()) continue;
        auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
        auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin);
        if (!dis) continue;
        hadDismember = true;
        MeshSlot ms;
        ms.shapeIndex = (int)k;
        ms.name = bs->name.get();
        for (const auto& p : dis->partitions) ms.slots.push_back((int)p.partID);
        outMeshes.push_back(std::move(ms));
    }
    return hadDismember;
}

// 従来シグネチャ（ボーン不要の呼び出し用）。ボーンは捨てる薄いラッパー。
bool ReadNifPerMeshSlots(const std::string& nifFullPath, std::vector<MeshSlot>& outMeshes) {
    std::vector<ShapeBone> dummy;
    return ReadNifPerMeshSlotsAndBones(nifFullPath, outMeshes, dummy);
}

// ★スケルトン解決パス: ';' 区切りのスケルトン NIF パス列を読み、全 NiNode 名を outNames に集める。
//   各パスは ConstructSafePath（InputRoot 相対）→ 絶対パス（そのまま）の順で解決を試みる。
//   いずれか1つでも NIF が読めれば true。outResolvedId に実際に読めたパスを ';' 連結で返す。
bool BuildSkeletonBoneSet(const std::string& semicolonPaths, std::set<std::string>& outNames,
                          std::string& outResolvedId) {
    outNames.clear();
    outResolvedId.clear();
    if (semicolonPaths.empty()) return false;
    bool anyLoaded = false;
    std::stringstream ss(semicolonPaths);
    std::string one;
    while (std::getline(ss, one, ';')) {
        // 前後空白を除去
        size_t a = one.find_first_not_of(" \t");
        size_t b = one.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        std::string rel = one.substr(a, b - a + 1);
        if (rel.empty()) continue;

        // 候補パス: そのまま（絶対）／InputRoot 相対の両方を試す。
        std::vector<fs::path> candidates;
        candidates.push_back(fs::path(rel));
        if (strlen(g_InputRootPath) > 0) candidates.push_back(ConstructSafePath(g_InputRootPath, rel));

        nifly::NifFile skelNif;
        bool loaded = false;
        std::string usedPath;
        for (const auto& c : candidates) {
            std::error_code ec;
            if (!fs::exists(c, ec)) continue;
            if (SafeNifLoad(skelNif, c) == 0) { loaded = true; usedPath = c.string(); break; }
        }
        if (!loaded) {
            AddLog("Skeleton resolve: could not read '" + rel + "' (tried direct + InputRoot-relative).", LogType::Warning);
            continue;
        }
        anyLoaded = true;
        if (!outResolvedId.empty()) outResolvedId += ";";
        outResolvedId += usedPath;
        // 全 NiNode 名を集合へ（厳密一致・大小区別を保持）。
        for (auto* node : skelNif.GetNodes()) {
            if (!node) continue;
            std::string nm = node->name.get();
            if (!nm.empty()) outNames.insert(nm);
        }
    }
    return anyLoaded;
}

// ★スケルトン解決パス: rec.shapeBones を boneSet に対し判定して rec.boneRes を埋める。
//   skeletonId 空（スケルトン未指定/読込不可）→ resolved=null（note に理由）＋ボーン抽出自体は維持。
void ComputeBoneResolution(SlotRecord& rec, const std::set<std::string>& boneSet,
                           const std::string& skeletonId, const std::string& genderLabel) {
    BoneResolution& br = rec.boneRes;
    br = BoneResolution{};
    br.computed = true;
    br.gender = genderLabel;
    br.skeleton = skeletonId;

    // 未スキン（リジッド）シェイプ名を収集（変形しない＝利用側で区別が要る）。
    for (const auto& sb : rec.shapeBones)
        if (!sb.hasSkin) br.unskinnedShapes.push_back(sb.name);

    if (skeletonId.empty() || boneSet.empty()) {
        br.resolved = -1; // null
        br.note = "skeleton not configured or unreadable; bones extracted but not resolved";
        return;
    }

    // 全スキンドシェイプの全ボーンが boneSet に存在するか。欠落は和集合で。
    std::set<std::string> missing;
    for (const auto& sb : rec.shapeBones) {
        if (!sb.hasSkin) continue;
        for (const auto& bn : sb.bones)
            if (!boneSet.count(bn)) missing.insert(bn);
    }
    br.missingBones.assign(missing.begin(), missing.end());
    br.resolved = missing.empty() ? 1 : 0;
}

// ★Force Overwrite: 上書き前にオリジナルを .bak へ退避する（既存 .bak は保持＝最初の1回だけ作る）。
static void BackupFileOnce(const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(target, ec)) return;
    fs::path bak = target; bak += ".bak";
    if (fs::exists(bak, ec)) return; // 既存 .bak は pristine として保持
    fs::copy_file(target, bak, ec);
    if (!ec) { if (LogVerbose()) AddLog("Backup: " + bak.string(), LogType::Info); }
    else AddLog("Backup failed (" + bak.string() + "): " + ec.message(), LogType::Warning);
}

// ★BSD→NiSkin: 指定 shape の BSDismemberSkinInstance を NiSkinInstance に置換する。
//   新規 NiSkinInstance に bone/data/skinPartition/target を引き継ぎ、AddBlock→shape を付け替え→旧 BSD を DeleteBlock。
//   nifly の DeleteBlock が全参照番号を補正する。BSD でなければ（既に NiSkin 等）false。
bool ConvertShapeToNiSkin(nifly::NifFile& nif, int shapeIndex) {
    try {
        auto shapes = nif.GetShapes();
        if (shapeIndex < 0 || shapeIndex >= (int)shapes.size()) return false;
        auto* bs = dynamic_cast<nifly::BSTriShape*>(shapes[shapeIndex]);
        if (!bs) return false;
        auto* skinRef = bs->SkinInstanceRef();
        if (!skinRef || skinRef->IsEmpty()) return false;
        uint32_t oldIdx = skinRef->index;
        auto* skin = nif.GetHeader().GetBlock<nifly::NiObject>(oldIdx);
        auto* bsd = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin);
        if (!bsd) return false; // BSD でない（NiSkin/別型）→ 変換不要/不可

        auto newSkin = std::make_unique<nifly::NiSkinInstance>();
        newSkin->boneRefs = bsd->boneRefs;                 // ボーン一覧（NiBoneContainer）
        newSkin->dataRef = bsd->dataRef;                   // NiSkinData
        newSkin->skinPartitionRef = bsd->skinPartitionRef; // NiSkinPartition（ジオメトリ側・無傷）
        newSkin->targetRef = bsd->targetRef;               // skeleton root

        uint32_t newIdx = nif.GetHeader().AddBlock(std::move(newSkin));
        skinRef->index = newIdx;            // shape を新 NiSkinInstance へ
        nif.GetHeader().DeleteBlock(oldIdx); // 旧 BSD を削除（参照番号は nifly が自動補正）
        return true;
    }
    catch (...) { return false; }
}

// ★NiSkin→BSD: 指定 shape を BSDismemberSkinInstance に変換し、全三角形を1パーティション（slot）に割当てる。
//   nifly の SetShapePartitions(convertSkinInstance=true) が NiSkinInstance を BSD に変換してくれる。
bool ConvertShapeToBSD(nifly::NifFile& nif, int shapeIndex, int slot) {
    try {
        auto shapes = nif.GetShapes();
        if (shapeIndex < 0 || shapeIndex >= (int)shapes.size()) return false;
        auto* shape = shapes[shapeIndex];
        if (!shape) return false;

        nifly::NiVector<nifly::BSDismemberSkinInstance::PartitionInfo> partInfo;
        nifly::BSDismemberSkinInstance::PartitionInfo pi;
        pi.flags = (nifly::PartitionFlags)(nifly::PF_EDITOR_VISIBLE | nifly::PF_START_NET_BONESET);
        pi.partID = (uint16_t)slot;
        partInfo.push_back(pi);

        uint32_t triCount = shape->GetNumTriangles();
        std::vector<int> triParts((size_t)triCount, 0); // 全三角形を partition 0 へ
        nif.SetShapePartitions(shape, partInfo, triParts, true); // convertSkinInstance=true
        return true;
    }
    catch (...) { return false; }
}

// ★#4 共有: 読み込み済み NIF に rec の per-mesh（無ければ armaSlots=union）スロットを適用する。
//   ExportSingleNif（Pending 統合 Export）と ExportOSPWorker（OSP 出力）の両方が使い、per-mesh /
//   toNiSkin / NiSkin↔BSD 変換と SEH 保護（SafeWritePartitions）を一貫させる。
//   戻り値 = 何か書込/変換したか。outMismatch = slots が partition 数を超過 or 書込中クラッシュ
//   （いずれも呼び出し側は「保存中止＝手動チェック要」として扱う）。
bool ApplyPerMeshSlotsToNif(nifly::NifFile& nif, const SlotRecord& rec, bool& outMismatch) {
    outMismatch = false;
    std::vector<int> unionSlots = ParseSlotString(rec.armaSlots); // フォールバック（meshes 空のレコード用）
    bool usePerMesh = !rec.meshes.empty();
    bool anyWritten = false;

    auto iequals = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        return std::equal(a.begin(), a.end(), b.begin(),
            [](char x, char y) { return ::tolower((unsigned char)x) == ::tolower((unsigned char)y); });
    };

    if (!usePerMesh && unionSlots.empty()) return false;

    auto shapes = nif.GetShapes();
    for (size_t k = 0; k < shapes.size(); ++k) {
        auto bs = dynamic_cast<nifly::BSTriShape*>(shapes[k]);
        if (!bs) continue;
        std::string meshName = bs->name.get();

        // この shape に書く slots（per-mesh: shapeIndex 主・name 副 / フォールバック: union）。
        std::vector<int> slotsForShape;
        if (usePerMesh) {
            const MeshSlot* ms = nullptr;
            for (const auto& m : rec.meshes) if (m.shapeIndex == (int)k) { ms = &m; break; }
            if (!ms) for (const auto& m : rec.meshes) if (iequals(m.name, meshName)) { ms = &m; break; }
            if (!ms) { if (LogVerbose()) AddLog("  no per-mesh entry for shape: " + meshName + " (left as-is)", LogType::Info); continue; }
            // ★BSD→NiSkin: フラグ付き shape は NiSkin へ変換（スロット書込はしない）。
            if (ms->toNiSkin) {
                if (ConvertShapeToNiSkin(nif, (int)k)) { anyWritten = true; if (LogVerbose()) AddLog("  converted to NiSkin: " + meshName, LogType::Info); }
                else AddLog("  NiSkin convert skipped (not BSD?): " + meshName, LogType::Warning);
                continue;
            }
            slotsForShape = ms->slots;
        }
        else {
            slotsForShape = unionSlots;
        }
        if (slotsForShape.empty()) continue;

        auto skinRef = bs->SkinInstanceRef();
        if (skinRef->IsEmpty()) { if (LogVerbose()) AddLog("  mesh has no SkinInstanceRef: " + meshName, LogType::Info); continue; }
        auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
        if (!skin) { if (LogVerbose()) AddLog("  skin block null for: " + meshName, LogType::Warning); continue; }
        auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin);
        if (!dis) {
            // ★元が NiSkin で per-mesh スロット指定あり → NiSkin→BSD 変換（スロットは変換時に付与）。
            if (usePerMesh && !slotsForShape.empty()) {
                if (ConvertShapeToBSD(nif, (int)k, slotsForShape[0])) {
                    anyWritten = true;
                    if (LogVerbose()) AddLog("  converted NiSkin->BSD (slot " + std::to_string(slotsForShape[0]) + "): " + meshName, LogType::Info);
                }
                else AddLog("  NiSkin->BSD convert failed: " + meshName, LogType::Warning);
            }
            else if (LogVerbose()) AddLog("  not a BSDismemberSkinInstance: " + meshName, LogType::Info);
            continue;
        }

        size_t partCount = dis->partitions.size();
        if (LogVerbose()) AddLog("  partitions: " + std::to_string(partCount) + ", slots: " + std::to_string(slotsForShape.size()) + " on '" + meshName + "'", LogType::Info);
        // ★⑤ 保険 mismatch: per-mesh slots がその shape の partition 数を超える場合のみ。
        if (slotsForShape.size() > partCount) {
            AddLog("  SLOT MISMATCH: slots(" + std::to_string(slotsForShape.size()) + ") > NIF partitions(" + std::to_string(partCount) + ") on mesh '" + meshName + "'", LogType::Error);
            outMismatch = true;
            return anyWritten;
        }
        int writeCount = (int)std::min(partCount, slotsForShape.size());
        if (writeCount > 0) {
            if (SafeWritePartitions(dis, slotsForShape.data(), writeCount)) anyWritten = true;
            else { AddLog("  crash during partition write (skipped file): " + meshName, LogType::Error); outMismatch = true; return anyWritten; }
        }
    }
    return anyWritten;
}

void SaveAndExportAllWorker(bool doNif, bool doEsl, bool doTxt) {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;
    // ★A1: detach ワーカーから C++ 例外が抜けると std::terminate。本体全体を try/catch で囲い、
    //   いずれの脱出経路でも g_IsProcessing を下ろす（Guard）。
    struct Guard { ~Guard() { g_IsProcessing = false; } } _seGuard;
    try {

    if (!doNif && !doEsl && !doTxt) {
        AddLog("Export: nothing selected (NIF/ESL/txt all off).", LogType::Warning);
        g_IsProcessing = false;
        return;
    }
    AddLog(std::string("Starting Export: ") + (doNif ? "NIF " : "") + (doEsl ? "ESL " : "") + (doTxt ? "txt" : ""), LogType::Info);

    // 1. pending をコピー
    std::map<std::string, SlotRecord> pendingCopy;
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        pendingCopy = g_SessionChanges;
    }

    if (pendingCopy.empty()) {
        AddLog("No pending changes to export.", LogType::Warning);
        g_IsProcessing = false;
        return;
    }

    // 2. NIF出力 -> 成功したレコードのみ後で保存する
    int success = 0;   // 実書き出し成功 + pendingOnly
    int dupSkip = 0;   // 同 NIF を別レコードが出力済み（成功扱い）
    int noPart = 0;    // パーティション無し NiSkin/静的（成功扱い）
    int mismatch = 0;  // パーティション数不一致（手動チェック要・pending 残す）
    int fail = 0;      // 真の失敗（ロード/保存失敗・例外）
    int current = 0;
    int total = (int)pendingCopy.size();

    std::set<std::string> processedPaths;
    std::set<std::string> successfulKeys; // 保存対象キー

    // ExportSingleNif: inPath を基に NIF を出力し、結果ステータスを返す。
    //   Duplicate（同 NIF を別レコードが出力済み）/ NoPartition（NiSkin 等）/ PendingOnly は成功扱い。
    auto ExportSingleNif = [&](const std::string& inPathStr, const SlotRecord& rec, bool isOsp, const std::string& recKey) -> NifExportStatus {
         fs::path inPath(inPathStr);
         std::string lowerPath = inPath.generic_string();
         AsciiLowerInplace(lowerPath); // ★B5: 重複出力検出キー（パス）

        if (processedPaths.count(lowerPath)) {
             if (LogVerbose()) AddLog("Skipping duplicate path (already exported by sibling record): " + inPath.string(), LogType::Info);
             return NifExportStatus::DupSkip;
         }
         processedPaths.insert(lowerPath);

         fs::path outPath;
         if (g_ForceOverwrite) {
             // ★Force Overwrite: Output Root を無視し、オリジナル（入力 NIF）に直接上書き（.bak は保存直前に作る）。
             outPath = inPath;
         }
         else if (strlen(g_OutputRootPath) > 0) {
             fs::path outRoot(g_OutputRootPath);
             if (isOsp) {
                 fs::path rel = GetPathFromFolder(inPath, "shapedata");
                 outPath = outRoot / "CalienteTools" / "BodySlide" / rel;
             }
             else {
                 fs::path rel = GetPathFromFolder(inPath, "meshes");
                 outPath = outRoot / rel;
             }
         }
         else {
             outPath = inPath;
         }

         {
             std::lock_guard<std::mutex> lock(g_ProgressMutex);
             g_CurrentProcessItem = "Exporting: " + outPath.filename().string();
         }

         try {
            // pendingOnly の場合は NIF 書き込みを行わず成功扱い（slotdata への反映対象にする）
            if (rec.pendingOnly) {
                if (LogVerbose()) AddLog("pendingOnly record (no NIF write), marked success: " + inPath.string(), LogType::Info);
                return NifExportStatus::PendingOnly;
            }

             if (outPath.has_parent_path()) fs::create_directories(outPath.parent_path());

             if (LogVerbose()) AddLog("ExportSingleNif: in=" + inPath.string() + " out=" + outPath.string() + " slots=" + rec.armaSlots, LogType::Info);

             nifly::NifFile nif;
             if (SafeNifLoad(nif, inPath) == 0) {
                 if (LogVerbose()) AddLog("Loaded NIF: " + inPath.string(), LogType::Info);
                 // ★#4: per-mesh / union 適用は共有関数に一本化（OSP 経路と同一ロジック・SEH 保護）。
                 bool skipFile = false;
                 bool anyWritten = ApplyPerMeshSlotsToNif(nif, rec, skipFile);

                // ★分類は skipFile を最優先で判定する（旧: !anyWritten を先に見ていたため、
                //   最初のメッシュで mismatch すると anyWritten=false で NoPartition に誤分類され、
                //   MANUAL CHECK が出なかった）。
                if (skipFile) {
                    // パーティション数不一致 → 強調＋手動チェック要（NIF は書かない）。
                    AddLog("==> SLOT MISMATCH, MANUAL CHECK REQUIRED: " + inPath.string()
                        + " (DB slot count exceeds NIF partitions; NOT written)", LogType::Error);
                    return NifExportStatus::Mismatch;
                }
                else if (anyWritten) {
                    if (g_ForceOverwrite) BackupFileOnce(outPath); // ★上書き前にオリジナルを退避
                    if (SafeNifSave(nif, outPath)) {
                        AddLog(std::string("Exported (NIF) -> ") + outPath.string(), LogType::Success);
                        return NifExportStatus::Exported;
                    }
                    AddLog("Crash/exception during NIF save (skipped): " + outPath.string(), LogType::Error);
                    return NifExportStatus::Failed;
                }
                else {
                    // パーティション無し（NiSkin/静的）。スロット編集対象外なので成功扱い（ESL/txt は別途出る）。
                    AddLog("No partitions (NiSkin/static), nothing to write: " + inPath.string(), LogType::Info);
                    return NifExportStatus::NoPartition;
                }
             }
             else {
                 AddLog("Skipping: source is not a binary NIF or failed to load: " + inPath.string(), LogType::Warning);
                 return NifExportStatus::Failed;
             }
         }
         catch (const std::exception& ex) {
             AddLog(std::string("Export exception for ") + inPath.string() + ": " + ex.what(), LogType::Error);
             return NifExportStatus::Failed;
         }
         catch (...) {
             AddLog(std::string("Unknown export exception for ") + inPath.string(), LogType::Error);
             return NifExportStatus::Failed;
         }
         };

    // メインループ（NIF 出力は doNif のときのみ。for 全体が if の対象）
    if (doNif)
    for (const auto& [key, rec] : pendingCopy) {
        if (g_CancelRequested) break;
        current++;
        g_Progress = (float)current / (float)total;

        std::string targetPath = rec.originalNifPath;
        if (targetPath.empty() && !rec.isOspSource) {
            targetPath = (g_TargetGender == 0) ? rec.malePath : rec.femalePath;
            if (targetPath.empty()) targetPath = (g_TargetGender == 0) ? rec.femalePath : rec.malePath;
            targetPath = ConstructSafePath(g_InputRootPath, targetPath).string();
        }
        if (targetPath.empty()) continue;

        // rec を渡して pendingOnly フラグを参照できるようにする
        NifExportStatus st = ExportSingleNif(targetPath, rec, rec.isOspSource, key);
        // 成功扱い（Exported/PendingOnly/DupSkip/NoPartition）→ カウント＋クリア対象。
        // Mismatch/Failed → pending に残す。
        bool handledOk = false;
        switch (st) {
        case NifExportStatus::Exported:    success++;  handledOk = true; break;
        case NifExportStatus::PendingOnly: success++;  handledOk = true; break;
        case NifExportStatus::DupSkip:     dupSkip++;  handledOk = true; break;
        case NifExportStatus::NoPartition: noPart++;   handledOk = true; break;
        case NifExportStatus::Mismatch:    mismatch++; break;
        case NifExportStatus::Failed:      fail++;     break;
        }
        if (handledOk) successfulKeys.insert(key);

        // ペアファイルの自動検出とエクスポート (OSPでない場合のみ)。
        //   ペアが Mismatch/Failed のときだけ警告（dup/noPart は正常）。
        if (handledOk && !rec.isOspSource) {
            fs::path p(targetPath);
            std::string stem = p.stem().string();
            std::string ext = p.extension().string();
            std::string pairName = "";
            if (stem.size() >= 2) {
                if (stem.substr(stem.size() - 2) == "_0") pairName = stem.substr(0, stem.size() - 2) + "_1" + ext;
                else if (stem.substr(stem.size() - 2) == "_1") pairName = stem.substr(0, stem.size() - 2) + "_0" + ext;
            }
            if (!pairName.empty()) {
                fs::path pairPath = p.parent_path() / pairName;
                if (fs::exists(pairPath)) {
                    NifExportStatus pst = ExportSingleNif(pairPath.string(), rec, false, key);
                    if (pst == NifExportStatus::Mismatch)
                        AddLog("==> Pair SLOT MISMATCH, MANUAL CHECK: " + pairPath.string(), LogType::Error);
                    else if (pst == NifExportStatus::Failed)
                        AddLog("Pair export failed for: " + pairPath.string(), LogType::Warning);
                }
            }
        }
    }

    // ★統合 Export: 選択された出力を実行し、成功分の pending をクリアする。
    //   NIF 成否集合: doNif なら成功キー、!doNif なら全 pending を「NIF OK」扱い。
    std::set<std::string> nifOkKeys;
    if (doNif) nifOkKeys = successfulKeys;
    else for (const auto& [k, rec] : pendingCopy) { (void)rec; nifOkKeys.insert(k); }

    // txt: slotdata-Output.txt を更新（スロット決定の記録。NIF 成否に依らず全 pending を書く）。
    if (doTxt && !g_CancelRequested) {
        SaveSessionChangesToFileFiltered(pendingCopy);
    }

    // esl: override-only ESL パッチを生成（全 pending・all-or-nothing）。
    bool eslOk = true;
    if (doEsl && !g_CancelRequested) {
        eslOk = RunSlotPatchImport(pendingCopy);
    }

    // クリア: キャンセルされず、(esl 未選択 or esl 成功) のとき、NIF が OK だったレコードを除去。
    //   NIF 失敗分は再試行できるよう pending に残す。
    if (!g_CancelRequested && (!doEsl || eslOk)) {
        int cleared = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
            for (const auto& [k, rec] : pendingCopy) {
                (void)rec;
                if (nifOkKeys.count(k) && g_SessionChanges.count(k)) { g_SessionChanges.erase(k); ++cleared; }
            }
        }
        // NIF 成功計上 = 実書き出し + 重複スキップ + パーティション無し。
        int nifOkTotal = success + dupSkip + noPart;
        AddLog("Export done. "
            + std::string(doNif ? ("NIF ok=" + std::to_string(nifOkTotal)
                + " (written=" + std::to_string(success)
                + ", skip-dup=" + std::to_string(dupSkip)
                + ", no-partition=" + std::to_string(noPart) + ")"
                + ", fail=" + std::to_string(fail) + " ") : "")
            + (doEsl ? std::string("| ESL=") + (eslOk ? "ok " : "FAIL ") : "")
            + (doTxt ? "| txt-updated " : "")
            + "| cleared " + std::to_string(cleared) + " pending.", LogType::Success);
        // mismatch は強調して別出力（手動チェックを促す）。
        if (doNif && mismatch > 0)
            AddLog("*** " + std::to_string(mismatch) + " NIF(s) had a SLOT MISMATCH and were NOT written. "
                "Check them MANUALLY (search 'SLOT MISMATCH' in the log). They remain in pending. ***", LogType::Error);
    }
    else if (g_CancelRequested) {
        AddLog("Export Cancelled.", LogType::Warning);
    }
    else {
        AddLog("Export: ESL generation FAILED; pending kept for retry (NIF/txt may already be written).", LogType::Warning);
    }
    g_IsProcessing = false;
    }
    catch (const std::exception& e) { AddLog(std::string("Export worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("Export worker unknown error.", LogType::Error); }
}


// =========================================================================
// ★#2: DB slot vs NIF パーティション slot の不一致スキャン
//   全 DB レコードの NIF を読み、BSDismemberSkinInstance のパーティション partID 集合と
//   DB の armaSlots を比較。異なれば mismatch。NiSkin（dismember 無し）は除外。
//   NIF ロードは path 単位でキャッシュ（男女・カラバリで共有 NIF を再読込しない）。
// =========================================================================
void ScanNifSlotsWorker() {
    g_IsProcessing = true;
    g_CancelRequested = false;
    g_Progress = 0.0f;
    // ★A1: ワーカー全体を try/catch で囲い、いずれの脱出でも g_IsProcessing を下ろす。
    struct Guard { ~Guard() { g_IsProcessing = false; } } _seGuard;
    try {
    AddLog("Scanning NIF partition slots vs DB slots...", LogType::Info);

    // レコードのスナップショット（id, armaSlots, male/female path）。
    std::vector<SlotRecord> recs;
    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        recs = g_AllRecords;
    }

    // path -> (hasDismember, slots)。NIF ロード結果のキャッシュ。
    std::map<std::string, std::pair<bool, std::set<int>>> nifCache;
    std::set<int> mismatches;
    int total = (int)recs.size();
    int cur = 0, scanned = 0, skippedNoPath = 0, skippedNoLoad = 0, niSkin = 0;

    for (const auto& r : recs) {
        if (g_CancelRequested) break;
        ++cur;
        g_Progress = total > 0 ? (float)cur / (float)total : 1.0f;

        // 性別優先で NIF パスを解決（DB 表示と同じ規則）。
        std::string rel = (g_TargetGender == 0) ? r.malePath : r.femalePath;
        if (rel.empty()) rel = (g_TargetGender == 0) ? r.femalePath : r.malePath;
        if (rel.empty() && !r.originalNifPath.empty()) rel = r.originalNifPath;
        if (rel.empty()) { ++skippedNoPath; continue; }
        std::string full = ConstructSafePath(g_InputRootPath, rel).string();

        bool hasDis = false;
        std::set<int> nifSlots;
        auto it = nifCache.find(full);
        if (it != nifCache.end()) {
            hasDis = it->second.first;
            nifSlots = it->second.second;
        }
        else {
            nifly::NifFile nif;
            if (SafeNifLoad(nif, fs::path(full)) == 0) {
                for (auto* s : nif.GetShapes()) {
                    auto* bs = dynamic_cast<nifly::BSTriShape*>(s);
                    if (!bs) continue;
                    auto skinRef = bs->SkinInstanceRef();
                    if (skinRef->IsEmpty()) continue;
                    auto skin = nif.GetHeader().GetBlock<nifly::NiObject>(skinRef->index);
                    if (auto dis = dynamic_cast<nifly::BSDismemberSkinInstance*>(skin)) {
                        hasDis = true;
                        for (const auto& p : dis->partitions) nifSlots.insert((int)p.partID);
                    }
                }
            }
            else { ++skippedNoLoad; }
            nifCache[full] = { hasDis, nifSlots };
        }

        if (!hasDis) { ++niSkin; continue; } // NiSkin / パーティション無し → 判定対象外

        std::vector<int> dbV = ParseSlotString(r.armaSlots);
        std::set<int> dbSlots(dbV.begin(), dbV.end());
        if (dbSlots.empty()) continue; // DB 側にスロット情報が無い → 比較不能（ノイズ回避のため除外）
        ++scanned;
        // ★per-mesh モデル: armaSlots はブロックスロット（ジオメトリ無し）を含みうるので、
        //   「完全一致」では恒常的に mismatch になる。真の不整合は「NIF パーティション slot が
        //   armaSlots に含まれない」場合（= ESP が宣言していない geometry slot が NIF にある）。
        bool realMismatch = false;
        for (int s : nifSlots) if (!dbSlots.count(s)) { realMismatch = true; break; }
        if (realMismatch) mismatches.insert(r.id);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_DataMutex);
        g_SlotMismatchRecords.swap(mismatches);
    }
    AddLog("Slot scan done: " + std::to_string((int)g_SlotMismatchRecords.size()) + " mismatch(es) of "
        + std::to_string(scanned) + " checked (NiSkin skipped: " + std::to_string(niSkin)
        + ", no-path: " + std::to_string(skippedNoPath) + ", load-fail: " + std::to_string(skippedNoLoad) + ").",
        LogType::Success);
    g_IsProcessing = false;
    }
    catch (const std::exception& e) { AddLog(std::string("Scan worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("Scan worker unknown error.", LogType::Error); }
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
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Nif Slot Sniper v2.0.0 (per-mesh)", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    g_ShaderProgram = CreateShader(vertexShaderSource, fragmentShaderSource);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // ★#6: imgui.ini も実行ファイルのディレクトリ基準（CWD 非依存）。静的に保持して寿命を確保。
    static std::string s_imguiIniPath = AppPath("imgui.ini");
    io.IniFilename = s_imguiIniPath.c_str();
    // ★コンテンツ部のドラッグでウィンドウが動くのを防ぐ（タイトルバーからのみ移動）。
    //   3D Viewport 上でのドラッグがウィンドウ移動になる問題への対策も兼ねる。
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    ImGui::StyleColorsDark();

    ImVec4 clear_color = ImVec4(0.2f, 0.2f, 0.25f, 1.00f);
    bool g_RequestInitialSetup = false;
    LoadUnifiedConfig();
    if (strlen(g_GameDataPath) == 0 || strlen(g_InputRootPath) == 0) {
        g_RequestInitialSetup = true;
    }

    SlotDictionary::LoadRules(); // ★修正: コンボのデフォルトは LoadRules 内で「空のときのみ」適用する

    AddLog("===== NifSlotSniper session start =====", LogType::Info);
    // ★旧版の起動時 slotdata 自動読込は撤去（DB は「Import DB (slottool)」で構築する）。
    //   slotdata-Output.txt はバッチ蓄積として残し、「Load slotdata → Pending」で読み込む。
    AddLog("DB is empty. Use 'Import DB (slottool)' to load the current load order.", LogType::Info);

    // ★#5: 記憶した参照ボディを起動時に自動読込（表示は OFF のまま。Show Ref で出す）。
    if (strlen(g_RefBodyPath) > 0 && fs::exists(g_RefBodyPath)) {
        LoadReferenceBody(g_RefBodyPath);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // ★3D Viewport にマウスがある時だけカメラ操作を受け付ける（背景全画面ではなくパネル基準）
        if (g_ViewportHovered) {
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

        // ★A: バックグラウンド Import DB の結果をメインスレッドで適用（構造変更は描画と非競合に）
        if (g_ImportResultReady.load()) {
            std::map<std::string, SlotRecord> result;
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                g_ImportResult.swap(result);
            }
            // ★スレッド安全: g_AllRecords/g_DisplayTree/g_RecordSelectionMap の作り直しは
            //   detach ワーカー（BatchExport/Scan/OSP）の読みと競合するため g_DataMutex 下で行う。
            {
                std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
                g_DisplayTree.clear();
                g_AllRecords.clear();
                g_RecordSelectionMap.clear();
                BuildRecordsAndTree(result);
            }
            g_ImportResultReady = false;
            AddLog("DB applied: " + std::to_string(g_AllRecords.size()) + " records via slottool.", LogType::Success);
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
            //ImGui::EndMenu();
            if (ImGui::MenuItem("KID Generator")) g_ShowKIDGeneratorWindow = true;
            
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

        // --- 3D Viewport（FBO をパネル内に表示。拡縮等の入力はこのパネルにスコープ） ---
        {
            ImVec2 _p, _s; GetMainPanelRect(MainPanel::Viewport, _p, _s);
            ImGui::SetNextWindowPos(_p, ImGuiCond_Always);
            ImGui::SetNextWindowSize(_s, ImGuiCond_Always);
        }
        ImGui::Begin("3D Viewport", nullptr, PIN_PANEL_FLAGS);
        ImGui::TextDisabled("L-Click=Rotate | Shift+L-Click=Pan | Wheel=Zoom | Space=Reset");
        ImVec2 viewAvail = ImGui::GetContentRegionAvail();
        g_PanelW = (int)viewAvail.x;
        g_PanelH = (int)viewAvail.y;
        if (g_ViewColor)
            ImGui::Image((ImTextureID)(intptr_t)g_ViewColor, viewAvail, ImVec2(0, 1), ImVec2(1, 0)); // V反転
        g_ViewportHovered = (g_ViewColor != 0) && ImGui::IsItemHovered();
        // ホイールズーム（Viewport 上のときのみ）— これが従来効かなかったズームの修正点
        if (g_ViewportHovered) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                g_CamDistance *= (1.0f - wheel * 0.1f);
                if (g_CamDistance < 1.0f) g_CamDistance = 1.0f;
                if (g_CamDistance > 5000.0f) g_CamDistance = 5000.0f;
            }
        }
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
        //RenderKidGenerator();

        RenderKidGenerator();

		// 7. デバッグログウィンドウ
        RenderAnalysisDetailsLog();

        // ---------------------------------------------------------
        // ログウィンドウ (短いのでここにあってもOKですが、必要なら分離可)
        // ---------------------------------------------------------
        {
            ImVec2 _p, _s; GetMainPanelRect(MainPanel::LogConsole, _p, _s);
            ImGui::SetNextWindowPos(_p, ImGuiCond_Always);
            ImGui::SetNextWindowSize(_s, ImGuiCond_Always);
            ImGui::Begin("Log Console", nullptr, PIN_PANEL_FLAGS);
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
            bool cancelable = g_ProcessCancelable.load();
            ImGui::Text("Processing... Please Wait");
            ImGui::Separator();

            // 現在の処理対象を表示 (排他制御)。BSA/Import はここに具体名が出る。
            {
                std::lock_guard<std::mutex> lock(g_ProgressMutex);
                ImGui::TextWrapped("%s", g_CurrentProcessItem.c_str());
            }

            // プログレスバー。キャンセル不可（BSA/Import 等の不定長）はアニメーション表示。
            double _t = ImGui::GetTime();
            float frac = cancelable ? g_Progress.load() : (float)(_t - (double)(long long)_t);
            ImGui::ProgressBar(frac, ImVec2(300, 20), cancelable ? nullptr : "working...");

            ImGui::Spacing();
            ImGui::Separator();

            // キャンセルボタン（可能な処理のみ）
            if (cancelable) {
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    g_CancelRequested = true;
                    std::lock_guard<std::mutex> lock(g_ProgressMutex);
                    g_CurrentProcessItem = "Canceling...";
                }
            }
            else {
                ImGui::TextDisabled("This operation cannot be canceled.");
            }

            // 処理が終わったら閉じる
            if (!g_IsProcessing) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        // --- Rendering ---
        ImGui::Render();

        // ★3D を Viewport FBO に描画（パネル内寸）。既定FBではなくオフスクリーンへ。
        EnsureViewportFBO(g_PanelW, g_PanelH);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ViewFBO);
        glViewport(0, 0, g_ViewW, g_ViewH);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(g_ShaderProgram); // ★シェーダーを有効化

        // --- Projection & View 行列 (アスペクトは Viewport パネル基準) ---
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), (float)g_ViewW / (float)g_ViewH, 0.1f, 10000.0f);
        glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(g_CamOffset.x, g_CamOffset.y, -g_CamDistance));

        // --- Model 行列と中心点の決定 ---
        glm::mat4 model = glm::mat4(1.0f);

        // 回転 (変更なし)
        model = glm::rotate(model, glm::radians(g_ModelRotation[0]), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(g_ModelRotation[1] + 180), glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(g_ModelRotation[2]), glm::vec3(0, 0, 1));
        model = glm::rotate(model, glm::radians(-90.0f), glm::vec3(1, 0, 0));

        // ★★★ カメラ／回転中心の決定（g_CamFocus を尊重） ★★★
        glm::vec3 targetCenter = g_BodyCenter;
        const bool hasRefBody = (g_ShowRef && !g_RefRenderMeshes.empty());

        switch (g_CamFocus) {
        case CamFocus::Auto:
            if (hasRefBody) targetCenter = g_RefBodyCenter;
            else if (g_SelectedMeshIndex >= 0 && g_SelectedMeshIndex < static_cast<int>(g_RenderMeshes.size()))
                targetCenter = g_RenderMeshes[g_SelectedMeshIndex].center;
            break;
        case CamFocus::Nif:
            targetCenter = g_BodyCenter;
            break;
        case CamFocus::Ref:
            if (hasRefBody) targetCenter = g_RefBodyCenter;
            break;
        case CamFocus::Mesh:
            if (g_CamTargetMeshIndex >= 0 && g_CamTargetMeshIndex < static_cast<int>(g_RenderMeshes.size()))
                targetCenter = g_RenderMeshes[g_CamTargetMeshIndex].center;
            break;
        }

        if (hasRefBody) targetCenter.z += g_RefCamZOffset;
        // 中心点分だけずらして、回転の中心を合わせる
        model = glm::translate(model, -targetCenter);

        glUniformMatrix4fv(glGetUniformLocation(g_ShaderProgram, "projection"), 1, GL_FALSE, glm::value_ptr(proj));
        glUniformMatrix4fv(glGetUniformLocation(g_ShaderProgram, "view"), 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(g_ShaderProgram, "model"), 1, GL_FALSE, glm::value_ptr(model));
        glUniform3f(glGetUniformLocation(g_ShaderProgram, "lightDir"), 0.5f, 1.0f, 0.3f);
        // ★Tier3: sampler を unit0 に、ブレンド関数を設定
        glUniform1i(glGetUniformLocation(g_ShaderProgram, "diffuseTex"), 0);
        // ★#3: 法線マップ sampler は unit1。カメラのワールド座標を viewPos に渡す（スペキュラ用）。
        glUniform1i(glGetUniformLocation(g_ShaderProgram, "normalTex"), 1);
        glm::vec3 camWorldPos = glm::vec3(glm::inverse(view)[3]);
        glUniform3f(glGetUniformLocation(g_ShaderProgram, "viewPos"), camWorldPos.x, camWorldPos.y, camWorldPos.z);
        // ★#3-debug: 法線マップ基底の切替を毎フレーム反映。
        glUniform1i(glGetUniformLocation(g_ShaderProgram, "nmFlipGreen"), g_NmFlipGreen ? 1 : 0);
        glUniform1i(glGetUniformLocation(g_ShaderProgram, "nmFlipHand"), g_NmFlipHand ? 1 : 0);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // （描画は下の「★A1: 透過2パス」に統一。ref/main を不透明→半透明の順で描く。）

        // ★カラバリ: 選択中レコードの AlternateTextures（gender に応じ male/female）を取得。
        //   テクスチャモード時、3D Index（or shape 名）が一致するメッシュは NIF ベースを上書き。
        const std::vector<AltTex>* altList = nullptr;
        if (g_TextureMode && g_SelectedRecordID >= 0) {
            for (const auto& rec : g_AllRecords) {
                if (rec.id == g_SelectedRecordID) {
                    altList = (g_TargetGender == 1) ? &rec.altFemale : &rec.altMale;
                    break;
                }
            }
        }

        // ★カラバリ: メッシュへの上書き diffuse を解決するヘルパー。
        //   主キー= 3D Index（mesh.shapeIndex 一致）、無ければ shape 名（case-insensitive）。
        auto resolveAltDiffuse = [&](const RenderMesh& m) -> std::string {
            if (!altList) return "";
            for (const auto& a : *altList)            // 主: index 一致
                if (a.index >= 0 && a.index == m.shapeIndex) return a.diffuse;
            for (const auto& a : *altList) {          // フォールバック: 名前一致
                if (a.shape.empty()) continue;
                if (a.shape.size() == m.name.size()
                    && std::equal(a.shape.begin(), a.shape.end(), m.name.begin(),
                        [](char x, char y) { return ::tolower((unsigned char)x) == ::tolower((unsigned char)y); }))
                    return a.diffuse;
            }
            return "";
        };

        // ★診断: 選択レコード/性別が変わったら、カラバリ上書きを 1 回ダンプする。
        if (LogVerbose()) {
            static int s_lastAltRec = -2; static int s_lastAltGender = -1;
            if (g_SelectedRecordID != s_lastAltRec || g_TargetGender != s_lastAltGender) {
                s_lastAltRec = g_SelectedRecordID; s_lastAltGender = g_TargetGender;
                if (altList) {
                    AddLog("[Alt] record " + std::to_string(g_SelectedRecordID)
                        + (g_TargetGender == 1 ? " (Female)" : " (Male)")
                        + ": " + std::to_string(altList->size()) + " override(s).", LogType::Info);
                    for (const auto& a : *altList)
                        AddLog("[Alt]   shape=\"" + a.shape + "\" index=" + std::to_string(a.index) + " -> " + a.diffuse, LogType::Info);
                    for (const auto& m : g_RenderMeshes)
                        AddLog("[Alt]   NIF mesh idx=" + std::to_string(m.shapeIndex) + " name=\"" + m.name + "\"", LogType::Info);
                }
                else if (g_TextureMode && g_SelectedRecordID >= 0) {
                    AddLog("[Alt] record " + std::to_string(g_SelectedRecordID) + ": no overrides (using base diffuse).", LogType::Info);
                }
            }
        }

        // ★A1: 透過描画を正しくするための統一処理。
        //   1 メッシュを描く drawOne（ref/main 共通）。isRef でテクスチャ解決先を切替。
        auto drawOne = [&](const RenderMesh& m, bool isRef, int mainIdx) {
            unsigned int tex = 0;
            if (g_TextureMode) {
                if (isRef) {
                    if (strlen(g_RefTexFolder) > 0 && !m.diffuseTexPath.empty())
                        tex = GetOrLoadTextureFromFolder(g_RefTexFolder, m.diffuseTexPath);
                }
                else {
                    std::string texPath = m.diffuseTexPath;
                    std::string altDiff = resolveAltDiffuse(m); // カラバリ: index 優先で上書き
                    if (!altDiff.empty()) texPath = altDiff;
                    if (!texPath.empty()) tex = GetOrLoadDiffuseTexture(texPath);
                }
            }
            bool textured = (tex != 0);
            glUniform1i(glGetUniformLocation(g_ShaderProgram, "useTexture"), textured ? 1 : 0);
            if (textured) {
                glm::vec3 tint = (!isRef && mainIdx == g_SelectedMeshIndex) ? glm::vec3(1.4f) : glm::vec3(1.0f);
                glUniform3f(glGetUniformLocation(g_ShaderProgram, "tint"), tint.r, tint.g, tint.b);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                // ★#3: 法線マップ。モデルスペース(_msn)は接線スペース適用が誤りなので除外。
                unsigned int ntex = 0;
                if (g_UseNormalMap && m.hasTangents && !m.modelSpaceNormal && !m.normalTexPath.empty())
                    ntex = isRef ? GetOrLoadTextureFromFolder(g_RefTexFolder, m.normalTexPath)
                                 : GetOrLoadDiffuseTexture(m.normalTexPath);
                if (ntex != 0) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, ntex);
                    glActiveTexture(GL_TEXTURE0);
                }
                glUniform1i(glGetUniformLocation(g_ShaderProgram, "useNormal"), ntex != 0 ? 1 : 0);
                // ★#3-alpha: 完全透過部の黒残り対策。
                bool doDiscard = m.alphaTest || m.alphaBlend;
                float thr = m.alphaTest ? m.alphaThreshold : 0.02f;
                glUniform1i(glGetUniformLocation(g_ShaderProgram, "useAlphaTest"), doDiscard ? 1 : 0);
                glUniform1f(glGetUniformLocation(g_ShaderProgram, "alphaThreshold"), thr);
                // 半透明は depth 書き込み OFF（透過2パスのソートに任せる）。
                if (m.alphaBlend) { glEnable(GL_BLEND); glDepthMask(GL_FALSE); }
                else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
            }
            else {
                glm::vec3 c = m.color;
                if (!isRef && mainIdx == g_SelectedMeshIndex) c = glm::vec3(1.2f, 1.2f, 1.2f);
                glUniform3f(glGetUniformLocation(g_ShaderProgram, "objectColor"), c.r, c.g, c.b);
                glUniform1i(glGetUniformLocation(g_ShaderProgram, "useNormal"), 0);
                glUniform1i(glGetUniformLocation(g_ShaderProgram, "useAlphaTest"), 0);
                glDisable(GL_BLEND); glDepthMask(GL_TRUE);
            }
            glBindVertexArray(m.VAO);
            glDrawElements(GL_TRIANGLES, (GLsizei)m.indexCount, GL_UNSIGNED_INT, 0);
        };

        // ★A1: 描画アイテムを収集（ref + main）。半透明 = texture mode かつ alphaBlend。
        struct DrawItem { const RenderMesh* m; bool isRef; int mainIdx; bool transparent; float depth; };
        std::vector<DrawItem> drawItems;
        auto isMeshBlocked = [&](const RenderMesh& m) {
            for (const auto& bw : g_KeywordBlockedList)
                if (!bw.empty() && m.name.find(bw) != std::string::npos) return true;
            return false;
        };
        if (g_ShowRef && !g_RefRenderMeshes.empty())
            for (const auto& m : g_RefRenderMeshes)
                drawItems.push_back({ &m, true, -1, g_TextureMode && m.alphaBlend, 0.0f });
        for (int i = 0; i < (int)g_RenderMeshes.size(); ++i) {
            const auto& m = g_RenderMeshes[i];
            if (isMeshBlocked(m)) continue; // ブロックは非表示
            drawItems.push_back({ &m, false, i, g_TextureMode && m.alphaBlend, 0.0f });
        }

        // ★A1: パス1 = 不透明（depth 書き込みあり）。順序は問わない。
        for (const auto& it : drawItems)
            if (!it.transparent) drawOne(*it.m, it.isRef, it.mainIdx);

        // ★A1: パス2 = 半透明。ビュー空間 Z で奥→手前にソートして描く（depth 書き込みは drawOne 内で OFF）。
        std::vector<const DrawItem*> trans;
        for (const auto& it : drawItems) if (it.transparent) trans.push_back(&it);
        for (auto* it : trans) {
            glm::vec4 vp = view * model * glm::vec4(it->m->center, 1.0f);
            const_cast<DrawItem*>(it)->depth = vp.z; // view 空間 z（負ほど遠い）
        }
        std::sort(trans.begin(), trans.end(),
            [](const DrawItem* a, const DrawItem* b) { return a->depth < b->depth; }); // 遠い(負)が先
        for (auto* it : trans) drawOne(*it->m, it->isRef, it->mainIdx);

        // 3. 後処理 (3D描画の状態リセット)
        glDisable(GL_BLEND); glDepthMask(GL_TRUE); glBindTexture(GL_TEXTURE_2D, 0); // ★Tier3: 状態リセット
        glBindVertexArray(0);

        // ★FBO を解除して既定フレームバッファへ。背景クリア後に ImGui を描画
        //   （ImGui の "3D Viewport" Image が上で描いた FBO テクスチャを表示する）
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        int dw, dh; glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ★A2: detach ワーカーがまだ生きている可能性がある。終了処理（GL/テクスチャ破棄・静的 ofstream の
    //   破棄等）の前にキャンセルを要求し、g_IsProcessing が下りるまで最大 ~5s 待つ。生存中ワーカーの
    //   g_AllRecords/AddLog アクセスと片付けが競合して UB/クラッシュになるのを防ぐ。
    //   （子プロセス待ちは A3 でタイムアウト/キャンセル化済みなので概ね 5s 以内に収束する。）
    g_CancelRequested = true;
    for (int i = 0; i < 100 && g_IsProcessing.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (g_IsProcessing.load())
        AddLog("Shutdown: a background worker did not finish in time; exiting anyway.", LogType::Warning);

    // ★② 終了時の GL リソース解放（GL コンテキストが有効なうちに行う）。
    //   OS もプロセス終了時に回収するが、明示解放で検出ツールのノイズを減らし意図を明確化。
    g_RenderMeshes.clear();      // 各 RenderMesh デストラクタで VAO/VBO/EBO 解放
    g_RefRenderMeshes.clear();
    ClearTextureCache();         // キャッシュ内の全 GL テクスチャ解放
    if (g_ShaderProgram) { glDeleteProgram(g_ShaderProgram); g_ShaderProgram = 0; }
    if (g_ViewColor) { glDeleteTextures(1, &g_ViewColor); g_ViewColor = 0; }
    if (g_ViewDepth) { glDeleteRenderbuffers(1, &g_ViewDepth); g_ViewDepth = 0; }
    if (g_ViewFBO) { glDeleteFramebuffers(1, &g_ViewFBO); g_ViewFBO = 0; }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
};