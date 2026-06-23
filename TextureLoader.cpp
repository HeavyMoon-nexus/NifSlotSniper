// =============================================================================
// TextureLoader.cpp ★Tier3-2
//   NIF の diffuse テクスチャ（DDS）をルーズファイルから読み込み、OpenGL テクスチャ化する。
//   BC1/BC2/BC3/BC5/BC7 圧縮 + 非圧縮 RGBA/BGRA に対応。mip0 のみ使用（プレビュー用途）。
//   BSA 内テクスチャは対象外（見つからなければ 0 を返し、呼び出し側はフラット表示）。
//   将来 BSArch 連携で BSA 対応を追加する余地を残す。
// =============================================================================
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // ★① BSArch 子プロセス起動用
#endif

#include "Globals.h"
#include <glad/glad.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>

namespace fs = std::filesystem;

// glad が定義していない場合に備えて圧縮フォーマット定数を用意
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif

// ★① キャッシュ世代管理（NIF 単位の上限）。
//   旧実装は「テクスチャ枚数」で描画中に評価・クリアしていたため、1 つの NIF が
//   上限超のテクスチャを要求すると「読込→超過でクリア→未ロードなので再読込→…」の
//   無限ループに陥った。対策として:
//     - クリア判定は **メイン NIF 切替の瞬間だけ**（描画ループ中は一切クリアしない）。
//       → 現在表示中の NIF が要求するテクスチャは何枚あっても途中で消えない＝ループ原理消滅。
//     - 上限は「テクスチャ枚数」ではなく **直近 N 个の NIF**（世代）で管理。
//     - ref body 由来（"folder|" キー）は常駐＝世代対象外で常に保持。
//   各エントリは「GL id + 読み込んだ世代(gen)」を持つ。ref は REF_GEN(=-1)。
//   負キャッシュ(id=0)は現世代で保持し、世代が窓から外れたら破棄（=再試行可）。
struct TexEntry { unsigned int id = 0; long gen = 0; };
static std::map<std::string, TexEntry> g_TexCache;

static long g_TexGeneration = 0;       // メイン NIF を切り替えるたびに +1
static std::string g_TexLastNifKey;    // 直近に世代を進めたメイン NIF（同一なら据え置き）
static constexpr long REF_GEN = -1;    // ref body 用：世代対象外（常駐）

// ref body 由来テクスチャは "folder|" プレフィックスのキー（GetOrLoadTextureFromFolder）。
static bool IsRefTexKey(const std::string& k) { return k.rfind("folder|", 0) == 0; }

// キャッシュ格納。クリアはここでは行わない（NIF 切替時のみ）。
//   ref キーは REF_GEN（常駐）、それ以外は現世代でタグ付け。
static void CacheStore(const std::string& key, unsigned int tex) {
    g_TexCache[key] = TexEntry{ tex, IsRefTexKey(key) ? REF_GEN : g_TexGeneration };
}

// キャッシュ参照。ヒット時、管理対象は現世代に更新（直近 N NIF で使われたものを保持）。
//   見つかれば true を返し outId に id（負キャッシュは 0）を入れる。
static bool CacheLookup(const std::string& key, unsigned int& outId) {
    auto it = g_TexCache.find(key);
    if (it == g_TexCache.end()) return false;
    if (it->second.gen != REF_GEN) it->second.gen = g_TexGeneration; // LRU 的リフレッシュ
    outId = it->second.id;
    return true;
}

static uint32_t FourCC(const char* s) {
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8)
        | ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}

// NIF 相対テクスチャパスを実ファイルに解決する。MO2 経由なら USVFS が Data 配下の
// ルーズテクスチャを見せるため、g_GameDataPath 起点で探せばよい。
static std::string ResolveTexturePath(const std::string& relRaw) {
    if (relRaw.empty() || strlen(g_GameDataPath) == 0) return "";
    std::string rel = relRaw;
    std::replace(rel.begin(), rel.end(), '/', '\\');
    // 先頭の余計な区切りを除去
    while (!rel.empty() && (rel.front() == '\\')) rel.erase(rel.begin());

    std::string lower = rel;
    AsciiLowerInplace(lower); // ★B5

    std::vector<fs::path> candidates;
    fs::path dataRoot(g_GameDataPath);
    if (lower.rfind("textures", 0) == 0) {
        candidates.push_back(dataRoot / rel);
    }
    else {
        candidates.push_back(dataRoot / "textures" / rel);
        candidates.push_back(dataRoot / rel);
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c.string();
    }
    return "";
}

