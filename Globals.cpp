#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#endif

#include "Globals.h"
#include <algorithm>
#include "SlotDictionary.hpp" 
#include <NifFile.hpp>        
#include <imgui.h>            
#include <sstream>            
#include <glad/glad.h>        
#include <GLFW/glfw3.h>       
#include <commdlg.h>  
#include <shlobj.h> 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream> // 追加: std::cout 使用のため
#include <filesystem>
#include <fstream>  // ★ログのファイル出力
#include <chrono>   // ★タイムスタンプ
#include <ctime>    // ★localtime_s / strftime

// ==========================================
// ログ上限（必要なら Globals.h に移動しても良い）
// ==========================================
static constexpr size_t kLogHistoryMax = 1000; // 既定の上限（件）

// ==========================================
// 1. 定数・グローバル変数の「実体」定義
// ==========================================

const std::string CONFIG_FILENAME = "config.ini";

// スレッド制御
std::mutex g_LogMutex;
std::mutex g_ProgressMutex;
std::recursive_mutex g_DataMutex;
std::atomic<bool> g_IsProcessing(false);
std::atomic<bool> g_CancelRequested(false);
std::atomic<float> g_Progress(0.0f);
std::string g_CurrentProcessItem;
// ★A: 非同期化の共有状態
std::atomic<bool> g_ProcessCancelable(true);
std::atomic<bool> g_ImportResultReady(false);
std::map<std::string, SlotRecord> g_ImportResult;

// ログ
std::vector<LogEntry> g_LogHistory;
bool g_LogAutoScroll = true;
bool g_LogScrollToBottom = false;
std::string g_StatusMessage = "Ready";

// パス設定
char g_InputRootPath[4096] = { 0 };
char g_OutputRootPath[4096] = { 0 };
char g_GameDataPath[4096] = { 0 };
char g_SlotDataPath[4096] = { "slotdataTXT" };
char g_SlotToolPath[4096] = { 0 }; // ★Step3: slottool.exe のパス
char g_BsArchPath[4096] = { 0 };   // ★① BSArch.exe のパス
char g_RefBodyPath[4096] = { 0 };  // ★#5: 参照ボディ NIF のパス
char g_RefTexFolder[4096] = { 0 }; // ★#5: 参照ボディ用テクスチャフォルダ
char g_SkeletonPathFemale[4096] = { 0 }; // ★スケルトン解決パス: 女性スケルトン NIF（';' 区切りで複数可）
char g_SkeletonPathMale[4096]   = { 0 }; // ★スケルトン解決パス: 男性スケルトン NIF（';' 区切りで複数可）
/*
char g_KIDTargetBuffer[4096] = { 0 };
char g_KIDResultBuffer[4096] = { 0 };
char g_InputBuffer[1024] = { 0 };
*/
// UIフラグ　不必要なものがあるかも知れない。要確認
bool g_ShowSettingsWindow = false;
bool g_ShowRulesWindow = false;
bool g_ShowDatabaseWindow = true;
bool g_ShowControlPanel = true;
bool g_ShowPendingAreaWindow = true;
bool g_ShowKIDGeneratorWindow = false;
bool g_ShowDebugWindow = true;
bool g_ShowLogWindow = true;
bool g_ShowProgressWindow = false;
bool g_ShowAnalysisDetailsLog = false; // デフォルトは非表示

// 設定リスト
std::vector<KidKeyword> g_KeywordList;
std::vector<std::string> g_SourceBlockedList;
std::vector<std::string> g_KeywordBlockedList;

// KID Generator
char g_KIDTargetBuffer[4096] = "";
char g_KIDResultBuffer[4096] = "";
int g_SelectedKeywordIndex = -1;
ImGuiTextFilter g_KIDKeywordFilter;

// NIF関連
fs::path g_CurrentNifPath = "";
nifly::NifFile g_NifData;
std::vector<RenderMesh> g_RenderMeshes;
int g_SelectedMeshIndex = -1;

fs::path g_PairedNifPath = "";
nifly::NifFile g_PairedNifData;
bool g_HasPairedFile = false;

fs::path g_RefNifPath = "";
nifly::NifFile g_RefNifData;
std::vector<RenderMesh> g_RefRenderMeshes;
bool g_ShowRef = false;

