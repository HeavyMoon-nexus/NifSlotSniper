#pragma once
// ★Step4: Synthesis 連携は撤去（slottool に置換）。LaunchSynthesis() を削除。

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <glm/glm.hpp>
#include "NifFile.hpp"
#include "BoneAnalyzer.hpp"
#include "SlotDictionary.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>


namespace fs = std::filesystem;

// ★B5: ASCII 範囲限定の小文字化。パスのバイト列に ::tolower を直接適用すると Shift-JIS 等の
//   2 バイト目（0x41-0x5A の 'A'-'Z' 域に入り得る）を誤変換し、キャッシュキー/パス照合が化けて
//   別ファイルへのヒット/正レコード非ヒット（誤ファイル操作）を招く。0x80 以上のバイトは保持する。
//   （負値を ::tolower に渡す未定義動作も回避できる。）
inline void AsciiLowerInplace(std::string& s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
}
inline std::string AsciiLower(std::string s) { AsciiLowerInplace(s); return s; }

// ★B4: UTF-8 ↔ UTF-16 変換（Win32 W 系 API の境界用）。実装は Globals.cpp。
//   内部文字列は UTF-8 とする（exe マニフェストで ACP=UTF-8 化。下記は ACP 非依存の明示変換）。
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& w);

// ============================================================
// Constants / enums
// ============================================================
extern const std::string CONFIG_FILENAME;

enum class LogType { Info, Success, Warning, Error };

enum class CamFocus {
    Auto = 0,
    Nif = 1,
    Ref = 2,
    Mesh = 3
};

// ============================================================
// Struct definitions
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

    std::vector<MatchReason> debugReasons;

    int shapeIndex = -1;

    glm::vec3 center = glm::vec3(0.0f);
    float boundingRadius = 1.0f;

    // ★Tier3: テクスチャ表示用。diffuse パス(NIF相対)とアルファ/両面フラグ。
    std::string diffuseTexPath;
    // ★#3: 法線マップパス(NIF相対, slot1)。接線がある形状のみ有効。空なら法線マップ無効。
    std::string normalTexPath;
    // ★#3: 形状に接線データが存在したか（無ければシェーダ側で法線マップを無効化）。
    bool hasTangents = false;
    // ★#3-fix2: モデルスペース法線マップ(_msn, ボディ系)か。true なら接線スペース適用から除外。
    bool modelSpaceNormal = false;
    bool alphaBlend = false;
    // ★#3-alpha: アルファテスト（カットアウト）。有効なら threshold 未満を discard し、
    //   完全透過部が黒く残るのを防ぐ。
    bool alphaTest = false;
    float alphaThreshold = 0.0f;
    bool twoSided = false;

    RenderMesh() = default;

    ~RenderMesh() {
        FreeGPUResources();
    }

    void FreeGPUResources() {
        if (VAO) { glDeleteVertexArrays(1, &VAO); VAO = 0; }
        if (VBO) { glDeleteBuffers(1, &VBO); VBO = 0; }
        if (EBO) { glDeleteBuffers(1, &EBO); EBO = 0; }
    }

    RenderMesh(const RenderMesh&) = delete;
    RenderMesh& operator=(const RenderMesh&) = delete;

    RenderMesh(RenderMesh&& other) noexcept {
        *this = std::move(other);
    }

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
            debugReasons = std::move(other.debugReasons);
            shapeIndex = other.shapeIndex;
            center = other.center;
            boundingRadius = other.boundingRadius;
            diffuseTexPath = std::move(other.diffuseTexPath); // ★Tier3
            normalTexPath = std::move(other.normalTexPath);   // ★#3
            hasTangents = other.hasTangents;                  // ★#3
            modelSpaceNormal = other.modelSpaceNormal;        // ★#3-fix2
            alphaBlend = other.alphaBlend;
            alphaTest = other.alphaTest;            // ★#3-alpha
            alphaThreshold = other.alphaThreshold;  // ★#3-alpha
            twoSided = other.twoSided;
        }
        return *this;
    }
};