// DDS をパースして GL テクスチャを生成。失敗で 0。
static unsigned int LoadDDSFile(const std::string& fullPath) {
    std::ifstream f(fullPath, std::ios::binary);
    if (!f) return 0;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 128) return 0;

    if (std::memcmp(buf.data(), "DDS ", 4) != 0) return 0;
    auto rd32 = [&](size_t off) -> uint32_t {
        uint32_t v; std::memcpy(&v, buf.data() + off, 4); return v;
        };

    uint32_t height = rd32(12);
    uint32_t width = rd32(16);
    uint32_t pfFlags = rd32(80);
    uint32_t fourCC = rd32(84);

    size_t dataOffset = 128; // 4(magic)+124(header)
    GLenum glFormat = 0;
    bool compressed = true;
    int blockBytes = 16;
    GLenum unc_format = 0, unc_type = GL_UNSIGNED_BYTE;

    const uint32_t DDPF_FOURCC = 0x4;
    if (pfFlags & DDPF_FOURCC) {
        if (fourCC == FourCC("DXT1")) { glFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; blockBytes = 8; }
        else if (fourCC == FourCC("DXT3")) { glFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; blockBytes = 16; }
        else if (fourCC == FourCC("DXT5")) { glFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; blockBytes = 16; }
        else if (fourCC == FourCC("ATI2") || fourCC == FourCC("BC5U")) { glFormat = GL_COMPRESSED_RG_RGTC2; blockBytes = 16; }
        else if (fourCC == FourCC("DX10")) {
            if (buf.size() < 148) return 0;
            uint32_t dxgi = rd32(128);
            dataOffset = 148; // +20 (DXT10 header)
            switch (dxgi) {
            case 70: case 71: case 72: glFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; blockBytes = 8; break;
            case 73: case 74: case 75: glFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; blockBytes = 16; break;
            case 76: case 77: case 78: glFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; blockBytes = 16; break;
            case 82: case 83: case 84: glFormat = GL_COMPRESSED_RG_RGTC2; blockBytes = 16; break;
            case 98: case 99: glFormat = GL_COMPRESSED_RGBA_BPTC_UNORM; blockBytes = 16; break;
            case 28: compressed = false; unc_format = GL_RGBA; break; // R8G8B8A8_UNORM
            case 87: compressed = false; unc_format = GL_BGRA; break; // B8G8R8A8_UNORM
            default: return 0; // BC6H 等は未対応
            }
        }
        else return 0;
    }
    else {
        // 非圧縮（RGB/RGBA）。ここでは RGBA8 / BGRA8 のみ簡易対応。
        uint32_t rgbBitCount = rd32(88);
        if (rgbBitCount == 32) { compressed = false; unc_format = GL_BGRA; }
        else return 0;
    }

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // mip 不使用
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (compressed) {
        uint32_t blocksW = (width + 3) / 4; if (blocksW == 0) blocksW = 1;
        uint32_t blocksH = (height + 3) / 4; if (blocksH == 0) blocksH = 1;
        size_t imgSize = (size_t)blocksW * blocksH * blockBytes;
        if (dataOffset + imgSize > buf.size()) { glDeleteTextures(1, &tex); return 0; }
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, glFormat, width, height, 0,
            (GLsizei)imgSize, buf.data() + dataOffset);
    }
    else {
        size_t imgSize = (size_t)width * height * 4;
        if (dataOffset + imgSize > buf.size()) { glDeleteTextures(1, &tex); return 0; }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
            unc_format, unc_type, buf.data() + dataOffset);
    }
    // GL エラーが出ていたら失敗扱い（拡張未対応など）
    if (glGetError() != GL_NO_ERROR) { glDeleteTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, 0); return 0; }
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// =============================================================================
// ★① BSA 内テクスチャ対応（BSArch 連携）
//   ルーズで見つからない場合のフォールバック。
//   1) 初回: Data 内の全 *.bsa を `BSArch -list` して「textures\... → BSA」索引を作る（高速）。
//   2) 必要なテクスチャを含む BSA だけを一度 unpack してキャッシュ（重い展開は対象限定）。
//   3) キャッシュから DDS を読む。
// =============================================================================
// ★A: 索引/展開はバックグラウンドスレッドで行い UI を固めない。
static std::atomic<bool> g_BsaIndexReady{ false };    // 索引構築完了
static std::map<std::string, std::string> g_BsaIndex; // normKey -> bsa full path（g_BsaMutex 保護）
static std::set<std::string> g_BsaUnpacked;           // 展開済み bsa パス（g_BsaMutex 保護）
static std::mutex g_BsaMutex;                          // 上記2つを保護
static std::string g_BsaCacheDir;                     // %TEMP%\nss_bsa_cache