int g_TargetGender = 1; // 0:Male, 1:Female
bool g_TextureMode = false; // ★Tier3: 既定はフラット表示
// ★#2: DB slot と NIF 実パーティションが不一致のレコード id 集合（ScanNifSlotsWorker が更新）。
//   NIF database ツリーで該当 NIF 名をオレンジ表示するのに使う。NiSkin（パーティション無し）は除外。
std::set<int> g_SlotMismatchRecords;
// ★BSD→NiSkin: 現在の NIF で NiSkin 化指定された shapeIndex 集合（LoadNifFileCore でクリア）。
std::set<int> g_ConvertedNiSkinShapes;
// ★統合 Export: 何を出力するかのトグル（config 保存）。既定は全 ON。
bool g_ExportNif = true; // NIF（partition 書換）
bool g_ExportEsl = true; // ESL/ESP パッチ（slottool import）
bool g_ExportTxt = true; // slotdata-Output.txt 更新（バッチ蓄積用）
bool g_ForceOverwrite = false; // ★Force Overwrite: オリジナル NIF/ESP に直接上書き（.bak 付き）。既定 OFF。
bool g_ExportCostumeSeed = false; // ★衣装シード: ON で ChangeSet と同時に costume_seed.json を出力。既定 OFF。
// ★ログレベル: 0=Error / 1=Warning / 2=Info(+Success) / 3=Verbose。既定 2。
int g_LogLevel = 2;
// ★① テクスチャキャッシュ上限（直近 N 个の NIF 分を保持。クリアは NIF 切替時のみ）。
//   ref body 由来（"folder|" キー）は除外して常に保持。0 以下で無制限。
//   ※「枚数」ではなく「NIF 数」。描画中はクリアしないので、1 NIF が何枚要求しても
//     ループしない（旧・枚数方式のループ不具合への対策）。
int g_TexCacheNifLimit = 4;
// ★#3-debug: 法線マップの基底を実機で当てるための切替（既定=有効・反転なし）。
bool g_UseNormalMap = true; // 法線マップ全体の ON/OFF
bool g_NmFlipGreen = false; // グリーン反転（DirectX 規約用）
bool g_NmFlipHand = false;  // ハンドネス(w)反転
int g_NifLoadMode = 0;  // 0:Both, 1:Single, 2:Pair Only

// データベース
std::vector<SlotRecord> g_AllRecords;
int g_DbVersion = 0; // ★DB 版数（BuildRecordsAndTree で増加）
std::map<std::string, std::map<std::string, std::map<std::string, std::vector<int>>>> g_DisplayTree;
std::map<std::string, std::vector<std::string>> g_BodySlideSourceMap;
bool g_BodySlideScanned = false;
std::map<std::string, bool> g_SourceSelectionMap;

// タブ制御
bool g_ForceTabToList = false;
bool g_ForceTabToSource = false;

// 選択状態
std::map<int, bool> g_RecordSelectionMap;
ImGuiTextFilter g_SlotFilter;
int g_SelectedRecordID = -1;
std::string g_PreviewSlotStr = "";

// セッション
std::map<std::string, SlotRecord> g_SessionChanges;

// OpenGL / Camera
unsigned int g_ShaderProgram = 0;
glm::vec3 g_BodyCenter = glm::vec3(0.0f);
glm::vec3 g_RefBodyCenter = glm::vec3(0.0f); // ★追加: リファレンスボディの中心
char g_InputBuffer[1024] = "";
float g_CamDistance = 100.0f;
glm::vec3 g_CamOffset = glm::vec3(0.0f);
float g_ModelRotation[3] = { 0.0f, 0.0f, 0.0f };
double g_LastMouseX = 0.0;
double g_LastMouseY = 0.0;
bool g_MouseInitialized = false;

// カメラ注視モードの実体
CamFocus g_CamFocus = CamFocus::Auto;
int g_CamTargetMeshIndex = -1;

// リファレンス用のZオフセット（UIで設定可能にする想定）
float g_RefCamZOffset = 20.0f;

// 既存の g_BodyCenter / g_RefBodyCenter の定義はそのまま残します。
// （既にファイル内に g_BodyCenter / g_RefBodyCenter の初期化があるため追加の変更は不要）


// ==========================================
// 2. 関数の実装
// ==========================================