// ★カラバリ: AlternateTextures の 1 上書き（slottool export 由来）。
//   index = 3D Index（ジオメトリ順、主キー）。shape = 3D Name（フォールバック）。
struct AltTex {
    std::string shape;
    int index = -1;
    std::string diffuse;
};

// ★per-mesh: NIF のパーティション由来「メッシュ毎スロット」。union ではなく shape 毎の真実。
//   shapeIndex = GetShapes() の並び順（パーティション書込キー。AltTex.index と同思想）。
//   name = 形状名（shapeIndex が一致しないときのフォールバック照合・表示用）。
//   slots = その shape の BSDismemberSkinInstance パーティション partID 列。
struct MeshSlot {
    int shapeIndex = -1;
    std::string name;
    std::vector<int> slots;
    // ★BSD→NiSkin 変換: true ならこの shape を export 時に NiSkinInstance へ変換し、スロットを持たせない
    //   （ESP の armaSlots union からも除外＝ESP もスロットが外れる）。
    bool toNiSkin = false;
};

// ★スキンボーン抽出（衣装拡張システム連携）: shape 毎のスキンボーン情報。
//   meshes[]（BSD のみ）と独立に、NIF の全 shape（BSTriShape/NiTriShape/static）を網羅する。
//   利用側（SKSE 衣装拡張）はこの bones がスケルトンに名前解決できるかでアタッチ可否を判定する。
//   shapeIndex = GetShapes() の並び順（meshes[].shapeIndex と同一基準）。
struct ShapeBone {
    int shapeIndex = -1;
    std::string name;                  // 形状名
    bool hasSkin = false;              // skin instance（NiSkin/BSD）を持つか。false=リジッド（変形しない）
    std::vector<std::string> bones;    // スキンボーン名（NiNode 名。厳密一致・大小区別を保持）。hasSkin=false なら空
    std::string skeletonRoot;          // skin instance が宣言する Skeleton Root ノード名（診断用。取得不可なら空）
};

// ★スケルトン解決パス: 1 レコード（表示 gender）のボーンが、指定スケルトンに名前解決できるかの判定結果。
//   resolved は三値: -1=未判定（スケルトン未指定等）, 0=未解決あり, 1=全解決。
struct BoneResolution {
    bool computed = false;             // この判定を実施したか（false なら JSON に出さない）
    std::string skeleton;              // 判定に用いたスケルトン NIF の識別/パス（複数なら ';' 連結）
    std::string gender;                // 判定対象の gender（"male"/"female"。NSS は表示 gender のみ）
    int resolved = -1;                 // -1=null（未判定）, 0=false, 1=true
    std::vector<std::string> missingBones;     // 未解決ボーンの和集合（厳密一致）
    std::vector<std::string> unskinnedShapes;  // hasSkin=false のシェイプ名（変形しない＝利用側で区別が要る）
    std::string note;                  // resolved=null の理由など（スケルトン未指定等）
};

