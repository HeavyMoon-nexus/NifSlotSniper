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
std::mutex g_DataMutex;
std::atomic<bool> g_IsProcessing(false);
std::atomic<bool> g_CancelRequested(false);
std::atomic<float> g_Progress(0.0f);
std::string g_CurrentProcessItem;

// ログ
std::vector<LogEntry> g_LogHistory;
bool g_LogAutoScroll = true;
bool g_LogScrollToBottom = false;
std::string g_StatusMessage = "Ready";

// パス設定
char g_InputRootPath[4096] = { 0 };
char g_OutputRootPath[4096] = { 0 };
char g_GameDataPath[4096] = { 0 };
char g_SynthesisPath[4096] = { 0 };
char g_SlotDataPath[4096] = { "slotdataTXT" };
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
bool g_ShowKIDGeneratorWindow = true;
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
int g_NifLoadMode = 0;  // 0:Both, 1:Single, 2:Pair Only

// データベース
std::vector<SlotRecord> g_AllRecords;
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
extern glm::vec3 g_BodyCenter = glm::vec3(0.0f);
extern glm::vec3 g_RefBodyCenter(0.0f); // ★追加: リファレンスボディの中心
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

unsigned int CreateShader(const char* vSource, const char* fSource) {
    unsigned int v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vSource, NULL); glCompileShader(v);
    unsigned int f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fSource, NULL); glCompileShader(f);
    unsigned int p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
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
    for (auto& mesh : outMeshes) {
        if (mesh.VAO) glDeleteVertexArrays(1, &mesh.VAO);
        if (mesh.VBO) glDeleteBuffers(1, &mesh.VBO);
        if (mesh.EBO) glDeleteBuffers(1, &mesh.EBO);
    }
    outMeshes.clear();
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
        // 追加: mesh 中心と半径を格納
        mesh.center = meshCenter;
        mesh.boundingRadius = (meshRadius > 0.0f) ? meshRadius : 1.0f;

        glGenVertexArrays(1, &mesh.VAO); glGenBuffers(1, &mesh.VBO); glGenBuffers(1, &mesh.EBO);
        glBindVertexArray(mesh.VAO);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.VBO); glBufferData(GL_ARRAY_BUFFER, gpuVerts.size() * sizeof(Vertex), gpuVerts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.EBO); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx)); glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        outMeshes.push_back(std::move(mesh));
    }
    // 全体中心の設定（isRef に合わせて body / ref を更新）
    if (hasVertices) {
        glm::vec3 overallCenter = (minBounds + maxBounds) * 0.5f;
        if (!isRef) g_BodyCenter = overallCenter;
        else g_RefBodyCenter = overallCenter;
    }
}

std::string OpenFileDialog(const char* filter) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    char szFile[260] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn) == TRUE) return std::string(szFile);
#endif
    return "";
}

std::string SelectFolderDialog() {
#ifdef _WIN32
    char path[MAX_PATH];
    BROWSEINFOA bi = { 0 };
    bi.lpszTitle = "Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != 0) {
        if (SHGetPathFromIDListA(pidl, path)) {
            CoTaskMemFree(pidl);
            return std::string(path);
        }
        CoTaskMemFree(pidl);
    }
#endif
    return "";
}
// ==========================================
// OSP Browser 関連 (実体)
// ==========================================
std::map<std::string, OSPFile> g_OspFiles;
std::string g_SelectedOspName = "";

// ワーカー関数の中身は main.cpp から移動してくるのがベストですが、
// 今はとりあえず main.cpp にあるものを呼ぶ形にしますか？
// もし main.cpp に残すなら、ここには実体を書かず、
// main.cpp 側で実装し、Globals.h で宣言だけしておけばOKです。
// 今回は「変数の実体」だけここに書いてください。
#ifdef _WIN32
void LaunchSynthesis() {
    if (strlen(g_SynthesisPath) > 0) {
        // ANSI 版を使う既存コードに合わせて ShellExecuteA を呼ぶ
        ShellExecuteA(NULL, "open", g_SynthesisPath, NULL, NULL, SW_SHOWNORMAL);
    }
}
#else
void LaunchSynthesis() {
    // Windows 以外では未実装（安全のため空実装）
}
#endif