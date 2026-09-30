#include "utils/dict_dir_resolver.h"

#include "common/exception/binder.h"
#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lbug {
namespace fts_extension {

namespace fs = std::filesystem;

namespace {

// Anchor whose address lives inside the fts extension module, used to locate
// the module at runtime. noinline keeps the symbol in this translation unit
// so the address is never merged into a caller.
#if defined(_MSC_VER)
__declspec(noinline)
#endif
const void* moduleAnchor() {
    return reinterpret_cast<const void*>(&moduleAnchor);
}

std::string normalizePath(std::string path) {
    for (auto& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

#ifdef _WIN32
std::wstring utf8ToWstring(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

std::string wstringToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size,
        nullptr, nullptr);
    return result;
}

std::string getModuleDir() {
    HMODULE module = nullptr;
    // FROM_ADDRESS + the anchor's address resolves the module handle of this
    // extension without knowing its file name in advance.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) {
        return "";
    }
    DWORD capacity = 1024;
    std::vector<wchar_t> buffer(capacity);
    while (true) {
        auto len = GetModuleFileNameW(module, buffer.data(), capacity);
        if (len == 0) {
            return "";
        }
        if (len < capacity - 1) {
            // GetModuleFileNameW returns the full module FILE path; strip the
            // file name so callers anchor sub-directories (fts_dict / dict)
            // next to the binary, not under the file name.
            auto fullPath = wstringToUtf8(std::wstring(buffer.data(), len));
            return normalizePath(fs::path(fullPath).parent_path().string());
        }
        if (capacity >= 65536) {
            return "";
        }
        capacity *= 2;
        buffer.resize(capacity);
    }
}
#else
std::string getModuleDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&moduleAnchor), &info) == 0 || info.dli_fname == nullptr) {
        return "";
    }
    return normalizePath(fs::path(info.dli_fname).parent_path().string());
}
#endif

bool fileExists(const std::string& utf8Path) {
    std::error_code ec;
#ifdef _WIN32
    // std::filesystem converts narrow paths from the ANSI code page on
    // Windows; convert explicitly so UTF-8 paths (and the /utf-8 build
    // convention) work regardless of the machine locale.
    return fs::exists(fs::path(utf8ToWstring(utf8Path)), ec);
#else
    return fs::exists(fs::path(utf8Path), ec);
#endif
}

const char* getEnvOverrideName(DictKind kind) {
    return kind == DictKind::JIEBA ? "LBUG_FTS_JIEBA_DICT_DIR" : "LBUG_FTS_MECAB_DICT_DIR";
}

// Sub-directory names probed next to the extension, in order. The project
// deployment layout ships the dictionaries as fts_dict / fts_dict-ipadic
// (yongxue/extensions), the source build layout uses dict / dict-ipadic
// (extension/fts/build).
std::vector<std::string> getDictDirCandidates(DictKind kind) {
    if (kind == DictKind::JIEBA) {
        return {"fts_dict", "dict"};
    }
    return {"fts_dict-ipadic", "dict-ipadic"};
}

} // namespace

bool dictDirHasDictionary(const std::string& dir, DictKind kind) {
    if (dir.empty()) {
        return false;
    }
    if (kind == DictKind::JIEBA) {
        // Mirrors the file set cppjieba::Jieba opens on construction; missing
        // any of them aborts with a FATAL from its internal CHECK.
        static const std::vector<std::string> requiredFiles = {"jieba.dict.utf8", "hmm_model.utf8",
            "user.dict.utf8", "idf.utf8", "stop_words.utf8"};
        for (auto& file : requiredFiles) {
            if (!fileExists(dir + "/" + file)) {
                return false;
            }
        }
        return true;
    }
    // MeCab needs the compiled binary dictionary plus the rc file. The rc's
    // own dicdir line is a stale build-time absolute path; the tagger is
    // always invoked with a command-line -d, which takes precedence.
    return fileExists(dir + "/sys.dic") && fileExists(dir + "/mecabrc");
}

std::string getExtensionDir() {
    static const std::string cached = normalizePath(getModuleDir());
    return cached;
}

std::string resolveDictDir(const std::string& requested, DictKind kind) {
    auto envName = getEnvOverrideName(kind);
    std::vector<std::string> tried;
    if (!requested.empty()) {
        if (dictDirHasDictionary(requested, kind)) {
            return normalizePath(requested);
        }
        tried.push_back(std::format("'{}' (stored/config path: not found)", requested));
    }
    const char* envValue = std::getenv(envName);
    if (envValue != nullptr) {
        if (dictDirHasDictionary(envValue, kind)) {
            return normalizePath(envValue);
        }
        tried.push_back(std::format("env {} ('{}'): set but invalid", envName, envValue));
    } else {
        tried.push_back(std::format("env {}: not set", envName));
    }
    auto extensionDir = getExtensionDir();
    if (extensionDir.empty()) {
        tried.push_back("extension module location: unavailable (statically linked build?)");
    } else {
        for (auto& sub : getDictDirCandidates(kind)) {
            auto candidate = extensionDir + "/" + sub;
            if (dictDirHasDictionary(candidate, kind)) {
                return candidate;
            }
            tried.push_back(std::format("'{}': not found", candidate));
        }
    }
#ifdef LBUG_ROOT_DIRECTORY
    auto buildDefault = std::string(LBUG_ROOT_DIRECTORY) + "/extension/fts/build/" +
        (kind == DictKind::JIEBA ? "dict" : "dict-ipadic");
    if (dictDirHasDictionary(buildDefault, kind)) {
        return buildDefault;
    }
    tried.push_back(std::format("'{}' (build-time default): not found", buildDefault));
#endif
    std::string detail;
    for (auto& t : tried) {
        detail += "\n  - " + t;
    }
    throw common::BinderException{std::format(
        "Cannot locate the {} dictionary directory. Tried:{}"
        "\nFix: set the {} environment variable, or place the dictionary next "
        "to the fts extension binary, or recreate the FTS index with an explicit "
        "jieba_dict_dir/mecab_dict_dir.",
        kind == DictKind::JIEBA ? "jieba" : "mecab", detail, envName)};
}

} // namespace fts_extension
} // namespace lbug