struct SlotRecord {
    int id = -1;
    std::string sourceFile;
    std::string armaFormID;
    std::string armaEditorID;
    std::string armoFormID;
    std::string armoEditorID;
    std::string malePath;
    std::string femalePath;
    std::string armoSlots;
    std::string armaSlots;
    // ★per-mesh（Option C）: NIF パーティション由来のメッシュ毎スロット（表示 gender 分）。
    //   Import DB 時に NIF を読んで構築。Apply 時は表示中 NIF から再構築する。
    //   meshes が空のレコードは旧来の armaSlots(union) 位置適用にフォールバックする。
    std::vector<MeshSlot> meshes;
    // ★per-mesh: ブロックスロット = armaSlots − union(meshes.slots)。ジオメトリを持たない
    //   biped フラグ（例 48/49/56）。編集時に armaSlots を「置換維持型」
    //   （= union(meshes) ∪ blockingSlots）で再構成するため Import DB 時に算出して保持。
    std::vector<int> blockingSlots;
    // ★スキンボーン抽出: NIF の全 shape を網羅するスキンボーン情報（表示 gender 分）。
    //   meshes[] とは独立で、リジッド/NiSkin shape も含む。Import DB / Convert 時に NIF から構築し、
    //   ChangeSet・per-mesh キャッシュ双方へ出力する（利用側の主要ターゲットのネイルはスロット未変更で
    //   changeset に現れず、キャッシュにのみ存在するため両方に乗せる）。
    std::vector<ShapeBone> shapeBones;
    // ★スケルトン解決パス: 設定スケルトンに対するボーン解決可否（表示 gender 分）。
    //   スケルトン設定/config に依存するため NIF キャッシュには載せず、毎回算出して ChangeSet へ出力する。
    BoneResolution boneRes;
    // ★Step3: Mutagen 正規形の完全 FormKey（"000800:Nurse.esp"）。JSON 連携で
    //   origin mod を失わず往復するために保持する（空なら未設定＝txt 由来）。
    std::string armaFormKey;
    std::string armoFormKey;
    // ★カラバリ: AlternateTextures（上書き対象→diffuse パス）を男女別に保持。
    //   エンジンは 3D Index でジオメトリにマッチするため index を主キーにする
    //   （Name は "1"/"2" 等の索引文字列のことがあり、メッシュ名と一致しない）。
    //   index<0 のときのみ shape 名（case-insensitive）でフォールバック照合。
    std::vector<AltTex> altMale;
    std::vector<AltTex> altFemale;
    std::string nifPath;
    std::string baseNifKey;

    std::string baseNifKeyMale;
    std::string baseNifKeyFemale;

    std::string displayText;
    bool isOspSource = false;
    std::string originalNifPath;
    bool pendingOnly = false;
    bool nifModified = false;
};
int CountSlotsInString(const std::string& slotStr);

struct KidKeyword {
    std::string keyword;
    std::vector<int> targetSlots;
    std::vector<std::string> matchWords;
};

// ============================================================
// Global variables (declarations)
// ============================================================

extern std::mutex g_LogMutex;
extern std::mutex g_ProgressMutex;
// ★#1: recursive_mutex 化。UI パネル描画は g_AllRecords/g_DisplayTree/g_OspFiles/g_SessionChanges を
//   毎フレーム反復するため、これらを触る描画区間を g_DataMutex で囲む。その区間から呼ぶ
//   ヘルパー（LoadOSPDetails / SaveSessionChangesToFile / RecordCurrentMeshesToPending 等）も
//   g_DataMutex を取るため、同一スレッドでの再帰ロックが発生する。recursive_mutex で安全に許容する。
extern std::recursive_mutex g_DataMutex;
extern std::atomic<bool> g_IsProcessing;
extern std::atomic<bool> g_CancelRequested;
extern std::atomic<float> g_Progress;
extern std::string g_CurrentProcessItem;
// ★A: 非同期化（Import DB / BSA unpack）の共有状態
extern std::atomic<bool> g_ProcessCancelable;    // Processing モーダルに Cancel を出すか
extern std::atomic<bool> g_ImportResultReady;    // Import DB の結果が「メインで適用待ち」か
extern std::map<std::string, SlotRecord> g_ImportResult; // バックグラウンドが作った結果（main で適用）

extern std::vector<LogEntry> g_LogHistory;
extern bool g_LogAutoScroll;
extern bool g_LogScrollToBottom;
extern std::string g_StatusMessage;

extern char g_InputRootPath[4096];
extern char g_OutputRootPath[4096];
extern char g_GameDataPath[4096];
extern char g_SlotDataPath[4096];
// ★Step3: Mutagen CLI(slottool.exe) のパス。Synthesis を置き換える。
extern char g_SlotToolPath[4096];
// ★① BSArch.exe（CLI版）のパス。BSA 内テクスチャ表示に使う（未設定なら BSA 非対応）。
extern char g_BsArchPath[4096];
// ★#5: Load Ref Body のパスを記憶。起動時に自動読込する。
extern char g_RefBodyPath[4096];
// ★#5: 参照ボディ用テクスチャフォルダ（texture mode 時、ref のディフューズをここから解決）。
extern char g_RefTexFolder[4096];
// ★スケルトン解決パス: ボーン名前解決の照合に使うスケルトン NIF（例 XPMSE の skeleton_female.nif）。
//   ';' 区切りで複数指定可（全 NiNode 名の和集合で集合を作る）。Female/Male 別。実機の MO2 経由で解決される
//   フルパス or InputRoot 相対のどちらでも可（ConstructSafePath で解決を試みる）。
extern char g_SkeletonPathFemale[4096];
extern char g_SkeletonPathMale[4096];
// ★#5: 指定パスの参照ボディを読み込む共有ヘルパー（UI_ControlPanel.cpp 実装）。
bool LoadReferenceBody(const std::string& path);

