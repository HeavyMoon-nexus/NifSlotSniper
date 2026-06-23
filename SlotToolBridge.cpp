// =============================================================================
// SlotToolBridge.cpp ★Step3
//   NIF Slot Sniper 本体から Mutagen CLI(slottool.exe) を子プロセス起動し、
//   JSON でデータ授受する。Synthesis + slotdata-*.txt を置き換える経路。
//   本体は MO2 経由起動（USVFS 内）なので、子プロセスも USVFS を継承し、
//   slottool は正しい load order を解決する。
// =============================================================================
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "Globals.h"
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <set>
#include <map>
#include <algorithm>
#include <iterator>
#include "nlohmann/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// slottool.exe を引数付きで起動し、終了を待って終了コードを返す。
// 失敗時は err にメッセージを格納し負値を返す。
static int RunSlotTool(const std::string& argLine, std::string& err)
{
#ifdef _WIN32
    if (strlen(g_SlotToolPath) == 0) { err = "SlotTool path is not set (Settings で slottool.exe を指定してください)"; return -1; }
    if (!fs::exists(g_SlotToolPath)) { err = std::string("slottool.exe not found: ") + g_SlotToolPath; return -1; }

    // ★B4: CreateProcessW（ワイド）。slottool パス・引数・一時ファイルパスに非 ASCII が含まれても
    //   正しく起動できる。コマンドラインは UTF-8 → UTF-16 へ変換し、可変ワイドバッファに積む。
    std::string cmd = "\"" + std::string(g_SlotToolPath) + "\" " + argLine;
    std::wstring wcmd = Utf8ToWide(cmd);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(L'\0');

    // ★slottool の診断は stderr に出る。終了コードだけでは「書けたが env 解放で例外→非ゼロ」等を
    //   切り分けられないため、stderr（と stdout）を一時ファイルへ捕捉し、後でログに出す。
    fs::path errLog = fs::temp_directory_path() / "nss_slottool_stderr.txt";
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hErr = CreateFileW(errLog.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    BOOL inherit = FALSE;
    if (hErr != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hErr;
        si.hStdError = hErr;
        inherit = TRUE;
    }
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: コンソール窓を出さない。USVFS フックは親から継承される。
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, inherit, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        err = "CreateProcess failed (code " + std::to_string(GetLastError()) + ")";
        return -1;
    }
    // ★A3: INFINITE 待ちだと slottool がハングした際に g_IsProcessing が永久 true になり、
    //   ウィンドウを閉じる以外に復帰不能。250ms ポーリングし、キャンセル要求（モーダル Cancel・
    //   アプリ終了時の g_CancelRequested）で TerminateProcess して復帰する。
    for (;;) {
        DWORD r = WaitForSingleObject(pi.hProcess, 250);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        if (g_CancelRequested.load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            AddLog("slottool: canceled/terminated by request.", LogType::Warning);
            break;
        }
    }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);

    // 捕捉した stderr を読み、非ゼロ終了時は Warning、Verbose 時は Info でログに出す。
    {
        std::ifstream ef(errLog);
        if (ef) {
            std::string content((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
            // 末尾の空白/改行を整理
            while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' '))
                content.pop_back();
            if (!content.empty()) {
                if (code != 0) AddLog("slottool stderr (exit " + std::to_string((int)code) + "):\n" + content, LogType::Warning);
                else if (LogVerbose()) AddLog("slottool stderr:\n" + content, LogType::Info);
            }
        }
    }
    return static_cast<int>(code);
#else
    err = "Windows only";
    return -1;
#endif
}

// "000800:Nurse.esp" -> "000800"
static std::string LocalId(const std::string& formKey)
{
    auto p = formKey.find(':');
    return (p == std::string::npos) ? formKey : formKey.substr(0, p);
}