void AddLog(const std::string& msg, LogType type) {
    // ★ログレベルフィルタ: Error は常に、Warning は level>=1、Info/Success は level>=2 で表示。
    //   閾値未満は記録もファイル出力もしない（コンソール履歴・nss_log.txt 共通）。
    int req = (type == LogType::Error) ? 0 : (type == LogType::Warning) ? 1 : 2; // Info/Success=2
    if (req > g_LogLevel) return;

    std::lock_guard<std::mutex> lock(g_LogMutex);
    // 追加
    g_LogHistory.push_back({ msg, type });

    // 上限を超えたら古いエントリを削除（先頭から）
    if (g_LogHistory.size() > kLogHistoryMax) {
        size_t removeCount = g_LogHistory.size() - kLogHistoryMax;
        // erase の引数はイテレータ範囲
        g_LogHistory.erase(g_LogHistory.begin(), g_LogHistory.begin() + static_cast<std::ptrdiff_t>(removeCount));
    }

    if (g_LogAutoScroll) g_LogScrollToBottom = true;
    g_StatusMessage = msg;
    std::cout << "[LOG] " << msg << std::endl;

    // ★ログのファイル出力（コンソールに出る全ログ＋バッチ結果が残る）。
    //   セッション毎に新規ファイル。初回呼び出しでローテーション（最大3世代）してから開く:
    //     nss_log.txt(最新) → nss_log_1.txt(1つ前) → nss_log_2.txt(2つ前)。
    static std::ofstream s_logFile;
    static bool s_logInit = false;
    if (!s_logInit) {
        s_logInit = true;
        std::error_code ec;
        // ★#6: 実行ファイルのディレクトリ基準（CWD 非依存）。
        const std::string log0 = AppPath("nss_log.txt");
        const std::string log1 = AppPath("nss_log_1.txt");
        const std::string log2 = AppPath("nss_log_2.txt");
        fs::remove(log2, ec);                                  // 最古を破棄
        if (fs::exists(log1, ec)) fs::rename(log1, log2, ec);  // 1つ前 → 2つ前
        if (fs::exists(log0, ec)) fs::rename(log0, log1, ec);  // 前回 → 1つ前
        s_logFile.open(log0, std::ios::trunc);                 // 今回分を新規作成
    }
    if (s_logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        const char* lvl =
            type == LogType::Error   ? "ERROR" :
            type == LogType::Warning ? "WARN " :
            type == LogType::Success ? "OK   " : "INFO ";
        s_logFile << ts << " [" << lvl << "] " << msg << "\n";
        s_logFile.flush();
    }
}

void ShowTooltip(const char* desc) { // 引数を string から char* に統一
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::vector<std::string> SplitString(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) tokens.push_back(token);
    return tokens;
}

glm::vec3 GetColorFromIndex(int index) {
    static const glm::vec3 palette[] = {
        {1.0f, 0.5f, 0.5f}, {0.5f, 1.0f, 0.5f}, {0.5f, 0.5f, 1.0f}, {1.0f, 1.0f, 0.5f},
        {0.5f, 1.0f, 1.0f}, {1.0f, 0.5f, 1.0f}, {1.0f, 0.7f, 0.2f}, {0.2f, 0.8f, 0.8f}
    };
    return palette[index % 8];
}

std::string FormatSlotStringWithNames(const std::string& rawSlots) {
    std::stringstream ss(rawSlots);
    std::string segment;
    std::string result;
    bool first = true;
    while (std::getline(ss, segment, ',')) {
        try {
            int id = std::stoi(segment);
            if (!first) result += ", ";
            result += std::to_string(id) + " (" + SlotDictionary::GetSlotName(id) + ")";
            first = false;
        }
        catch (...) {}
    }
    return result;
}

std::vector<int> ParseSlotString(const std::string& slotStr) {
    std::vector<int> slots;
    std::string slotPart = slotStr;
    size_t lastSemi = slotStr.rfind(';');
    if (lastSemi != std::string::npos) slotPart = slotStr.substr(lastSemi + 1);
    std::stringstream ss(slotPart);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
        try { slots.push_back(std::stoi(segment)); }
        catch (...) {}
    }
    return slots;
}
//slotdata-*.txtの8-9列目のスロット数を数える
int CountSlotsInString(const std::string& slotStr) {
    return static_cast<int>(ParseSlotString(slotStr).size());
}

