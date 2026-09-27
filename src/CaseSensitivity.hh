#ifndef CASE_SENSITIVITY_H
#define CASE_SENSITIVITY_H

#include <string>

enum class DirectoryCaseSensitivity {
  Sensitive,
  Insensitive,
  Unknown,
};

// Returns Unknown when the platform cannot determine the directory's policy
// (including a path which disappeared between an event and this query).
DirectoryCaseSensitivity directoryCaseSensitivity(const std::string &path);

#endif
