#ifndef DIR_TREE_H
#define DIR_TREE_H

#include <string>
#include <unordered_map>
#include <memory>
#include <optional>
#include "Event.hh"

#ifdef _WIN32
#define DIR_SEP "\\"
#else
#define DIR_SEP "/"
#endif

struct DirEntry {
  std::string path;
  uint64_t mtime = 0;
  bool isDir = false;
  mutable void *state = nullptr;

  DirEntry(std::string p, uint64_t t, bool d);
  DirEntry(FILE *f);
  void write(FILE *f) const;
  bool operator==(const DirEntry &other) const {
    return path == other.path;
  }
};

class DirTree {
public:
  static std::shared_ptr<DirTree> getCached(std::string root);
  DirTree(std::string root) : root(root), isComplete(false) {}
  DirTree(std::string root, FILE *f);
  void add(std::string path, uint64_t mtime, bool isDir);
  std::optional<DirEntry> find(std::string path);
  bool update(std::string path, uint64_t mtime);
  std::vector<DirEntry> extract(std::string path);
  void restore(std::vector<DirEntry> entries, std::string oldPath, std::string newPath);
  void rename(std::string oldPath, std::string newPath);
  void remove(std::string path);
  void write(FILE *f);
  void getChanges(DirTree *snapshot, EventList &events);

  std::mutex mMutex;
  std::string root;
  bool isComplete;
  std::unordered_map<std::string, DirEntry> entries;

private:
  DirEntry *_find(std::string path);
};

#endif