// ★③ シェーダのコンパイル状態を検査し、失敗時はログにエラーを出す（従来は握りつぶし）。
static void CheckShaderCompile(unsigned int shader, const char* stageName) {
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 1 ? len : 1, '\0');
        glGetShaderInfoLog(shader, (GLsizei)log.size(), nullptr, log.data());
        AddLog(std::string("[Shader] ") + stageName + " compile FAILED: " + log.c_str(), LogType::Error);
    }
}

unsigned int CreateShader(const char* vSource, const char* fSource) {
    unsigned int v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vSource, NULL); glCompileShader(v);
    CheckShaderCompile(v, "vertex");
    unsigned int f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fSource, NULL); glCompileShader(f);
    CheckShaderCompile(f, "fragment");
    unsigned int p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    // ★③ リンク状態も検査。
    GLint linked = GL_FALSE; glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 1 ? len : 1, '\0');
        glGetProgramInfoLog(p, (GLsizei)log.size(), nullptr, log.data());
        AddLog(std::string("[Shader] program link FAILED: ") + log.c_str(), LogType::Error);
    }
    glDeleteShader(v); glDeleteShader(f); return p;
}

// NIFからメッシュリストを更新
// 引数の NifFile& targetNif は、g_NifData などを渡す想定
// ※もしオーバーロードが必要なら、引数なし版もここで定義可能
void UpdateMeshList() {
    // グローバル変数を直接操作する版 (互換性維持)
    // 既存の UpdateMeshList(g_NifData, g_RenderMeshes, false); を呼ぶ形にするか、
    // ここに直接ロジックを書くか選べますが、今回は既存の引数あり版を呼ぶヘルパーにします。
    // ※引数あり版は Globals.h で宣言されていないようなので、ここで static 関数にするか、
    //   あるいは下に定義する引数あり版を直接使うようにコードを修正してください。
    //   ここでは「引数あり版」を「引数なし版」としてラップします。

    // 下記の実装を使うため、前方宣言
    void UpdateMeshListInternal(nifly::NifFile & targetNif, std::vector<RenderMesh>&outMeshes, bool isRef);

    UpdateMeshListInternal(g_NifData, g_RenderMeshes, false);
}