// ★要望①: "X_SlotPatch.esp" / "X_slotdata.esp"（二重も）→ "X.esp"。
//   生成パッチが load order の勝者になると export の source がパッチ名になり、再 import で
//   <パッチ>_SlotPatch.esp が増殖する。source を元 mod に正規化して 1 パッチに集約・merge する。
//   armaFormKey/armoFormKey は定義元（元 mod）を指すため override は正しく当たる（安全）。
//   ★共有化（Globals.h で宣言）: Import / changeset 読込 / 変換 / slottool 送出前の全経路で使う。
std::string NormalizePatchSource(const std::string& src)
{
    fs::path p(src);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    auto endsWithCI = [](const std::string& s, const char* sufC) {
        std::string suf = sufC;
        if (s.size() < suf.size()) return false;
        for (size_t i = 0; i < suf.size(); ++i)
            if (::tolower((unsigned char)s[s.size() - suf.size() + i]) != ::tolower((unsigned char)suf[i])) return false;
        return true;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* suf : { "_SlotPatch", "_slotdata" }) {
            if (endsWithCI(stem, suf)) { stem = stem.substr(0, stem.size() - std::string(suf).size()); changed = true; }
        }
    }
    return stem + ext;
}

// int 配列 JSON -> "32,38"
static std::string SlotsToStr(const json& arr)
{
    std::string s; bool first = true;
    if (arr.is_array()) {
        for (const auto& v : arr) {
            if (!v.is_number_integer()) continue;
            if (!first) s += ",";
            s += std::to_string(v.get<int>());
            first = false;
        }
    }
    return s;
}

// "32, 38" -> int 配列 JSON
static json StrToSlots(const std::string& s)
{
    json arr = json::array();
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        try { arr.push_back(std::stoi(tok.substr(a, b - a + 1))); }
        catch (...) {}
    }
    return arr;
}

