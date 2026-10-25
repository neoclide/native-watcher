#include "Glob.hh"
#include "CaseSensitivity.hh"

#include <algorithm>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef FS_EVENTS
#include <CoreFoundation/CoreFoundation.h>
#elif defined(WINDOWS)
#include <windows.h>
#include "windows/win_utils.hh"
#endif

namespace {

size_t trailingZeros(uint64_t value) {
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanForward64(&index, value);
  return static_cast<size_t>(index);
#else
  return static_cast<size_t>(__builtin_ctzll(value));
#endif
}

bool isAsciiString(std::string_view value) {
  for (unsigned char ch : value) {
    if (ch >= 0x80) return false;
  }
  return true;
}

std::string toAsciiLower(std::string_view value) {
  std::string result(value.size(), '\0');
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(value[i]);
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    result[i] = static_cast<char>(ch);
  }
  return result;
}

bool equalsAsciiLower(std::string_view lowerPattern, std::string_view value) {
  if (lowerPattern.size() != value.size()) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(value[i]);
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (static_cast<unsigned char>(lowerPattern[i]) != ch) return false;
  }
  return true;
}

bool matchWildcardAscii(
  std::string_view pattern,
  std::string_view value,
  bool ignoreCase
) {
  size_t p = 0;
  size_t v = 0;
  size_t star = std::string_view::npos;
  size_t match = 0;
  while (v < value.size()) {
    if (p < pattern.size() && pattern[p] != '*') {
      unsigned char pc = static_cast<unsigned char>(pattern[p]);
      unsigned char vc = static_cast<unsigned char>(value[v]);
      if (ignoreCase) {
        if (pc >= 'A' && pc <= 'Z') pc += 'a' - 'A';
        if (vc >= 'A' && vc <= 'Z') vc += 'a' - 'A';
      }
      if (pc == vc) {
        p++;
        v++;
        continue;
      }
    }
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      match = v;
      continue;
    }
    if (star != std::string_view::npos) {
      p = star + 1;
      v = ++match;
      continue;
    }
    return false;
  }
  while (p < pattern.size() && pattern[p] == '*') {
    p++;
  }
  return p == pattern.size();
}

std::string collapseStars(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (char ch : value) {
    if (ch == '*' && !result.empty() && result.back() == '*') {
      continue;
    }
    result.push_back(ch);
  }
  return result;
}

std::vector<std::string> components(const std::string &path) {
  std::vector<std::string> result;
  if (path.empty()) return result;
  size_t start = 0;
  for (size_t index = 0; index <= path.size(); index++) {
    if (index == path.size() || path[index] == '/') {
      result.push_back(path.substr(start, index - start));
      start = index + 1;
    }
  }
  return result;
}

#ifndef FS_EVENTS
bool equalIgnoringCase(const std::string &left, const std::string &right) {
#ifdef WINDOWS
  if (isAsciiString(left) && isAsciiString(right)) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); index++) {
      unsigned char a = left[index];
      unsigned char b = right[index];
      if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
      if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
      if (a != b) return false;
    }
    return true;
  }
  auto a = utf8ToUtf16(left);
  auto b = utf8ToUtf16(right);
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
    b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
  if (left.size() != right.size()) return false;
  for (size_t index = 0; index < left.size(); index++) {
    unsigned char a = left[index];
    unsigned char b = right[index];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
  }
  return true;
#endif
}
#endif

#ifdef FS_EVENTS
std::string foldedComponent(std::string_view value) {
  if (isAsciiString(value)) {
    return toAsciiLower(value);
  }
  CFMutableStringRef string = CFStringCreateMutable(nullptr, 0);
  CFStringRef utf8 = CFStringCreateWithBytes(nullptr,
    reinterpret_cast<const UInt8 *>(value.data()), value.size(),
    kCFStringEncodingUTF8, false);
  if (string == nullptr || utf8 == nullptr) {
    if (string != nullptr) CFRelease(string);
    if (utf8 != nullptr) CFRelease(utf8);
    return std::string(value);
  }
  CFStringAppend(string, utf8);
  CFRelease(utf8);
  CFStringFold(string, kCFCompareCaseInsensitive, nullptr);
  CFStringNormalize(string, kCFStringNormalizationFormC);
  CFIndex length = CFStringGetLength(string);
  CFIndex maximum = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
  std::string result(static_cast<size_t>(maximum), '\0');
  CFIndex used = 0;
  bool converted = CFStringGetBytes(string, CFRangeMake(0, length),
    kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8 *>(result.data()),
    maximum, &used);
  CFRelease(string);
  if (!converted) return std::string(value);
  result.resize(static_cast<size_t>(used));
  return result;
}
#endif

