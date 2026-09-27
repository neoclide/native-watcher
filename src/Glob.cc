#include "Glob.hh"
#include "CaseSensitivity.hh"

#include <algorithm>
#include <unordered_map>
#include <vector>

#ifdef FS_EVENTS
#include <CoreFoundation/CoreFoundation.h>
#elif defined(WINDOWS)
#include <windows.h>
#include "windows/win_utils.hh"
#endif

namespace {

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
std::string foldedComponent(const std::string &value) {
  CFMutableStringRef string = CFStringCreateMutable(nullptr, 0);
  CFStringRef utf8 = CFStringCreateWithBytes(nullptr,
    reinterpret_cast<const UInt8 *>(value.data()), value.size(),
    kCFStringEncodingUTF8, false);
  if (string == nullptr || utf8 == nullptr) {
    if (string != nullptr) CFRelease(string);
    if (utf8 != nullptr) CFRelease(utf8);
    return value;
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
  if (!converted) return value;
  result.resize(static_cast<size_t>(used));
  return result;
}
#endif

std::vector<std::string> utf8Characters(const std::string &value) {
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
    result.push_back(value.substr(index, length));
    index += length;
  }
  return result;
}

bool componentMatches(const std::string &pattern, const std::string &value,
                      bool ignoreCase) {
  std::string comparedPattern = pattern;
  std::string comparedValue = value;
#ifdef FS_EVENTS
  if (ignoreCase) {
    comparedPattern = foldedComponent(pattern);
    comparedValue = foldedComponent(value);
  }
#endif
  auto patternChars = utf8Characters(comparedPattern);
  auto valueChars = utf8Characters(comparedValue);
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
#ifdef WINDOWS
  std::string pattern = mRaw;
  std::replace(pattern.begin(), pattern.end(), '\\', '/');
  mComponents = components(pattern);
#else
  mComponents = components(mRaw);
#endif
}

bool Glob::isIgnored(const std::string &relativePath, const std::string &root) const {
  if (mRaw.empty()) return false;
  std::string path = relativePath;
#ifdef WINDOWS
  std::replace(path.begin(), path.end(), '\\', '/');
#endif
  const auto &patternParts = mComponents;
  auto pathParts = components(path);
  std::vector<bool> next(pathParts.size() + 1, false);
  next[pathParts.size()] = true;
  std::unordered_map<std::string, DirectoryCaseSensitivity> sensitivity;
  for (size_t patternIndex = patternParts.size(); patternIndex-- > 0;) {
    std::vector<bool> current(pathParts.size() + 1, false);
    for (size_t pathIndex = pathParts.size() + 1; pathIndex-- > 0;) {
      if (patternParts[patternIndex] == "**") {
        current[pathIndex] = next[pathIndex] ||
          (pathIndex < pathParts.size() && current[pathIndex + 1]);
        continue;
      }
      if (pathIndex == pathParts.size() ||
          !next[pathIndex + 1]) {
        continue;
      }
      if (componentMatches(patternParts[patternIndex], pathParts[pathIndex], false)) {
        current[pathIndex] = true;
        continue;
      }
      if (!componentMatches(patternParts[patternIndex], pathParts[pathIndex], true)) {
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