extern char g_KIDTargetBuffer[4096];
extern char g_KIDResultBuffer[4096];
extern char g_InputBuffer[1024];

extern bool g_ShowSettingsWindow;
extern bool g_ShowRulesWindow;
extern bool g_ShowDatabaseWindow;
extern bool g_ShowControlPanel;
extern bool g_ShowPendingAreaWindow;
extern bool g_ShowKIDGeneratorWindow;
extern bool g_ShowDebugWindow;
extern bool g_ShowLogWindow;
extern bool g_ShowProgressWindow;
extern bool g_ShowAnalysisDetailsLog;

extern std::vector<KidKeyword> g_KeywordList;
extern std::vector<std::string> g_SourceBlockedList;
extern std::vector<std::string> g_KeywordBlockedList;

// KID Generator
extern char g_KIDTargetBuffer[4096];
extern char g_KIDResultBuffer[4096];
extern int g_SelectedKeywordIndex;
#include <imgui.h> 
extern ImGuiTextFilter g_KIDKeywordFilter;

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
// ★Tier3: テクスチャ表示モード（既定 false=フラット色）。true でルーズ diffuse を貼る。
extern bool g_TextureMode;
// ★統合 Export: 何を出力するかのトグル（config 保存）。
extern bool g_ExportNif;
extern bool g_ExportEsl;
extern bool g_ExportTxt;
// ★衣装シード出力: ON のとき ChangeSet 書き出しと同時に costume_seed.json（利用側への受け渡しシード）を
//   同フォルダへ出力する。ポリシー項目（default_enabled/strip_class）は常に null（NIF から導出不能）。既定 OFF・config 保存。
extern bool g_ExportCostumeSeed;
// ★Force Overwrite: Output Root を無視し、オリジナルの NIF / ESP に直接上書きする（.bak バックアップ付き）。
//   個別・バッチ（Pending 統合 Export）両方に適用。破壊的なので既定 OFF・config 保存。
extern bool g_ForceOverwrite;
// ★ログレベル: 0=Error / 1=Warning / 2=Info(+Success) / 3=Verbose（診断含む）。既定 2。
//   Verbose のとき、旧 g_VerboseExportLog / g_TexResolveLog / [NormalMap] 等の診断ログを出す。
extern int g_LogLevel;
inline bool LogVerbose() { return g_LogLevel >= 3; }
// ★#2: DB slot と NIF 実パーティションが不一致のレコード id 集合。NIF tree のオレンジ表示用。
extern std::set<int> g_SlotMismatchRecords;
// ★#2: 全 DB レコードの NIF を走査し、DB slot と NIF パーティションの不一致を g_SlotMismatchRecords に集める。
//   NiSkin（パーティション無し）は除外。スレッドで起動する。
void ScanNifSlotsWorker();
// ★① テクスチャキャッシュ上限（直近 N 个の NIF を保持。ref body 除外。0以下=無制限）。
extern int g_TexCacheNifLimit;
// ★① メイン NIF 切替時にキャッシュ世代を進め、保持窓外の管理テクスチャを解放する
//   （TextureLoader.cpp 実装）。描画ループからは呼ばないこと。
void TexCacheOnMainNifSwitch(const std::string& nifKey);
// ★#3-debug: 法線マップ基底の切替（実機で正しい組み合わせを当てるため）。
extern bool g_UseNormalMap; // 法線マップ全体の ON/OFF
extern bool g_NmFlipGreen;  // グリーン反転（DirectX 規約）
extern bool g_NmFlipHand;   // ハンドネス(w)反転

