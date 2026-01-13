#pragma once
#ifdef _WIN32
// Synthesis を起動するラッパー。実体は Globals.cpp に置く。
void LaunchSynthesis();
#endif

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <glm/glm.hpp>
#include "NifFile.hpp" // niflyのヘッダー
#include "BoneAnalyzer.hpp"
#include "SlotDictionary.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>


namespace fs = std::filesystem;

// ============================================================
// 定数・列挙型
// ============================================================
extern const std::string CONFIG_FILENAME;

enum class LogType { Info, Success, Warning, Error };

// ============================================================
// 構造体定義
// ============================================================
struct LogEntry {
    std::string message;
    LogType type;
};

struct RenderMesh {
    std::string name;
    unsigned int VAO = 0, VBO = 0, EBO = 0;
    size_t indexCount = 0;
    glm::vec3 color = glm::vec3(1.0f);
    std::string slotInfo;
    std::vector<int> activeSlots;
    std::string beforeSlotInfo;
    std::vector<std::pair<int, int>> suggestions;

    // ★★★ 修正箇所: string ではなく MatchReason に戻す ★★★
    std::vector<MatchReason> debugReasons;

    int shapeIndex = -1;

    RenderMesh() = default;

    ~RenderMesh() {
        FreeGPUResources();
    }

    void FreeGPUResources() {
        if (VAO) { glDeleteVertexArrays(1, &VAO); VAO = 0; }
        if (VBO) { glDeleteBuffers(1, &VBO); VBO = 0; }
        if (EBO) { glDeleteBuffers(1, &EBO); EBO = 0; }
    }

    // コピー禁止
    RenderMesh(const RenderMesh&) = delete;
    RenderMesh& operator=(const RenderMesh&) = delete;

    // ムーブコンストラクタ
    RenderMesh(RenderMesh&& other) noexcept {
        *this = std::move(other);
    }

    // ムーブ代入演算子
    RenderMesh& operator=(RenderMesh&& other) noexcept {
        if (this != &other) {
            FreeGPUResources();

            name = std::move(other.name);
            VAO = other.VAO; other.VAO = 0;
            VBO = other.VBO; other.VBO = 0;
            EBO = other.EBO; other.EBO = 0;
            indexCount = other.indexCount;
            color = other.color;
            slotInfo = std::move(other.slotInfo);
            activeSlots = std::move(other.activeSlots);
            beforeSlotInfo = std::move(other.beforeSlotInfo);
            suggestions = std::move(other.suggestions);
            debugReasons = std::move(other.debugReasons); // ここも MatchReason型として移動されます
            shapeIndex = other.shapeIndex;
        }
        return *this;
    }
};

struct SlotRecord {
    int id = -1;
    std::string sourceFile;   // ESP名
    std::string armaFormID;
    std::string armaEditorID;
    std::string armoFormID;
    std::string armoEditorID;
    std::string malePath;
    std::string femalePath;
    std::string armoSlots;
    std::string armaSlots;
    std::string nifPath;
    std::string baseNifKey;

    // 追加: male / female 別々に正規化した base key を保持する
    std::string baseNifKeyMale;
    std::string baseNifKeyFemale;

    std::string displayText;
    bool isOspSource = false;
    std::string originalNifPath;
};

struct KidKeyword {
    std::string keyword;
    std::vector<int> targetSlots;
    std::vector<std::string> matchWords;
};

// ============================================================
// グローバル変数 (宣言)
// ============================================================

// スレッド制御
extern std::mutex g_LogMutex;
extern std::mutex g_ProgressMutex;
extern std::mutex g_DataMutex;
extern std::atomic<bool> g_IsProcessing;
extern std::atomic<bool> g_CancelRequested;
extern std::atomic<float> g_Progress;
extern std::string g_CurrentProcessItem;

// ログ
extern std::vector<LogEntry> g_LogHistory;
extern bool g_LogAutoScroll;
extern bool g_LogScrollToBottom;
extern std::string g_StatusMessage;

// パス設定
// 主要なグローバル変数（使用している箇所に合わせて extern 宣言だけを置く）
// 実体定義は必ず Globals.cpp に一箇所だけ置いてください。
// 例: Globals.cpp に `char g_InputRootPath[4096] = {0};` のように定義する。
extern char g_InputRootPath[4096];
extern char g_OutputRootPath[4096];
extern char g_GameDataPath[4096];
extern char g_SynthesisPath[4096];
extern char g_SlotDataPath[4096];

extern char g_KIDTargetBuffer[4096];
extern char g_KIDResultBuffer[4096];
extern char g_InputBuffer[1024];

// UIフラグ 　不必要なものがあるかも知れない。要確認
extern bool g_ShowSettingsWindow;
extern bool g_ShowRulesWindow;
extern bool g_ShowDatabaseWindow;
extern bool g_ShowControlPanel;
extern bool g_ShowPendingAreaWindow;
extern bool g_ShowKIDGeneratorWindow;
extern bool g_ShowDebugWindow;
extern bool g_ShowLogWindow;
extern bool g_ShowProgressWindow;
extern bool g_ShowAnalysisDetailsLog; // Debug Logウィンドウの表示フラグ

