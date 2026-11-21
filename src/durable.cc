// SPDX-License-Identifier: Apache-2.0
#include "durable.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace abmio {
namespace durable {
namespace {

Status Fail(std::string* why, const std::string& message) {
  if (why != nullptr) {
    *why = message + " (" + std::strerror(errno) + ")";
  }
  return Status::kIo;
}

std::string DirectoryOf(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  return slash == 0 ? "/" : path.substr(0, slash);
}

Status Sync(const std::string& path, int flags, std::string* why) {
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    return Fail(why, "cannot open " + path + " to flush it");
  }
  const int r = ::fsync(fd);
  const int saved = errno;
  ::close(fd);
  errno = saved;
  return r == 0 ? Status::kOk : Fail(why, "cannot flush " + path);
}

}  // namespace

Status Replace(const std::string& temporary, const std::string& final_path,
               std::string* why) {
  Status s = Sync(temporary, O_RDONLY, why);
  if (s != Status::kOk) {
    return s;
  }
  if (std::rename(temporary.c_str(), final_path.c_str()) != 0) {
    return Fail(why, "cannot rename " + temporary + " to " + final_path);
  }
  return Sync(DirectoryOf(final_path), O_RDONLY | O_DIRECTORY, why);
}

Status WriteFile(const std::string& final_path, const std::string& content,
                 std::string* why) {
  const std::string temporary = final_path + ".tmp";
  const int fd =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return Fail(why, "cannot create " + temporary);
  }
  std::size_t done = 0;
  while (done < content.size()) {
    const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved = errno;
      ::close(fd);
      errno = saved;
      return Fail(why, "cannot write " + temporary);
    }
    done += static_cast<std::size_t>(n);
  }
  if (::close(fd) != 0) {
    return Fail(why, "cannot write " + temporary);
  }
  return Replace(temporary, final_path, why);
}

Status ReadFile(const std::string& path, std::size_t limit, std::string* out,
                bool* missing, std::string* why) {
  out->clear();
  *missing = false;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      *missing = true;
      return Status::kOk;
    }
    return Fail(why, "cannot open " + path);
  }
  char buffer[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buffer, sizeof buffer);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved = errno;
      ::close(fd);
      errno = saved;
      return Fail(why, "cannot read " + path);
    }
    if (n == 0) {
      break;
    }
    if (out->size() + static_cast<std::size_t>(n) > limit) {
      ::close(fd);
      if (why != nullptr) {
        *why = path + " is larger than abm-io accepts";
      }
      return Status::kCorrupt;
    }
    out->append(buffer, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return Status::kOk;
}

Status SizeOf(const std::string& path, uint64_t* bytes, bool* missing,
              std::string* why) {
  *missing = false;
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) {
      *missing = true;
      return Status::kOk;
    }
    return Fail(why, "cannot look at " + path);
  }
  if (!S_ISREG(st.st_mode)) {
    if (why != nullptr) {
      *why = path + " is not a regular file";
    }
    return Status::kCorrupt;
  }
  *bytes = static_cast<uint64_t>(st.st_size);
  return Status::kOk;
}

Status MakeDirectories(const std::string& path, std::string* why) {
  std::string partial;
  std::size_t i = 0;
  while (i <= path.size()) {
    const std::size_t slash = path.find('/', i);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    partial = path.substr(0, end);
    if (!partial.empty() && ::mkdir(partial.c_str(), 0755) != 0 &&
        errno != EEXIST) {
      return Fail(why, "cannot create the directory " + partial);
    }
    if (slash == std::string::npos) {
      break;
    }
    i = slash + 1;
  }
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return Fail(why, path + " is not a directory");
  }
  return Status::kOk;
}

}  // namespace durable
}  // namespace abmio