// テクスチャパスを索引キーに正規化（小文字・バックスラッシュ・"textures\" 前置）
static std::string NormTexKey(const std::string& p) {
    std::string s = p;
    std::replace(s.begin(), s.end(), '/', '\\');
    AsciiLowerInplace(s); // ★B5: テクスチャ索引キー（パス）
    while (!s.empty() && s.front() == '\\') s.erase(s.begin());
    if (s.rfind("textures\\", 0) != 0) s = "textures\\" + s;
    return s;
}

#ifdef _WIN32
// BSArch を引数付きで起動（出力を捨てる）。終了コードを返す。
static int RunBsArch(const std::string& args) {
    // ★B4: CreateProcessW（ワイド）。BSArch パス・引数の非 ASCII を正しく扱う。
    std::wstring wcmd = Utf8ToWide("\"" + std::string(g_BsArchPath) + "\" " + args);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end()); buf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    // ★A3: BSArch ハング時に永久待ちにならないよう、ポーリング＋キャンセルで TerminateProcess。
    for (;;) {
        DWORD r = WaitForSingleObject(pi.hProcess, 250);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        if (g_CancelRequested.load()) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
    }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// BSArch を起動し標準出力を outFile に保存（-list 用）。
static int RunBsArchCapture(const std::string& args, const std::string& outFile) {
    // ★B4: CreateFileW / CreateProcessW（ワイド）。出力先・BSArch パスの非 ASCII を正しく扱う。
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hOut = CreateFileW(Utf8ToWide(outFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) return -1;
    std::wstring wcmd = Utf8ToWide("\"" + std::string(g_BsArchPath) + "\" " + args);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end()); buf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = hOut; si.hStdError = hOut;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) { CloseHandle(hOut); return -1; }
    // ★A3: BSArch ハング時に永久待ちにならないよう、ポーリング＋キャンセルで TerminateProcess。
    for (;;) {
        DWORD r = WaitForSingleObject(pi.hProcess, 250);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        if (g_CancelRequested.load()) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
    }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hOut);
    return (int)code;
}
#else
static int RunBsArch(const std::string&) { return -1; }
static int RunBsArchCapture(const std::string&, const std::string&) { return -1; }
#endif