std::vector<std::string> utf8Characters(std::string_view value) {
  std::vector<std::string> result;
  for (size_t index = 0; index < value.size();) {
    unsigned char first = value[index];
    size_t length = 1;
    auto continuation = [&](size_t offset) {
      return index + offset < value.size() &&
        (static_cast<unsigned char>(value[index + offset]) & 0xc0) == 0x80;
    };
    if (first >= 0xc2 && first <= 0xdf && continuation(1)) {
      length = 2;
    } else if (first >= 0xe0 && first <= 0xef && continuation(1) &&
        continuation(2) && !(first == 0xe0 &&
        static_cast<unsigned char>(value[index + 1]) < 0xa0) &&
        !(first == 0xed && static_cast<unsigned char>(value[index + 1]) >= 0xa0)) {
      length = 3;
    } else if (first >= 0xf0 && first <= 0xf4 && continuation(1) &&
        continuation(2) && continuation(3) && !(first == 0xf0 &&
        static_cast<unsigned char>(value[index + 1]) < 0x90) &&
        !(first == 0xf4 && static_cast<unsigned char>(value[index + 1]) >= 0x90)) {
      length = 4;
    }
    result.emplace_back(value.substr(index, length));
    index += length;
  }
  return result;
}

bool matchWildcardUtf8(
  const std::vector<std::string> &patternChars,
  std::string_view value,
  bool ignoreCase
) {
  auto valueChars = utf8Characters(value);
  std::vector<bool> next(valueChars.size() + 1, false);
  next[valueChars.size()] = true;
  for (size_t patternIndex = patternChars.size(); patternIndex-- > 0;) {
    std::vector<bool> current(valueChars.size() + 1, false);
    for (size_t valueIndex = valueChars.size() + 1; valueIndex-- > 0;) {
      if (patternChars[patternIndex] == "*") {
        current[valueIndex] = next[valueIndex] ||
          (valueIndex < valueChars.size() && current[valueIndex + 1]);
      } else if (valueIndex < valueChars.size() &&
#ifdef FS_EVENTS
          patternChars[patternIndex] == valueChars[valueIndex]) {
#else
          (ignoreCase ? equalIgnoringCase(patternChars[patternIndex], valueChars[valueIndex])
                      : patternChars[patternIndex] == valueChars[valueIndex])) {
#endif
        current[valueIndex] = next[valueIndex + 1];
      }
    }
    next = std::move(current);
  }
  return next[0];
}

bool componentMatchesExact(const GlobComponent &part, std::string_view value) {
  if (part.isSingleStar) return true;
  if (!part.hasWildcard) return part.raw == value;
  if (part.isAscii && isAsciiString(value)) {
    return matchWildcardAscii(part.raw, value, false);
  }
  return matchWildcardUtf8(part.chars, value, false);
}

bool componentMatchesFolded(const GlobComponent &part, std::string_view value) {
  if (part.isSingleStar) return true;
  if (part.isFoldedAscii && isAsciiString(value)) {
    if (!part.hasWildcard) {
      return equalsAsciiLower(part.folded, value);
    }
    return matchWildcardAscii(part.folded, value, true);
  }
#ifdef FS_EVENTS
  std::string foldedVal = foldedComponent(value);
  if (!part.hasWildcard) {
    return part.folded == foldedVal;
  }
  if (part.isFoldedAscii && isAsciiString(foldedVal)) {
    return matchWildcardAscii(part.folded, foldedVal, false);
  }
  return matchWildcardUtf8(part.foldedChars, foldedVal, false);
#else
  if (!part.hasWildcard) {
    return equalIgnoringCase(part.raw, std::string(value));
  }
  return matchWildcardUtf8(part.chars, value, true);
#endif
}