// 設定リスト
extern std::vector<KidKeyword> g_KeywordList;
extern std::vector<std::string> g_SourceBlockedList;
extern std::vector<std::string> g_KeywordBlockedList;

// KID Generator
extern char g_KIDTargetBuffer[4096];
extern char g_KIDResultBuffer[4096];
extern int g_SelectedKeywordIndex;
// ImGuiTextFilter は <imgui.h> が必要ですが、ヘッダー依存を減らすため
// ここでは前方宣言できないので、main.cpp で include "imgui.h" してから Globals.h を読むか、
// ここに <imgui.h> を足す必要があります。今回は手っ取り早くここに足します。
#include <imgui.h> 
extern ImGuiTextFilter g_KIDKeywordFilter;

// NIF関連
extern fs::path g_CurrentNifPath;
extern nifly::NifFile g_NifData;
extern std::vector<RenderMesh> g_RenderMeshes;
extern int g_SelectedMeshIndex;

extern fs::path g_PairedNifPath;
extern nifly::NifFile g_PairedNifData;
extern bool g_HasPairedFile;

extern fs::path g_RefNifPath;
extern nifly::NifFile g_RefNifData;
extern std::vector<RenderMesh> g_RefRenderMeshes;
extern bool g_ShowRef;

extern int g_TargetGender;
extern int g_NifLoadMode;

// データベース
extern std::vector<SlotRecord> g_AllRecords;
extern std::map<std::string, std::map<std::string, std::map<std::string, std::vector<int>>>> g_DisplayTree;
extern std::map<std::string, std::vector<std::string>> g_BodySlideSourceMap;
extern bool g_BodySlideScanned;
extern std::map<std::string, bool> g_SourceSelectionMap;

// タブ制御
extern bool g_ForceTabToList;
extern bool g_ForceTabToSource;

// 選択状態
extern std::map<int, bool> g_RecordSelectionMap;
extern ImGuiTextFilter g_SlotFilter;
extern int g_SelectedRecordID;
extern std::string g_PreviewSlotStr;

// セッション
extern std::map<std::string, SlotRecord> g_SessionChanges;

// OpenGL / Camera
extern unsigned int g_ShaderProgram;
extern glm::vec3 g_BodyCenter;
extern glm::vec3 g_RefBodyCenter;
extern char g_InputBuffer[1024];
extern float g_CamDistance;
extern glm::vec3 g_CamOffset;
extern float g_ModelRotation[3];
extern double g_LastMouseX;
extern double g_LastMouseY;
extern bool g_MouseInitialized;

void ShowTooltip(const char* desc);
std::string FormatSlotStringWithNames(const std::string& slotStr);

// これを他のファイルからも呼べるように宣言
void UpdateMeshList();
//void AddLog(const std::string& message, int logType = 0);
void AddLog(const std::string& message, LogType logType = LogType::Info);
// ==========================================
// ヘルパー関数 (宣言)
// ==========================================

std::vector<std::string> SplitString(const std::string& s, char delimiter);
glm::vec3 GetColorFromIndex(int index);
std::string FormatSlotStringWithNames(const std::string& rawSlots);
std::vector<int> ParseSlotString(const std::string& slotStr);
// シェーダー作成 (OpenGL依存)
unsigned int CreateShader(const char* vSource, const char* fSource);



#ifndef NIF_SLOT_SNIPER_GLOBALS_H
#define NIF_SLOT_SNIPER_GLOBALS_H

#include <string>

#ifdef _WIN32
// ダイアログ関数はヘッダで宣言のみ行う（定義は Globals.cpp に置く）
std::string OpenFileDialog(const char* filter);
std::string SelectFolderDialog();
#endif


#endif // NIF_SLOT_SNIPER_GLOBALS_H

// ==========================================
// OSP Browser 関連 (更新)
// ==========================================

// 1つのスライダーセット情報
struct BodySlideSet {
    std::string setName;
    std::string sourceNifPath;
    std::string outputName;
    std::string outputPath;
    std::string fullOutputPath;
    bool selected = false;
};
// OSPファイル単位の管理用
struct OSPFile {
    std::string filename;
    std::string fullPath;
    std::vector<BodySlideSet> sets;
};
// グローバル変数の宣言
extern std::map<std::string, OSPFile> g_OspFiles;
extern std::string g_SelectedOspName;

// 関数宣言 (UI_Database.cpp 内で実装されるヘルパー関数も含む)
void ScanOSPWorker();     // OSP_Logic.cppで実装
void ExportOSPWorker();   // OSP_Logic.cppで実装
void BatchExportWorker(); // main.cppで実装
// OSPファイルを解析してセット情報を抽出する関数
void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets);

// 他のファイルの関数
void LoadSlotDataFolder(const std::string& path);
void LoadNifFileCore(const std::string& path);
fs::path ConstructSafePath(const std::string& root, const std::string& rel);
void SaveSessionChangesToFile();
void SaveUnifiedConfig(); // main.cpp にある場合は extern、UI_Database.cpp に移すならそのままでOK

//よくわからないものをココにおいておく

//よくわからないものゾーン終了