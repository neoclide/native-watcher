#include "Glob.hh"

Glob::Glob(std::string raw) {
  mRaw = raw;
  mHash = std::hash<std::string>()(raw);
  mRegex = std::regex(raw);
}

bool Glob::isIgnored(std::string relative_path) const {
  return std::regex_match(relative_path, mRegex);
}