// 内部ロジック (引数あり版)
void UpdateMeshListInternal(nifly::NifFile& targetNif, std::vector<RenderMesh>& outMeshes, bool isRef) {
    // ★メモリ: 旧 GPU リソースは clear() が各 RenderMesh のデストラクタ
    //   (FreeGPUResources) で解放する。手動 glDelete を併用すると名前をゼロ化しないため
    //   デストラクタで二重削除になる（GL は無効名を無視するが冗長）。clear() に一本化。
    outMeshes.clear();

    // ★① メイン NIF の再構築時にテクスチャキャッシュの世代を進める（NIF 切替時のみ実質発火、
    //   同一 NIF のスロット編集等では nifKey 一致で no-op）。ref body 再構築では進めない。
    //   テクスチャの読込は描画ループの遅延ロードなので、ここでクリアしても現 NIF は安全。
    if (!isRef) TexCacheOnMainNifSwitch(g_CurrentNifPath.string());
    if (!isRef) g_SelectedMeshIndex = -1;

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

        // 各メッシュごとのローカルバウンディングを計算
        glm::vec3 meshMin(99999.0f), meshMax(-99999.0f);
        for (const auto& v : rawVerts) {
            glm::vec3 vv(v.x, v.y, v.z);
            meshMin = glm::min(meshMin, vv);
            meshMax = glm::max(meshMax, vv);
        }
        glm::vec3 meshCenter = (meshMin + meshMax) * 0.5f;
        float meshRadius = 0.0f;
        for (const auto& v : rawVerts) {
            glm::vec3 vv(v.x, v.y, v.z);
            meshRadius = std::max(meshRadius, glm::length(vv - meshCenter));
        }

        // 全体のバウンディング更新
        minBounds = glm::min(minBounds, meshMin);
        maxBounds = glm::max(maxBounds, meshMax);
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

        // ★Tier3-1: 実法線と UV を取得（テクスチャ表示の土台）。取得不可なら従来通りフォールバック。
        const std::vector<nifly::Vector3>* normalsPtr = targetNif.GetNormalsForShape(shape);
        std::vector<nifly::Vector2> uvs;
        targetNif.GetUvsForShape(shape, uvs);
        // ★#3: 接線（法線マップ用 TBN の T 軸）。無い形状はフォールバックで法線のみ。
        const std::vector<nifly::Vector3>* tangentsPtr = targetNif.GetTangentsForShape(shape);
        // ★#3-fix: 従法線も取得。TBN のハンドネス（左右ミラー UV での符号反転）を正しく扱うため、
        //   cross(N,T) と従法線の向きを比較して符号 w を求め、頂点に持たせる（B = cross(N,T)*w）。
        const std::vector<nifly::Vector3>* bitangentsPtr = targetNif.GetBitangentsForShape(shape);
        bool hasTangents = (tangentsPtr != nullptr && tangentsPtr->size() == rawVerts.size());
        bool hasBitangents = (bitangentsPtr != nullptr && bitangentsPtr->size() == rawVerts.size());

        // tangent は w 符号付き（vec4）。w = sign(dot(cross(N,T), Bstored))。
        struct Vertex { float x, y, z; float nx, ny, nz; float u, v; float tx, ty, tz, tw; };
        std::vector<Vertex> gpuVerts;
        std::vector<unsigned int> indices;
        gpuVerts.reserve(rawVerts.size());
        for (size_t i = 0; i < rawVerts.size(); ++i) {
            Vertex vert;
            vert.x = rawVerts[i].x; vert.y = rawVerts[i].y; vert.z = rawVerts[i].z;
            if (normalsPtr && i < normalsPtr->size()) {
                vert.nx = (*normalsPtr)[i].x; vert.ny = (*normalsPtr)[i].y; vert.nz = (*normalsPtr)[i].z;
            }
            else { vert.nx = 0.0f; vert.ny = 0.0f; vert.nz = 1.0f; }
            if (i < uvs.size()) { vert.u = uvs[i].u; vert.v = uvs[i].v; }
            else { vert.u = 0.0f; vert.v = 0.0f; }
            if (hasTangents) {
                vert.tx = (*tangentsPtr)[i].x; vert.ty = (*tangentsPtr)[i].y; vert.tz = (*tangentsPtr)[i].z;
            }
            else { vert.tx = 1.0f; vert.ty = 0.0f; vert.tz = 0.0f; }
            // ★#3-fix: 従法線があればハンドネスを判定。無ければ +1（従来の cross(N,T) 相当）。
            float w = 1.0f;
            if (hasTangents && hasBitangents) {
                glm::vec3 N(vert.nx, vert.ny, vert.nz);
                glm::vec3 T(vert.tx, vert.ty, vert.tz);
                glm::vec3 B((*bitangentsPtr)[i].x, (*bitangentsPtr)[i].y, (*bitangentsPtr)[i].z);
                if (glm::dot(glm::cross(N, T), B) < 0.0f) w = -1.0f;
            }
            vert.tw = w;
            gpuVerts.push_back(vert);
        }
        for (const auto& t : bsShape->triangles) { indices.push_back(t.p1); indices.push_back(t.p2); indices.push_back(t.p3); }

        RenderMesh mesh;
        mesh.indexCount = indices.size();
        mesh.color = isRef ? glm::vec3(0.5f) : GetColorFromIndex((int)outMeshes.size());
        mesh.name = shape->name.get();
        mesh.slotInfo = currentSlots;
        mesh.activeSlots = slotsFound;
        mesh.shapeIndex = (int)k;
        // 追加: mesh 中心と半径を格納
        mesh.center = meshCenter;
        mesh.boundingRadius = (meshRadius > 0.0f) ? meshRadius : 1.0f;

        // ★Tier3: diffuse テクスチャパスとアルファブレンド有無を抽出（遅延ロードは描画側で）
        {
            std::string texPath;
            if (targetNif.GetTextureSlot(shape, texPath, 0) > 0 && !texPath.empty())
                mesh.diffuseTexPath = texPath;
            // ★#3: 法線マップ（slot1）。接線がある形状のみ意味を持つ。
            std::string nrmPath;
            if (targetNif.GetTextureSlot(shape, nrmPath, 1) > 0 && !nrmPath.empty())
                mesh.normalTexPath = nrmPath;
            mesh.hasTangents = hasTangents;
            // ★#3-fix2: モデルスペース法線マップ(_msn, ボディ系)を検出。
            //   シェーダフラグ Model_Space_Normals を主、ファイル名 "_msn" を予備に判定。
            //   これらは object 空間に法線を直接格納するため、接線スペースの TBN 適用は誤り
            //   （ボディに暗い領域が出る）。検出時は接線スペース法線マップを無効化する。
            {
                bool msn = false;
                nifly::NiShader* shader = targetNif.GetShader(shape);
                if (shader && shader->IsModelSpace()) msn = true;
                if (!msn) {
                    std::string low = mesh.normalTexPath;
                    AsciiLowerInplace(low); // ★B5: テクスチャパスの "_msn" 判定
                    if (low.find("_msn") != std::string::npos) msn = true;
                }
                mesh.modelSpaceNormal = msn;
            }
            nifly::NiAlphaProperty* alphaProp = targetNif.GetAlphaProperty(shape);
            mesh.alphaBlend = (alphaProp != nullptr) && ((alphaProp->flags & 0x1) != 0); // bit0=Alpha Blend
            // ★#3-alpha: アルファテスト（bit9=0x200）と閾値。完全透過部の黒残り対策で discard する。
            mesh.alphaTest = (alphaProp != nullptr) && ((alphaProp->flags & 0x200) != 0);
            mesh.alphaThreshold = (alphaProp != nullptr) ? (alphaProp->threshold / 255.0f) : 0.0f;
        }

        glGenVertexArrays(1, &mesh.VAO); glGenBuffers(1, &mesh.VBO); glGenBuffers(1, &mesh.EBO);
        glBindVertexArray(mesh.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.VBO); glBufferData(GL_ARRAY_BUFFER, gpuVerts.size() * sizeof(Vertex), gpuVerts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.EBO); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx)); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u)); glEnableVertexAttribArray(2); // ★Tier3-1: UV
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, tx)); glEnableVertexAttribArray(3); // ★#3: 接線(xyz)+ハンドネス(w)
        glBindVertexArray(0);

        // ★#3-debug: 接線/従法線/法線パスの有無をログ（ref ボディの法線崩れ診断用）。
        if (LogVerbose()) AddLog("[NormalMap] " + std::string(isRef ? "REF " : "BODY ") + mesh.name
            + " tangents=" + (hasTangents ? "Y" : "N")
            + " bitangents=" + (hasBitangents ? "Y" : "N")
            + " modelSpace=" + (mesh.modelSpaceNormal ? "Y(skip)" : "N")
            + " nrmPath=" + (mesh.normalTexPath.empty() ? "(none)" : mesh.normalTexPath),
            LogType::Info);
        outMeshes.push_back(std::move(mesh));
    }
    // 全体中心の設定（isRef に合わせて body / ref を更新）
    if (hasVertices) {
        glm::vec3 overallCenter = (minBounds + maxBounds) * 0.5f;
        if (!isRef) g_BodyCenter = overallCenter;
        else g_RefBodyCenter = overallCenter;
    }

    // ★A2: メイン NIF の全メッシュが Meshes Blocklist（部分一致）で非表示になっている場合に警告。
    //   「読み込んだのに何も表示されない」原因が Blocklist だと気づけるようにする。
    //   描画ループと同じ判定（mesh.name に blocklist 語を部分一致で含むか）。
    if (!isRef && !outMeshes.empty()) {
        int blocked = 0;
        std::set<std::string> hitWords;
        for (const auto& m : outMeshes) {
            for (const auto& bw : g_KeywordBlockedList) {
                if (!bw.empty() && m.name.find(bw) != std::string::npos) {
                    ++blocked; hitWords.insert(bw); break;
                }
            }
        }
        if (blocked == (int)outMeshes.size()) {
            std::string words;
            for (const auto& w : hitWords) { if (!words.empty()) words += ", "; words += "\"" + w + "\""; }
            AddLog("All " + std::to_string(blocked) + " mesh(es) are HIDDEN by the Meshes Blocklist (matched: "
                + words + "). Remove the word(s) in Rules to show them.", LogType::Warning);
        }
        else if (blocked > 0) {
            AddLog(std::to_string(blocked) + "/" + std::to_string(outMeshes.size())
                + " mesh(es) hidden by Meshes Blocklist.", LogType::Info);
        }
    }
}