extern std::vector<SlotRecord> g_AllRecords;
// ★DB 版数: g_AllRecords を作り直すたびに増加（BuildRecordsAndTree 内）。
//   フィルタ等のキャッシュ無効化に使う。
extern int g_DbVersion;
extern std::map<std::string, std::map<std::string, std::map<std::string, std::vector<int>>>> g_DisplayTree;
extern std::map<std::string, std::vector<std::string>> g_BodySlideSourceMap;
extern bool g_BodySlideScanned;
extern std::map<std::string, bool> g_SourceSelectionMap;

extern bool g_ForceTabToList;
extern bool g_ForceTabToSource;

extern std::map<int, bool> g_RecordSelectionMap;
extern ImGuiTextFilter g_SlotFilter;
extern int g_SelectedRecordID;
extern std::string g_PreviewSlotStr;

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

extern CamFocus g_CamFocus;
extern int g_CamTargetMeshIndex;
extern float g_RefCamZOffset;

void ShowTooltip(const char* desc);
std::string FormatSlotStringWithNames(const std::string& slotStr);
void UpdateMeshList();
void UpdateMeshListInternal(nifly::NifFile& targetNif, std::vector<RenderMesh>& outMeshes, bool isRef);
//void AddLog(const std::string& message, int logType = 0);
void AddLog(const std::string& message, LogType logType = LogType::Info);
fs::path FindFileInBodySlide(const std::string& filename);
// ==========================================
// Helper functions (declarations)
// ==========================================

std::vector<std::string> SplitString(const std::string& s, char delimiter);
glm::vec3 GetColorFromIndex(int index);
std::string FormatSlotStringWithNames(const std::string& rawSlots);
std::vector<int> ParseSlotString(const std::string& slotStr);
unsigned int CreateShader(const char* vSource, const char* fSource);




// ★修正: ファイル先頭の #pragma once に統一。中盤にあった
//   #ifndef NIF_SLOT_SNIPER_GLOBALS_H ... #endif の二重ガードを撤去
//   （宣言の大半がこのガードの外にあり、ガードとして機能していなかったため）。

#include <string>

#ifdef _WIN32
std::string OpenFileDialog(const char* filter);
std::string SelectFolderDialog();
#endif

// ==========================================
// OSP Browser related
// ==========================================

struct BodySlideSet {
    std::string setName;
    std::string sourceNifPath;
    std::string outputName;
    std::string outputPath;
    std::string fullOutputPath;
    bool selected = false;
};
struct OSPFile {
    std::string filename;
    std::string fullPath;
    std::vector<BodySlideSet> sets;
};
extern std::map<std::string, OSPFile> g_OspFiles;
extern std::string g_SelectedOspName;

void ScanOSPWorker();     // OSP_Logic.cpp
void ExportOSPWorker();   // OSP_Logic.cpp
// OSP file parser: extract sets from an .osp.
void ParseOSPFile(const fs::path& ospPath, std::vector<BodySlideSet>& outSets);

// ★バッチ: slotdata-Output.txt を pending に読み込む（→ 統合 Export で一括生成）。
void LoadSlotdataIntoPending();
// ★Step3: 共有ヘルパー（Main.cpp 実装）。uniqueRecords から g_AllRecords / g_DisplayTree を再構築。
void BuildRecordsAndTree(std::map<std::string, SlotRecord>& uniqueRecords);
std::string GetBaseNifKey(const std::string& path);
// ★要望①: "X_SlotPatch.esp" / "X_slotdata.esp"（二重含む）→ "X.esp"。生成パッチが勝者になった際の
//   source 二重サフィックスを集約する。実装は SlotToolBridge.cpp。全取り込み口・slottool 送出前で使う。
std::string NormalizePatchSource(const std::string& src);
// ★per-mesh: 指定 NIF（フルパス）を SEH 保護で読み、shape 毎のパーティション slot を outMeshes に詰める。
//   BSDismemberSkinInstance を持つ shape のみ。dismember が1つでもあれば true。実装は Main.cpp。
bool ReadNifPerMeshSlots(const std::string& nifFullPath, std::vector<MeshSlot>& outMeshes);
// ★スキンボーン抽出: 上記に加え、全 shape のスキンボーン情報（ShapeBone）も同時に取得する（NIF を1回開く）。
//   meshes[] の挙動は ReadNifPerMeshSlots と同一（BSD のみ）。outShapeBones は全 shape を網羅。実装は Main.cpp。
bool ReadNifPerMeshSlotsAndBones(const std::string& nifFullPath, std::vector<MeshSlot>& outMeshes,
                                 std::vector<ShapeBone>& outShapeBones);
