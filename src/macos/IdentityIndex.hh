#ifndef MACOS_IDENTITY_INDEX_H
#define MACOS_IDENTITY_INDEX_H

#include <sys/stat.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct FileIdentity {
  dev_t device;
  ino_t inode;

  bool operator==(const FileIdentity &other) const {
    return device == other.device && inode == other.inode;
  }
};

struct FileIdentityHash {
  size_t operator()(const FileIdentity &identity) const {
    return std::hash<uint64_t>()(static_cast<uint64_t>(identity.device)) ^
      (std::hash<uint64_t>()(static_cast<uint64_t>(identity.inode)) << 1);
  }
};

struct IndexedPath {
  FileIdentity identity;
  uint64_t mtime;
  nlink_t linkCount;
  bool isDirectory;
};

class IdentityIndex {
public:
  using Entries = std::unordered_map<std::string, IndexedPath>;

  const IndexedPath *find(const std::string &path) const;
  const Entries &entries() const;
  size_t pathCount(const FileIdentity &identity) const;
  void add(const std::string &path, const IndexedPath &entry);
  void remove(const std::string &path);
  void rename(const std::string &oldPath, const std::string &newPath);

private:
  Entries mByPath;
  std::unordered_map<
    FileIdentity,
    std::unordered_set<std::string>,
    FileIdentityHash
  > mByIdentity;

  void removeExact(const std::string &path);
};

std::optional<IndexedPath> readIndexedPath(const std::string &path);
IdentityIndex scanIdentityIndex(
  const std::string &root,
  const std::function<bool(const std::string &)> &isIgnored,
  const std::function<void(const std::string &, const IndexedPath &)> &onEntry
);

#endif
