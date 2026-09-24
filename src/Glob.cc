#include "Glob.hh"

Glob::Glob(std::string raw)
  : mHash(std::hash<std::string>()(raw)),
    mRaw(std::move(raw)),
    mRegex(mRaw) {
}

bool Glob::isIgnored(std::string relative_path) const {
  return std::regex_match(relative_path, mRegex);
}