// ★A: BSA 索引をバックグラウンドで構築（Data 内全 *.bsa を -list）。UI を固めない。
static void BuildBsaIndexWorker() {
    struct Guard { ~Guard() { g_ProcessCancelable = true; g_IsProcessing = false; } } guard;
    g_CancelRequested = false; // ★A3: 残留キャンセルで BSArch を即 Terminate しないようクリア。
    try { // ★A1: detach ワーカーから例外が抜けると terminate。
    {
        std::lock_guard<std::mutex> lk(g_ProgressMutex);
        g_CurrentProcessItem = "Building BSA texture index...";
    }

    std::map<std::string, std::string> localIndex; // ローカルに構築してから受け渡し
    if (strlen(g_BsArchPath) != 0 && strlen(g_GameDataPath) != 0
        && fs::exists(g_BsArchPath) && fs::exists(g_GameDataPath)) {
        fs::path tmpList = fs::temp_directory_path() / "nss_bsa_list.txt";
        int bsaCount = 0, texCount = 0;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(g_GameDataPath, ec)) {
            std::string ext = e.path().extension().string();
            AsciiLowerInplace(ext); // ★B5
            if (ext != ".bsa") continue;
            std::string bsa = e.path().string();
            if (RunBsArchCapture("\"" + bsa + "\" -list", tmpList.string()) != 0) continue;
            ++bsaCount;
            std::ifstream f(tmpList);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string low = line;
                AsciiLowerInplace(low); // ★B5: BSA 内テクスチャパス索引キー
                if (low.rfind("textures\\", 0) == 0) {
                    if (localIndex.find(low) == localIndex.end()) localIndex[low] = bsa; // 先勝ち
                    ++texCount;
                }
            }
        }
        AddLog("BSA index: " + std::to_string(texCount) + " textures in " + std::to_string(bsaCount) + " archives.", LogType::Info);
    }
    {
        std::lock_guard<std::mutex> lk(g_BsaMutex);
        g_BsaIndex.swap(localIndex);
    }
    g_BsaIndexReady = true;
    }
    catch (const std::exception& e) { AddLog(std::string("BSA index worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("BSA index worker unknown error.", LogType::Error); }
}

// ★A: 指定 BSA をバックグラウンドで展開してキャッシュする。
static void UnpackBsaWorker(std::string bsa) {
    struct Guard { ~Guard() { g_ProcessCancelable = true; g_IsProcessing = false; } } guard;
    g_CancelRequested = false; // ★A3: 残留キャンセルで BSArch を即 Terminate しないようクリア。
    try { // ★A1: detach ワーカーから例外が抜けると terminate。
    std::string bsaName = fs::path(bsa).stem().string();
    {
        std::lock_guard<std::mutex> lk(g_ProgressMutex);
        g_CurrentProcessItem = "BSA Unpacking... (" + bsaName + ")";
    }
    fs::path cacheSub = fs::path(g_BsaCacheDir) / bsaName;
    std::error_code ec; fs::create_directories(cacheSub, ec);
    RunBsArch("unpack \"" + bsa + "\" \"" + cacheSub.string() + "\"");
    {
        std::lock_guard<std::mutex> lk(g_BsaMutex);
        g_BsaUnpacked.insert(bsa);
    }
    AddLog("BSA unpacked: " + bsaName, LogType::Success);
    }
    catch (const std::exception& e) { AddLog(std::string("BSA unpack worker error: ") + e.what(), LogType::Error); }
    catch (...)                     { AddLog("BSA unpack worker unknown error.", LogType::Error); }
}

// 索引→対象BSA展開→キャッシュ読込（GL はメインスレッド）。状態を outStatus に返す。
//   Pending: バックグラウンド処理中（呼び出し側は負キャッシュせず再試行する）。
enum class BsaStatus { Loaded, Pending, Miss };
static unsigned int TryLoadFromBsa(const std::string& relTexPath, BsaStatus& outStatus) {
    outStatus = BsaStatus::Miss;
    if (strlen(g_BsArchPath) == 0) return 0;
    if (g_BsaCacheDir.empty()) g_BsaCacheDir = (fs::temp_directory_path() / "nss_bsa_cache").string();

    // 索引未構築 → バックグラウンド構築を開始して保留
    if (!g_BsaIndexReady.load()) {
        if (!g_IsProcessing.load()) {
            g_IsProcessing = true; g_ProcessCancelable = false;
            std::thread(BuildBsaIndexWorker).detach();
        }
        outStatus = BsaStatus::Pending;
        return 0;
    }

    std::string key = NormTexKey(relTexPath);
    std::string bsa;
    bool unpacked = false;
    {
        std::lock_guard<std::mutex> lk(g_BsaMutex);
        auto it = g_BsaIndex.find(key);
        if (it == g_BsaIndex.end()) { outStatus = BsaStatus::Miss; return 0; } // どの BSA にも無い
        bsa = it->second;
        unpacked = (g_BsaUnpacked.find(bsa) != g_BsaUnpacked.end());
    }

    if (unpacked) {
        std::string bsaName = fs::path(bsa).stem().string();
        fs::path ddsPath = fs::path(g_BsaCacheDir) / bsaName / key;
        std::error_code ec;
        if (fs::exists(ddsPath, ec)) { outStatus = BsaStatus::Loaded; return LoadDDSFile(ddsPath.string()); }
        outStatus = BsaStatus::Miss; // 展開済みだが該当ファイル無し
        return 0;
    }

    // 未展開 → バックグラウンド展開を開始して保留
    if (!g_IsProcessing.load()) {
        g_IsProcessing = true; g_ProcessCancelable = false;
        std::thread(UnpackBsaWorker, bsa).detach();
    }
    outStatus = BsaStatus::Pending;
    return 0;
}

unsigned int GetOrLoadDiffuseTexture(const std::string& relTexPath) {
    if (relTexPath.empty()) return 0;
    std::string key = relTexPath;
    AsciiLowerInplace(key); // ★B5: テクスチャキャッシュキー
    std::replace(key.begin(), key.end(), '/', '\\');

    unsigned int cached;
    if (CacheLookup(key, cached)) return cached; // 負キャッシュ(0)も含む

    std::string full = ResolveTexturePath(relTexPath);
    bool looseExists = !full.empty();
    unsigned int tex = looseExists ? LoadDDSFile(full) : 0;
    if (tex != 0) {
        if (LogVerbose()) AddLog("[Tex] LOOSE: " + relTexPath, LogType::Info);
        CacheStore(key, tex); return tex; // ルーズ成功
    }
    // ★診断: ルーズが存在するのにデコード失敗（未対応フォーマット等）。loose/BSA 併存時、
    //   壊れた/未対応のルーズが BSA の正しい変種を覆い隠す典型パターン。BSA を試す。
    if (looseExists)
        AddLog("[Tex] LOOSE found but decode FAILED (trying BSA): " + full, LogType::Warning);

    // ★① ルーズで見つからない/読めない → BSA から探す（索引/展開はバックグラウンド）
    BsaStatus st;
    unsigned int btex = TryLoadFromBsa(relTexPath, st);
    if (st == BsaStatus::Loaded) {
        if (LogVerbose()) AddLog("[Tex] BSA: " + relTexPath, LogType::Info);
        CacheStore(key, btex); return btex;
    }
    if (st == BsaStatus::Pending) return 0; // ★保留中は負キャッシュしない（展開完了後に再試行される）

    // どこにも無い（恒久的な miss）→ 0 をキャッシュして再探索を止める
    AddLog("Texture not found / unsupported: " + relTexPath, LogType::Warning);
    CacheStore(key, 0);
    return 0;
}

// ★#5: 指定フォルダ起点でテクスチャを解決して読み込む（参照ボディ用）。
unsigned int GetOrLoadTextureFromFolder(const std::string& folderRoot, const std::string& relTexPath) {
    if (folderRoot.empty() || relTexPath.empty()) return 0;
    std::string cacheKey = "folder|" + folderRoot + "|" + relTexPath;
    AsciiLowerInplace(cacheKey); // ★B5: 参照ボディ テクスチャキャッシュキー
    std::replace(cacheKey.begin(), cacheKey.end(), '/', '\\');
    unsigned int cached;
    if (CacheLookup(cacheKey, cached)) return cached;

    std::string rel = relTexPath;
    std::replace(rel.begin(), rel.end(), '/', '\\');
    while (!rel.empty() && rel.front() == '\\') rel.erase(rel.begin());
    std::string low = rel;
    AsciiLowerInplace(low); // ★B5

    fs::path root(folderRoot);
    std::vector<fs::path> cands;
    cands.push_back(root / rel);                                  // folder/<相対パスそのまま>
    if (low.rfind("textures\\", 0) == 0)
        cands.push_back(root / rel.substr(9));                    // folder/<"textures\" を除いた相対>
    cands.push_back(root / fs::path(rel).filename());            // folder/<ファイル名のみ>

    unsigned int tex = 0;
    for (const auto& c : cands) {
        std::error_code ec;
        if (fs::exists(c, ec)) { tex = LoadDDSFile(c.string()); if (tex) break; }
    }
    CacheStore(cacheKey, tex); // ref body キー("folder|...")は自動クリア対象外
    return tex;
}

void ClearTextureCache() {
    for (auto& [k, e] : g_TexCache) if (e.id) glDeleteTextures(1, &e.id);
    g_TexCache.clear();
    g_TexGeneration = 0;
    g_TexLastNifKey.clear();
}

// ★① メイン NIF 切替時に呼ぶ。前回と同じ NIF なら何もしない（スロット編集等での
//   メッシュ再構築では世代を進めない）。異なる NIF なら世代を 1 進め、保持窓
//   （直近 g_TexCacheNifLimit 世代）から外れた管理テクスチャだけを解放する。
//   ref body（REF_GEN）は常に保持。描画ループからは絶対に呼ばないこと（ループ防止）。
void TexCacheOnMainNifSwitch(const std::string& nifKey) {
    if (nifKey == g_TexLastNifKey) return; // 同一 NIF → 世代据え置き
    g_TexLastNifKey = nifKey;
    ++g_TexGeneration;

    if (g_TexCacheNifLimit <= 0) return; // 0 以下 = 無制限（クリアしない）

    // 保持する最古世代 = 現世代 - (N-1)。これ未満の管理テクスチャを解放。
    long minKeepGen = g_TexGeneration - (g_TexCacheNifLimit - 1);
    int freed = 0;
    for (auto it = g_TexCache.begin(); it != g_TexCache.end(); ) {
        if (it->second.gen != REF_GEN && it->second.gen < minKeepGen) {
            if (it->second.id != 0) { glDeleteTextures(1, &it->second.id); ++freed; }
            it = g_TexCache.erase(it);
        }
        else ++it;
    }
    if (freed > 0)
        AddLog("Texture cache: freed " + std::to_string(freed)
            + " from older NIFs (keep last " + std::to_string(g_TexCacheNifLimit)
            + ", ref body kept).", LogType::Info);
}
