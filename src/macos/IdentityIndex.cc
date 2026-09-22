#include "IdentityIndex.hh"

#include <fcntl.h>
#include <fts.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <functional>
#include <stdexcept>

#define CONVERT_TIME(ts) ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec)

namespace {

bool hasPathPrefix(
  const std::string &path,
  const std::string &prefix
) {
  return path.size() > prefix.size() &&
    path.compare(0, prefix.size(), prefix) == 0 &&
    path[prefix.size()] == '/';
}

bool pathHasExactCase(const std::string &path) {
  int fd = open(path.c_str(), O_RDONLY | O_SYMLINK);
  if (fd == -1) return false;

  char canonicalPath[PATH_MAX];
  bool result = fcntl(fd, F_GETPATH, canonicalPath) != -1 &&
    path == canonicalPath;
  close(fd);
  return result;
}

IndexedPath fromStat(const struct stat &file) {
  return IndexedPath {
    FileIdentity {file.st_dev, file.st_ino},
    CONVERT_TIME(file.st_mtimespec),
    file.st_nlink,
    S_ISDIR(file.st_mode)
  };
}

} // namespace

const IndexedPath *IdentityIndex::find(const std::string &path) const {
  auto found = mByPath.find(path);
  return found == mByPath.end() ? nullptr : &found->second;
}

const IdentityIndex::Entries &IdentityIndex::entries() const {
  return mByPath;
}

size_t IdentityIndex::pathCount(const FileIdentity &identity) const {
  auto found = mByIdentity.find(identity);
  return found == mByIdentity.end() ? 0 : found->second.size();
}

void IdentityIndex::add(
  const std::string &path,
  const IndexedPath &entry
) {
  removeExact(path);
  mByPath.emplace(path, entry);
  mByIdentity[entry.identity].insert(path);
}

void IdentityIndex::removeExact(const std::string &path) {
  auto found = mByPath.find(path);
  if (found == mByPath.end()) return;

  auto paths = mByIdentity.find(found->second.identity);
  if (paths != mByIdentity.end()) {
    paths->second.erase(path);
    if (paths->second.empty()) mByIdentity.erase(paths);
  }
  mByPath.erase(found);
}

void IdentityIndex::remove(const std::string &path) {
  auto exact = mByPath.find(path);
  if (exact != mByPath.end() && !exact->second.isDirectory) {
    removeExact(path);
    return;
  }

  std::vector<std::string> removed;
  for (const auto &entry : mByPath) {
    if (entry.first == path || hasPathPrefix(entry.first, path)) {
      removed.push_back(entry.first);
    }
  }
  for (const auto &removedPath : removed) removeExact(removedPath);
}

void IdentityIndex::rename(
  const std::string &oldPath,
  const std::string &newPath
) {
  auto exact = mByPath.find(oldPath);
  if (exact != mByPath.end() && !exact->second.isDirectory) {
    IndexedPath entry = exact->second;
    removeExact(oldPath);
    add(newPath, entry);
    return;
  }

  std::vector<std::pair<std::string, IndexedPath>> moved;
  for (const auto &entry : mByPath) {
    if (entry.first == oldPath || hasPathPrefix(entry.first, oldPath)) {
      moved.emplace_back(
        newPath + entry.first.substr(oldPath.size()),
        entry.second
      );
    }
  }

  remove(oldPath);
  for (const auto &entry : moved) add(entry.first, entry.second);
}

std::optional<IndexedPath> readIndexedPath(const std::string &path) {
  struct stat file;
  if (lstat(path.c_str(), &file) != 0 || !pathHasExactCase(path)) {
    return std::nullopt;
  }
  return fromStat(file);
}

IdentityIndex scanIdentityIndex(
  const std::string &root,
  const std::function<bool(const std::string &)> &isIgnored,
  const std::function<void(const std::string &, const IndexedPath &)> &onEntry
) {
  IdentityIndex result;
  char *paths[2] {const_cast<char *>(root.c_str()), nullptr};
  FTS *fts = fts_open(paths, FTS_NOCHDIR | FTS_PHYSICAL, nullptr);
  if (fts == nullptr) throw std::runtime_error(strerror(errno));

  FTSENT *node;
  while ((node = fts_read(fts)) != nullptr) {
    if (node->fts_info == FTS_DP) continue;
    if (node->fts_errno != 0) {
      int error = node->fts_errno;
      fts_close(fts);
      throw std::runtime_error(strerror(error));
    }

    std::string path(node->fts_path);
    if (path != root && isIgnored(path)) {
      if (node->fts_info == FTS_D) fts_set(fts, node, FTS_SKIP);
      continue;
    }

    if (node->fts_statp == nullptr) continue;
    IndexedPath entry = fromStat(*node->fts_statp);
    result.add(path, entry);
    onEntry(path, entry);
  }

  fts_close(fts);
  return result;
}
