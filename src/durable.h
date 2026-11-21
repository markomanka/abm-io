// SPDX-License-Identifier: Apache-2.0
// Replacing a file so that it appears whole or not at all, and stays after a
// crash: the content is flushed to disk, renamed over the final name, and the
// directory flushed so that the rename itself is kept. Rename is atomic on
// POSIX file systems, which is what entry 10 of docs/RATIONALE.md rests on;
// other storage needs another mechanism (docs/STORAGE-BACKENDS.md).
//
// Internal to the library.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "abmio/status.h"

namespace abmio {
namespace durable {

/// Flushes `temporary` to disk, renames it to `final` and flushes the
/// directory. `why` receives a sentence on failure, here and below.
Status Replace(const std::string& temporary, const std::string& final_path,
               std::string* why);

/// Writes `content` to final_path + ".tmp" and replaces final_path with it.
Status WriteFile(const std::string& final_path, const std::string& content,
                 std::string* why);

/// Reads at most `limit` bytes; a longer file is refused. `missing` is set,
/// with kOk returned, when there is no such file.
Status ReadFile(const std::string& path, std::size_t limit, std::string* out,
                bool* missing, std::string* why);

/// The size of a regular file; `missing` as above.
Status SizeOf(const std::string& path, uint64_t* bytes, bool* missing,
              std::string* why);

/// Creates a directory and any on the way, as `mkdir -p` does.
Status MakeDirectories(const std::string& path, std::string* why);

}  // namespace durable
}  // namespace abmio