// ★スケルトン解決パス: ';' 区切りのスケルトン NIF パス列を読み、全 NiNode 名の集合を outNames に詰める。
//   いずれかの NIF が読めれば true。識別子（解決できたパスの ';' 連結）を outResolvedId に返す。実装は Main.cpp。
bool BuildSkeletonBoneSet(const std::string& semicolonPaths, std::set<std::string>& outNames,
                          std::string& outResolvedId);
// ★衣装シード: 全件レコードから costume_seed.json を ChangeSet とは独立に出力（トグル ON 時のみ呼ぶ）。
//   Import DB（全件）／Convert（変換 recs）完了時に呼ぶ。スキンド shape を持つレコードのみ。実装は Main.cpp。
void WriteCostumeSeed(const std::map<std::string, SlotRecord>& records);
// ★スケルトン解決パス: rec.shapeBones を boneSet に対して判定し rec.boneRes を埋める。
//   skeletonId が空（スケルトン未指定/読込不可）のときは resolved=null（理由を note に）。実装は Main.cpp。
void ComputeBoneResolution(SlotRecord& rec, const std::set<std::string>& boneSet,
                           const std::string& skeletonId, const std::string& genderLabel);
// ★BSD→NiSkin: 指定 shape の BSDismemberSkinInstance を NiSkinInstance に置換する（パーティション/スロット喪失）。
//   bone/data/skinPartition/target を引き継ぐ。BSD でなければ false。実装は Main.cpp。
bool ConvertShapeToNiSkin(nifly::NifFile& nif, int shapeIndex);
// ★選択中メッシュを NiSkin 化し、pending に記録する（g_NifData＋ペアを即変換）。実装は Main.cpp。
void ConvertSelectedMeshToNiSkin();
// ★NiSkin→BSD: 指定 shape を BSDismemberSkinInstance に変換し、全三角形を1パーティション（slot）に割当てる。
//   nifly SetShapePartitions(convertSkinInstance=true) を使用。実装は Main.cpp。
bool ConvertShapeToBSD(nifly::NifFile& nif, int shapeIndex, int slot);
// ★選択中 NiSkin メッシュを BSD 化（既定 slot 32）して pending に記録する。実装は Main.cpp。
void ConvertSelectedMeshToBSD();
// ★現在の NIF で「NiSkin 化指定された」shapeIndex 集合（NIF 読込でクリア）。export の toNiSkin 反映に使う。
extern std::set<int> g_ConvertedNiSkinShapes;

