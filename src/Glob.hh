#ifndef GLOB_H
#define GLOB_H

#include <unordered_set>
#include <string>
#include <vector>

struct Glob {
  std::size_t mHash;
  std::string mRaw;
  std::vector<std::string> mComponents;

  Glob(std::string raw);

  bool operator==(const Glob &other) const {
    return mHash == other.mHash && mRaw == other.mRaw;
  }

  bool isIgnored(const std::string &relativePath, const std::string &root) const;
};

namespace std
{
  template <>
  struct hash<Glob>
  {
    size_t operator()(const Glob& g) const {
      return g.mHash;
    }
  };
}

#endif