// ---------------------------------------------------------------------------
// slottool export → JSON → ローカル map を構築。★A: バックグラウンド実行し、
//   結果は g_ImportResult に渡してメインスレッドで g_AllRecords に適用する
//   （UI を固めない／g_AllRecords 等の構造変更は描画スレッドと競合するため）。
// ---------------------------------------------------------------------------
static void ImportWorker()
{
    // 終了時に必ず処理中フラグを戻す
    struct Guard { ~Guard() { g_ProcessCancelable = true; g_IsProcessing = false; } } guard;
    try { // ★A1: 本体全体を例外境界で保護（records 構築段や JSON 型例外も含める）。
    // ★A3: 前回キャンセルの残留で export 用 slottool を即 Terminate しないよう、開始時にクリア。
    g_CancelRequested = false;
    {
        std::lock_guard<std::mutex> lk(g_ProgressMutex);
        g_CurrentProcessItem = "Importing DB (slottool export)...";
    }

    fs::path tmp = fs::temp_directory_path() / "nss_slot_export.json";
    std::string err;

    int code = RunSlotTool("export --out \"" + tmp.string() + "\"", err);
    if (code != 0) {
        AddLog("slottool export failed: " + (err.empty() ? ("exit " + std::to_string(code)) : err), LogType::Error);
        return;
    }

    std::ifstream f(tmp);
    if (!f) { AddLog("Export JSON not found: " + tmp.string(), LogType::Error); return; }
    // ★B6: 巨大 export JSON による OOM を防ぐ（slottool 由来＝比較的信頼だが上限は設ける）。256MB。
    if (!FileSizeOk(tmp, 256ull * 1024 * 1024, "export JSON")) return;

    json j;
    try { f >> j; }
    catch (const std::exception& e) { AddLog(std::string("Export JSON parse error: ") + e.what(), LogType::Error); return; }

    if (j.value("schema", "") != "nifslot.export/1") {
        AddLog("Unexpected export schema: " + j.value("schema", "(none)"), LogType::Warning);
    }

    std::map<std::string, SlotRecord> uniqueRecords; // ★ローカルに構築（グローバルは触らない）

    int count = 0;
    if (j.contains("records") && j["records"].is_array()) {
        for (const auto& r : j["records"]) {
            SlotRecord rec;
            rec.sourceFile   = NormalizePatchSource(r.value("source", "")); // ★要望①: 二重サフィックス集約
            rec.armaFormKey  = r.value("armaFormKey", "");
            rec.armoFormKey  = r.value("armoFormKey", "");
            rec.armaFormID   = LocalId(rec.armaFormKey);
            rec.armoFormID   = LocalId(rec.armoFormKey);
            rec.armaEditorID = r.value("armaEditorId", "");
            rec.armoEditorID = r.value("armoEditorId", "");
            rec.malePath     = r.value("malePath", "");
            rec.femalePath   = r.value("femalePath", "");
            rec.armoSlots    = SlotsToStr(r.contains("armoSlots") ? r["armoSlots"] : json::array());
            rec.armaSlots    = SlotsToStr(r.contains("armaSlots") ? r["armaSlots"] : json::array());

            // ★カラバリ: altMale / altFemale（[{shape, index, diffuse}]）を vector に展開
            auto parseAlt = [](const json& arr, std::vector<AltTex>& out) {
                if (!arr.is_array()) return;
                for (const auto& a : arr) {
                    AltTex t;
                    t.shape = a.value("shape", "");
                    t.index = a.value("index", -1);
                    t.diffuse = a.value("diffuse", "");
                    if (!t.diffuse.empty()) out.push_back(t);
                }
            };
            if (r.contains("altMale"))   parseAlt(r["altMale"], rec.altMale);
            if (r.contains("altFemale")) parseAlt(r["altFemale"], rec.altFemale);

            // gender 優先パス（txt ローダと同じ規則）
            if (g_TargetGender == 0)
                rec.nifPath = !rec.malePath.empty() ? rec.malePath : rec.femalePath;
            else
                rec.nifPath = !rec.femalePath.empty() ? rec.femalePath : rec.malePath;

            rec.baseNifKeyMale   = rec.malePath.empty() ? "" : GetBaseNifKey(rec.malePath);
            rec.baseNifKeyFemale = rec.femalePath.empty() ? "" : GetBaseNifKey(rec.femalePath);
            rec.baseNifKey = !rec.baseNifKeyMale.empty() ? rec.baseNifKeyMale : rec.baseNifKeyFemale;
            // ★A4: 表示名。EditorID が空でも区別できるよう FormID/source をフォールバック表示。
            rec.displayText = !rec.armaEditorID.empty()
                ? rec.armaEditorID
                : ("[" + (rec.armaFormID.empty() ? "?" : rec.armaFormID) + " " + rec.sourceFile + "]");

            // ★A4: 重複排除キーを ARMA EditorID → ARMA FormKey に変更。
            //   EditorID は空・重複しうるため、別 ARMA が潰れて「3 ARMA→2 検出」になっていた。
            //   FormKey は ARMA ごとに一意。同一 ARMA が複数 ARMO から参照される場合のみ集約
            //   （AlternateTextures は ARMA 固有なのでカラバリ情報は失われない）。
            //   FormKey 空（txt 由来）のときのみ EditorID にフォールバック。
            std::string uniqueKey = !rec.armaFormKey.empty() ? rec.armaFormKey : rec.armaEditorID;
            uniqueRecords[uniqueKey] = rec;
            count++;
        }
    }

    // ★per-mesh（Option C）: 各レコードの表示 gender NIF を読み、shape 毎スロット meshes[] と
    //   blockingSlots(= armaSlots − union(meshes)) を構築する。ユニーク NIF パス単位でキャッシュ。
    //   重いので進捗・キャンセル可。キャンセル時/読込不可時は meshes 空のまま（export 時に union フォールバック）。
    {
        std::lock_guard<std::mutex> lk(g_ProgressMutex);
        g_CurrentProcessItem = "Reading per-mesh slots from NIFs...";
    }
    g_ProcessCancelable = true;   // NIF 読込フェーズはキャンセル可
    g_CancelRequested = false;
    g_Progress = 0.0f;

    // ★堅牢化: per-mesh/キャッシュ処理での例外は detach スレッドを terminate（"Debug Error"=abort）させる。
    //   フェーズ全体を try で囲み、失敗しても DB 自体は（per-mesh 無し＝union フォールバックで）読み込めるようにする。
    // ★スケルトン解決パス: 表示 gender のスケルトン名集合を一度だけ構築（複数 NIF 可・';' 区切り）。
    std::set<std::string> skelSet; std::string skelId;
    {
        const char* skelCfg = (g_TargetGender == 0) ? g_SkeletonPathMale : g_SkeletonPathFemale;
        if (skelCfg && strlen(skelCfg) > 0) {
            if (BuildSkeletonBoneSet(skelCfg, skelSet, skelId))
                AddLog("Import: skeleton bone set built from '" + skelId + "' (" + std::to_string(skelSet.size()) + " nodes).", LogType::Info);
            else
                AddLog("Import: skeleton NIF(s) unreadable; boneResolution will be null.", LogType::Warning);
        }
        else AddLog("Import: skeleton path not set (Settings); boneResolution will be null. Bones are still extracted.", LogType::Info);
    }
    std::string genderLabel = (g_TargetGender == 0) ? "male" : "female";

    try {
    PerMeshCache_Load(); // ★ディスクキャッシュ（前回の per-mesh 結果）を読み込む
    std::map<std::string, std::vector<MeshSlot>> nifMeshCache; // この import 内の重複読込防止: lower(full path) -> meshes
    std::map<std::string, std::vector<ShapeBone>> nifBoneCache; // ★スキンボーン抽出: 同じく重複読込防止
    int total = (int)uniqueRecords.size();
    int cur = 0, withMesh = 0, cacheHit = 0, nifRead = 0;
    for (auto& [key, rec] : uniqueRecords) {
        if (g_CancelRequested) { AddLog("Import: per-mesh read canceled; remaining records use union fallback.", LogType::Warning); break; }
        ++cur;
        g_Progress = total > 0 ? (float)cur / (float)total : 1.0f;

        std::string rel = (g_TargetGender == 0) ? rec.malePath : rec.femalePath;
        if (rel.empty()) rel = (g_TargetGender == 0) ? rec.femalePath : rec.malePath;
        if (rel.empty()) continue;
        std::string full = ConstructSafePath(g_InputRootPath, rel).string();
        std::string lower = full;
        AsciiLowerInplace(lower); // ★B5: per-mesh 重複読込キー（パス）

        std::vector<MeshSlot> meshes;
        std::vector<ShapeBone> shapeBones;
        auto it = nifMeshCache.find(lower);
        if (it != nifMeshCache.end()) {
            meshes = it->second; // 同一 import 内で既に解決済み（stat も省略）
            shapeBones = nifBoneCache[lower];
        }
        else {
            {
                std::lock_guard<std::mutex> lk(g_ProgressMutex);
                g_CurrentProcessItem = "Per-mesh: " + fs::path(full).filename().string();
            }
            // ★ディスクキャッシュ命中（mtime+size 一致 ∧ ボーン込み /2 エントリ）なら NIF を開かない＝最大の高速化点。
            if (PerMeshCache_Get(full, meshes, shapeBones)) {
                ++cacheHit;
            }
            else {
                ReadNifPerMeshSlotsAndBones(full, meshes, shapeBones); // 失敗時は空（union フォールバック）
                PerMeshCache_Put(full, meshes, shapeBones);            // 空（NiSkin/静的）も含めて登録（次回の再読込を防ぐ）
                ++nifRead;
            }
            nifMeshCache[lower] = meshes;
            nifBoneCache[lower] = shapeBones;
        }

        // ★スキンボーン抽出: ボーンとスケルトン解決は meshes（BSD）の有無に関わらず常に付与する。
        rec.shapeBones = shapeBones;
        ComputeBoneResolution(rec, skelSet, skelId, genderLabel);

        if (meshes.empty()) continue; // dismember なし/ロード不可 → union フォールバック

        rec.meshes = meshes;
        // blockingSlots = armaSlots − union(meshes.slots)
        std::set<int> meshUnion;
        for (const auto& m : meshes) for (int s : m.slots) meshUnion.insert(s);
        std::vector<int> arma = ParseSlotString(rec.armaSlots);
        rec.blockingSlots.clear();
        for (int s : arma) if (!meshUnion.count(s)) rec.blockingSlots.push_back(s);
        ++withMesh;
    }
    PerMeshCache_Save(); // ★更新したキャッシュを書き戻す（次回 Import DB を高速化）
    AddLog("Import: per-mesh for " + std::to_string(withMesh) + "/" + std::to_string(total)
        + " records (" + std::to_string((int)nifMeshCache.size()) + " unique NIFs; cache hit "
        + std::to_string(cacheHit) + ", read " + std::to_string(nifRead) + ").", LogType::Info);
    }
    catch (const std::exception& e) {
        AddLog(std::string("Import: per-mesh phase error (DB will load with union fallback): ") + e.what(), LogType::Error);
    }
    catch (...) {
        AddLog("Import: per-mesh phase unknown error (DB will load with union fallback).", LogType::Error);
    }

    // ★衣装シード: トグル ON のとき、全件 DB（未編集アクセサリ含む）から costume_seed.json を出力。
    //   changeset とは独立（履歴を汚さない）。swap 前の uniqueRecords が全件を保持している。
    if (g_ExportCostumeSeed) {
        try { WriteCostumeSeed(uniqueRecords); }
        catch (const std::exception& e) { AddLog(std::string("Import: costume seed write error: ") + e.what(), LogType::Warning); }
        catch (...) { AddLog("Import: costume seed write unknown error.", LogType::Warning); }
    }

    // ★結果をメインスレッドへ受け渡し（適用は Main.cpp の loop で BuildRecordsAndTree）
    {
        std::lock_guard<std::recursive_mutex> lk(g_DataMutex);
        g_ImportResult.swap(uniqueRecords);
    }
    g_ImportResultReady = true;
    AddLog("slottool export parsed (" + std::to_string(count) + " records). Applying...", LogType::Info);
    }
    catch (const std::exception& e) { AddLog(std::string("Import worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("Import worker unknown error.", LogType::Error); }
}

void ImportDatabaseViaSlotTool()
{
    if (g_IsProcessing.load()) {
        AddLog("Busy - please wait for the current operation to finish.", LogType::Warning);
        return;
    }
    g_IsProcessing = true;       // ここで先に立てて二重起動を防ぐ
    g_ProcessCancelable = false; // export/import はキャンセル不可
    std::thread(ImportWorker).detach();
}

// ---------------------------------------------------------------------------
// g_SessionChanges → import JSON → slottool import（override-only ESL パッチ生成）
// ---------------------------------------------------------------------------
// ★分離: 指定した changes から override-only ESL パッチを生成する（pending はクリアしない）。
//   成功で true。NIF 出力と組み合わせて呼べるよう、g_SessionChanges に依存しない形にした。
bool RunSlotPatchImport(const std::map<std::string, SlotRecord>& changesMap)
{
    if (changesMap.empty()) { AddLog("No pending changes to generate patches.", LogType::Warning); return false; }

    json doc;
    doc["schema"] = "nifslot.import/1";

    // 出力先: Output Root が設定されていればそこへ絶対出力する。
    //   MO2 の mods 配下の mod フォルダ（例: <MO2>\mods\SlotPatches）を Output Root に
    //   設定しておくと、生成 ESP がプラグインとして MO2 に認識される。
    //   未設定だと overwrite\SlotPatches（サブフォルダ）に出るが、MO2 はサブフォルダ内の
    //   プラグインを認識しないため、手動で mod 化が必要になる。
    if (g_ForceOverwrite) {
        // ★Force Overwrite: パッチ esp を作らず、オリジナルのプラグインに直接スロットを書き込む。
        //   .bak バックアップは slottool 側が作成する（C++ は ESP パスを知らないため）。
        doc["output"] = { {"mode", "overwrite-original"} };
        AddLog("Direct Overwrite: writing slots directly into the ORIGINAL plugins (slottool makes .bak backups). No patch ESP is generated.", LogType::Warning);
    }
    else if (strlen(g_OutputRootPath) > 0) {
        doc["output"] = { {"mode", "absolute"}, {"path", std::string(g_OutputRootPath)} };
    }
    else {
        doc["output"] = { {"mode", "overwrite"}, {"path", "SlotPatches"} };
        AddLog("Output Root 未設定: overwrite\\SlotPatches に出力します。MO2 でプラグイン認識させるには "
               "Settings の Output Root を <MO2>\\mods\\<任意フォルダ> に設定してください。", LogType::Warning);
    }

    json changes = json::array();
    // ★#7(C): full FormKey が無く FormID+source から「再構成」した件を可視化する。再構成は source が
    //   勝者 mod（≠定義元）の場合に誤った plugin を指し、slottool 側で unresolved になり得るため、
    //   どのレコードが再構成依存かを警告に出す（件数＋先頭いくつかの識別子）。
    int reconstructed = 0;
    std::vector<std::string> reconNames;
    for (const auto& [k, rec] : changesMap) {
        // ★要望①: source を元 mod に正規化（_SlotPatch/_slotdata 二重を剥がす）。これにより
        //   esp 名が <元>_SlotPatch.esp に集約され、FormKey 再構成（保険）も元 mod を指すので解決できる。
        std::string normSource = NormalizePatchSource(rec.sourceFile);
        // 完全 FormKey があればそれを、無ければ FormID + 正規化 source から再構成（txt 由来の保険）
        bool armaRecon = rec.armaFormKey.empty() && !rec.armaFormID.empty();
        bool armoRecon = rec.armoFormKey.empty() && !rec.armoFormID.empty();
        std::string armaKey = !rec.armaFormKey.empty() ? rec.armaFormKey
            : (rec.armaFormID.empty() ? "" : rec.armaFormID + ":" + normSource);
        std::string armoKey = !rec.armoFormKey.empty() ? rec.armoFormKey
            : (rec.armoFormID.empty() ? "" : rec.armoFormID + ":" + normSource);
        if (armaRecon || armoRecon) {
            ++reconstructed;
            if (reconNames.size() < 8) {
                std::string nm = rec.armaEditorID.empty() ? (rec.armaFormID + " " + normSource) : rec.armaEditorID;
                reconNames.push_back(nm);
            }
        }

        json ch;
        ch["source"]      = normSource;
        ch["armaFormKey"] = armaKey;
        ch["armaSlots"]   = StrToSlots(rec.armaSlots);
        ch["armoFormKey"] = armoKey;
        ch["armoSlots"]   = StrToSlots(rec.armoSlots);
        changes.push_back(ch);
    }
    doc["changes"] = changes;

    if (reconstructed > 0) {
        std::string names;
        for (const auto& n : reconNames) { if (!names.empty()) names += ", "; names += n; }
        if ((int)reconNames.size() < reconstructed) names += ", ...";
        AddLog("ESL: " + std::to_string(reconstructed) + " change(s) have NO full FormKey and rely on "
               "FormID+source reconstruction, which may resolve to the WRONG plugin (watch for "
               "'ARMA/ARMO unresolved' below). Run 'Import DB (slottool)' then 'Convert old TXT' to backfill "
               "real FormKeys. Affected: " + names, LogType::Warning);
    }

    fs::path tmp = fs::temp_directory_path() / "nss_slot_import.json";
    {
        std::ofstream of(tmp);
        if (!of) { AddLog("Failed to write import JSON: " + tmp.string(), LogType::Error); return false; }
        of << doc.dump(2);
    }

    std::string err;
    AddLog("Running slottool import (" + std::to_string(changes.size()) + " changes)...", LogType::Info);
    int code = RunSlotTool("import --in \"" + tmp.string() + "\"", err);
    if (code != 0) {
        AddLog("slottool import failed: " + (err.empty() ? ("exit " + std::to_string(code)) : err), LogType::Error);
        return false; // 失敗時は呼び出し側が pending を保持（やり直せるように）
    }
    AddLog("slottool import completed. ESL patches generated.", LogType::Success);
    return true;
}