// ★B4: UTF-8 ↔ UTF-16 変換。境界（ファイルダイアログ・CreateProcessW 等）で使う。
//   ACP に依存しないので、UTF-8 マニフェストの有無にかかわらず非 ASCII パスを正しく扱える。
std::wstring Utf8ToWide(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}
std::string WideToUtf8(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

// ★#6: 実行ファイルのディレクトリを返す（末尾区切りなし）。失敗時は "." を返す。
std::string GetExeDir() {
#ifdef _WIN32
    // ★B4: GetModuleFileNameW（ワイド）→ UTF-8。非 ASCII の実行パスでも正しく扱う。
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= (sizeof(buf) / sizeof(buf[0]))) return ".";
    fs::path p(buf); // wchar_t* から構築（ネイティブ UTF-16）
    return WideToUtf8(p.parent_path().wstring());
#else
    return ".";
#endif
}

// ★#6: アプリ自身のファイル（config/log/keywords/cache/imgui.ini）を実行ディレクトリ基準で開くためのパス。
std::string AppPath(const std::string& filename) {
    return (fs::path(GetExeDir()) / filename).string();
}

std::string OpenFileDialog(const char* filter) {
#ifdef _WIN32
    // ★B4: GetOpenFileNameW（ワイド）。非 ASCII を含むパスでも正しく選択・取得できる。
    //   filter は二重 null 終端（"desc\0pattern\0...\0\0"）なので、実長を測ってからワイド化する。
    std::wstring wfilter;
    if (filter) {
        size_t flen = 0;
        while (!(filter[flen] == '\0' && filter[flen + 1] == '\0')) ++flen;
        flen += 2; // 末尾の \0\0 を含める
        int n = MultiByteToWideChar(CP_UTF8, 0, filter, (int)flen, nullptr, 0);
        if (n > 0) { wfilter.resize((size_t)n); MultiByteToWideChar(CP_UTF8, 0, filter, (int)flen, wfilter.data(), n); }
    }
    OPENFILENAMEW ofn;
    wchar_t szFile[4096] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = (DWORD)(sizeof(szFile) / sizeof(szFile[0]));
    ofn.lpstrFilter = wfilter.empty() ? nullptr : wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) == TRUE) return WideToUtf8(szFile);