std::string buildParentDirectory(
  std::string_view root,
  std::string_view relativeParent
) {
  if (relativeParent.empty()) {
    return std::string(root);
  }
  std::string result;
  result.reserve(root.size() + 1 + relativeParent.size());
  result.append(root);
#ifdef WINDOWS
  result.push_back('\\');
  for (char ch : relativeParent) {
    result.push_back(ch == '/' ? '\\' : ch);
  }
#else
  result.push_back('/');
  result.append(relativeParent);
#endif
  return result;
}

std::string parentDirectory(const std::string &root,
                            const std::vector<std::string> &path,
                            size_t component) {
  std::string result = root;
  for (size_t index = 0; index < component; index++) {
#ifdef WINDOWS
    result += '\\';
#else
    result += '/';
#endif
    result += path[index];
  }
  return result;
}

}

Glob::Glob(std::string raw)
  : mHash(std::hash<std::string>()(raw)),
    mRaw(std::move(raw)) {
  if (mRaw.empty()) return;
#ifdef WINDOWS
  std::string pattern = mRaw;
  std::replace(pattern.begin(), pattern.end(), '\\', '/');
  mComponents = components(pattern);
#else
  mComponents = components(mRaw);
#endif

  mParts.reserve(mComponents.size());
  for (const auto &part : mComponents) {
    if (part == "**" && !mParts.empty() && mParts.back().isDoubleStar) {
      continue;
    }
    GlobComponent compiled;
    compiled.isDoubleStar = (part == "**");
    compiled.hasWildcard = (part.find('*') != std::string::npos);
    if (compiled.hasWildcard && !compiled.isDoubleStar) {
      compiled.raw = collapseStars(part);
      compiled.isSingleStar = (compiled.raw == "*");
    } else {
      compiled.raw = part;
    }
    compiled.isAscii = isAsciiString(compiled.raw);
#ifdef FS_EVENTS
    compiled.folded = foldedComponent(compiled.raw);
#else
    compiled.folded = compiled.isAscii ? toAsciiLower(compiled.raw) : compiled.raw;
#endif
    compiled.isFoldedAscii = isAsciiString(compiled.folded);
    if (compiled.hasWildcard && !compiled.isDoubleStar && !compiled.isSingleStar) {
      compiled.chars = utf8Characters(compiled.raw);
      compiled.foldedChars = utf8Characters(compiled.folded);
    }
    mParts.push_back(std::move(compiled));
  }

  if (!mParts.empty() && mParts.size() < 63) {
    mUseBitmask = true;
    for (size_t i = 0; i < mParts.size(); ++i) {
      if (mParts[i].isDoubleStar) {
        mDoubleStarMask |= (uint64_t(1) << i);
      } else {
        mActiveMask |= (uint64_t(1) << i);
      }
    }
    mInitialState = uint64_t(1) | ((uint64_t(1) & mDoubleStarMask) << 1);
    mAcceptBit = uint64_t(1) << mParts.size();
  }
}

uint64_t Glob::stepBitmask(
  uint64_t state,
  std::string_view component,
  std::string_view root,
  std::string_view relativeParent,
  std::optional<DirectoryCaseSensitivity> &cachedSensitivity
) const {
  uint64_t next = state & mDoubleStarMask;
  uint64_t candidates = state & mActiveMask;
  while (candidates != 0) {
    size_t p = trailingZeros(candidates);
    candidates &= candidates - 1;
    const auto &part = mParts[p];
    if (componentMatchesExact(part, component)) {
      next |= (uint64_t(1) << (p + 1));
      continue;
    }
    if (!componentMatchesFolded(part, component)) {
      continue;
    }
    if (!cachedSensitivity.has_value()) {
      cachedSensitivity = directoryCaseSensitivity(
        buildParentDirectory(root, relativeParent)
      );
    }
    if (*cachedSensitivity == DirectoryCaseSensitivity::Insensitive) {
      next |= (uint64_t(1) << (p + 1));
    }
  }
  return next | ((next & mDoubleStarMask) << 1);
}

