#pragma once

#include <string>

namespace lbug {
namespace fts_extension {

enum class DictKind { JIEBA, MECAB };

// Resolves the dictionary directory a tokenizer should use, following the
// fallback chain:
//   1. `requested` — the path stored in / passed through the FTS config, if
//      its dictionary marker files still exist;
//   2. the LBUG_FTS_JIEBA_DICT_DIR / LBUG_FTS_MECAB_DICT_DIR env variable;
//   3. the directory the fts extension binary was loaded from (probing the
//      sub-directory names used by the project deployment layout as well as
//      the source-tree build layout);
//   4. the build-time default under LBUG_ROOT_DIRECTORY (dev machines that
//      ran the dict copy step).
//
// The chain is what makes indexes portable across machines: a catalog that
// serialized a machine-specific absolute path (see TokenizerInfo and
// CreateFTSConfig) no longer fails at query time when that path is gone — it
// falls back to a dictionary that ships with the extension. Throws a
// BinderException listing every candidate when nothing resolves.
std::string resolveDictDir(const std::string& requested, DictKind kind);

// Directory containing the loaded fts extension binary ("" when the module
// location cannot be determined, e.g. statically linked builds).
std::string getExtensionDir();

// Whether `dir` contains the marker files the tokenizer of `kind` needs.
bool dictDirHasDictionary(const std::string& dir, DictKind kind);

} // namespace fts_extension
} // namespace lbug