#endif
    return "";
}

std::string SelectFolderDialog() {
#ifdef _WIN32
    // ★B4: SHBrowseForFolderW / SHGetPathFromIDListW（ワイド）。非 ASCII フォルダパスを正しく取得。
    wchar_t path[MAX_PATH] = { 0 };
    BROWSEINFOW bi = { 0 };
    bi.lpszTitle = L"Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != 0) {
        bool ok = (SHGetPathFromIDListW(pidl, path) == TRUE);
        CoTaskMemFree(pidl);
        if (ok) return WideToUtf8(path);
    }
#endif
    return "";
}
// ==========================================
// OSP Browser 関連 (実体)
// ==========================================
std::map<std::string, OSPFile> g_OspFiles;
std::string g_SelectedOspName = "";

// ★Step4: LaunchSynthesis() は撤去（slottool 連携に置換）。

// ★固定・同期レイアウト: メインウィンドウの作業領域（メニューバー除く）を
//   画像の配置に比例分割して、各常時表示パネルの矩形を返す。
void GetMainPanelRect(MainPanel p, ImVec2& pos, ImVec2& size) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 wp = vp->WorkPos;
    ImVec2 ws = vp->WorkSize;
    float topH = ws.y * 0.76f;
    float botH = ws.y - topH;
    float leftW = ws.x * 0.21f;   // Control Panel
    float rightW = ws.x * 0.24f;  // NIF Database
    float centerW = ws.x - leftW - rightW; // 3D Viewport
    float blW = ws.x * 0.52f;     // Log Console（下段左）
    switch (p) {
    case MainPanel::ControlPanel: pos = wp; size = ImVec2(leftW, topH); break;
    case MainPanel::Viewport:     pos = ImVec2(wp.x + leftW, wp.y);            size = ImVec2(centerW, topH); break;
    case MainPanel::Database:     pos = ImVec2(wp.x + leftW + centerW, wp.y);  size = ImVec2(rightW, topH); break;
    case MainPanel::LogConsole:   pos = ImVec2(wp.x, wp.y + topH);             size = ImVec2(blW, botH); break;
    case MainPanel::Pending:      pos = ImVec2(wp.x + blW, wp.y + topH);       size = ImVec2(ws.x - blW, botH); break;
    }
}