bool Glob::isPathOrAncestorIgnored(
  std::string_view relativePath,
  const std::string &root
) const {
  if (mRaw.empty() || relativePath.empty()) return false;
#ifdef WINDOWS
  std::string normalized(relativePath);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  std::string_view path(normalized);
#else
  std::string_view path = relativePath;
#endif
  if (mUseBitmask) {
    uint64_t state = mInitialState;
    size_t start = 0;
    for (size_t index = 0; index <= path.size(); index++) {
      if (index == path.size() || path[index] == '/') {
        std::string_view component = path.substr(start, index - start);
        std::string_view relParent = start == 0
          ? std::string_view()
          : path.substr(0, start - 1);
        std::optional<DirectoryCaseSensitivity> sensitivity;
        state = stepBitmask(state, component, root, relParent, sensitivity);
        if ((state & mAcceptBit) != 0) return true;
        if (state == 0) return false;
        start = index + 1;
      }
    }
    return false;
  }

  if (isIgnored(std::string(relativePath), root)) return true;
  for (size_t end = path.find('/'); end != std::string_view::npos;
       end = path.find('/', end + 1)) {
    if (isIgnored(std::string(path.substr(0, end)), root)) return true;
  }
  return false;
}

bool Glob::isIgnored(const std::string &relativePath, const std::string &root) const {
  if (mRaw.empty()) return false;
#ifdef WINDOWS
  std::string normalized = relativePath;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  const std::string &path = normalized;
#else
  const std::string &path = relativePath;
#endif
  if (mUseBitmask) {
    if (path.empty()) {
      return (mInitialState & mAcceptBit) != 0;
    }
    uint64_t state = mInitialState;
    size_t start = 0;
    for (size_t index = 0; index <= path.size(); index++) {
      if (index == path.size() || path[index] == '/') {
        std::string_view component(path.data() + start, index - start);
        std::string_view relParent = start == 0
          ? std::string_view()
          : std::string_view(path.data(), start - 1);
        std::optional<DirectoryCaseSensitivity> sensitivity;
        state = stepBitmask(state, component, root, relParent, sensitivity);
        if (state == 0) return false;
        start = index + 1;
      }
    }
    return (state & mAcceptBit) != 0;
  }

  const auto &patternParts = mParts;
  auto pathParts = components(path);
  std::vector<bool> next(pathParts.size() + 1, false);
  next[pathParts.size()] = true;
  std::unordered_map<std::string, DirectoryCaseSensitivity> sensitivity;
  for (size_t patternIndex = patternParts.size(); patternIndex-- > 0;) {
    std::vector<bool> current(pathParts.size() + 1, false);
    for (size_t pathIndex = pathParts.size() + 1; pathIndex-- > 0;) {
      if (patternParts[patternIndex].isDoubleStar) {
        current[pathIndex] = next[pathIndex] ||
          (pathIndex < pathParts.size() && current[pathIndex + 1]);
        continue;
      }
      if (pathIndex == pathParts.size() ||
          !next[pathIndex + 1]) {
        continue;
      }
      if (componentMatchesExact(patternParts[patternIndex], pathParts[pathIndex])) {
        current[pathIndex] = true;
        continue;
      }
      if (!componentMatchesFolded(patternParts[patternIndex], pathParts[pathIndex])) {
        continue;
      }
      std::string parent = parentDirectory(root, pathParts, pathIndex);
      auto found = sensitivity.find(parent);
      DirectoryCaseSensitivity sensitivityValue;
      if (found == sensitivity.end()) {
        sensitivityValue = directoryCaseSensitivity(parent);
        sensitivity.emplace(parent, sensitivityValue);
      } else {
        sensitivityValue = found->second;
      }
      if (sensitivityValue == DirectoryCaseSensitivity::Insensitive) {
        current[pathIndex] = true;
      }
    }
    next = std::move(current);
  }
  return next[0];
}