// ★per-mesh ディスクキャッシュ（Import DB の NIF 読込を mtime+size 検証で省略）。実装は Main.cpp。
//   単一ワーカー内でのみ使用する想定（g_IsProcessing で排他されるためロック不要）。
void PerMeshCache_Load();                                                               // disk → memory（フェーズ開始時に1回）
bool PerMeshCache_Get(const std::string& fullPath, std::vector<MeshSlot>& out);         // 有効ヒットで true（mtime+size 一致）
void PerMeshCache_Put(const std::string& fullPath, const std::vector<MeshSlot>& meshes);// 現在の mtime+size で登録
// ★スキンボーン抽出: meshes[] に加え ShapeBone も同時にキャッシュする版（cache schema /2）。
//   旧 /1 キャッシュ命中時は shapeBones が無い＝ボーン未取得とみなし NIF 再読込させる（Get は false を返す）。
bool PerMeshCache_Get(const std::string& fullPath, std::vector<MeshSlot>& out, std::vector<ShapeBone>& outBones);
void PerMeshCache_Put(const std::string& fullPath, const std::vector<MeshSlot>& meshes, const std::vector<ShapeBone>& bones);
void PerMeshCache_Save();                                                               // memory → disk（フェーズ終了時に1回）
// ★移行: 旧 slotdata-Output.txt（union のみ）を読み、各 NIF から per-mesh を復元して
//   slotdata-ChangeSet.json に変換・追記する。NIF を読むのでスレッド起動推奨（進捗・キャンセル対応）。実装 Main.cpp。
void ConvertOldSlotdataToChangeSet();
// ★Step3: slottool 連携（SlotToolBridge.cpp 実装）
void ImportDatabaseViaSlotTool();   // slottool export → JSON → g_AllRecords
// ★指定 changes から ESL パッチ生成（pending クリアせず、成功で true）。NIF 出力と併用可。
bool RunSlotPatchImport(const std::map<std::string, SlotRecord>& changesMap);
// ★#4 共有: 読み込み済み NIF に SlotRecord の per-mesh（無ければ union）スロットを適用する。
//   shapeIndex 主・name 副で照合し、toNiSkin/NiSkin↔BSD 変換も行う。SEH 保護付き（実装は Main.cpp）。
//   戻り値=何か書き込んだか。outMismatch=スロット数が NIF パーティション数を超過（呼び出し側は書込中止推奨）。
bool ApplyPerMeshSlotsToNif(nifly::NifFile& nif, const SlotRecord& rec, bool& outMismatch);
// ★統合 Export ワーカー。pending に対し選択した出力（NIF / ESL / slotdata-txt）を実行し、
//   成功分の pending をクリアする。スレッドで起動する。
void SaveAndExportAllWorker(bool doNif, bool doEsl, bool doTxt);
// ★Tier3-2: テクスチャ読込（TextureLoader.cpp 実装）。relTexPath は NIF 内の "textures\..." 相対。
//   見つからない/デコード不可なら 0 を返す（呼び出し側はフラット表示にフォールバック）。
unsigned int GetOrLoadDiffuseTexture(const std::string& relTexPath);
// ★#5: 指定フォルダ起点でテクスチャを解決（参照ボディ用）。folder/相対パス・folder/ファイル名 を試す。
unsigned int GetOrLoadTextureFromFolder(const std::string& folderRoot, const std::string& relTexPath);
void ClearTextureCache();

// ★固定・同期レイアウト: 常時表示の5パネルをメインウィンドウサイズに比例配置する。
//   各パネルは GetMainPanelRect で自分の矩形を取得し、PIN_PANEL_FLAGS で固定する。
enum class MainPanel { ControlPanel, Viewport, Database, LogConsole, Pending };
void GetMainPanelRect(MainPanel p, ImVec2& outPos, ImVec2& outSize);
constexpr ImGuiWindowFlags PIN_PANEL_FLAGS =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
void LoadNifFileCore(const std::string& path);
fs::path ConstructSafePath(const std::string& root, const std::string& rel);
void SaveSessionChangesToFile();
void SaveUnifiedConfig();        // implemented in Main.cpp
void SaveKeywordsJSON();         // ★ KID キーワードを keywords.json へ即時保存（Rules/KID 編集時に呼ぶ）

// ★#6: アプリ自身の設定/ログ/キャッシュは CWD ではなく実行ファイルのあるディレクトリ基準で開く。
//   MO2 経由起動で CWD が想定外でも config 等が散らからないようにする。実装は Globals.cpp。
std::string GetExeDir();                          // 実行ファイルのディレクトリ（末尾区切りなし）
std::string AppPath(const std::string& filename); // GetExeDir()/filename を返す

// ★B6: パース前のファイルサイズ上限チェック（巨大入力による OOM 防止）。実装は Main.cpp。
//   サイズが maxBytes 超なら what 名でエラーログを出し false。取得不能/上限内は true。
bool FileSizeOk(const fs::path& p, uintmax_t maxBytes, const char* what);
