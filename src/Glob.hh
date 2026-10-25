#ifndef GLOB_H
#define GLOB_H

#include "CaseSensitivity.hh"
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <string>
#include <string_view>
#include <vector>

struct GlobComponent {
  std::string raw;
  std::string folded;
  std::vector<std::string> chars;
  std::vector<std::string> foldedChars;
  bool isDoubleStar = false;
  bool isSingleStar = false;
  bool hasWildcard = false;
  bool isAscii = true;
  bool isFoldedAscii = true;
};

struct Glob {
  std::size_t mHash;
  std::string mRaw;
  std::vector<std::string> mComponents;
  std::vector<GlobComponent> mParts;
  uint64_t mDoubleStarMask = 0;
  uint64_t mActiveMask = 0;
  uint64_t mInitialState = 0;
  uint64_t mAcceptBit = 0;
  bool mUseBitmask = false;

  Glob(std::string raw);

  bool operator==(const Glob &other) const {
    return mHash == other.mHash && mRaw == other.mRaw;
  }

  bool isIgnored(const std::string &relativePath, const std::string &root) const;
  bool isPathOrAncestorIgnored(std::string_view relativePath, const std::string &root) const;
  uint64_t stepBitmask(
    uint64_t state,
    std::string_view component,
    std::string_view root,
    std::string_view relativeParent,
    std::optional<DirectoryCaseSensitivity> &cachedSensitivity
  ) const;
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
