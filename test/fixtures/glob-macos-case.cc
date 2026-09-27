#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "Glob.hh"

int main(int argc, char **argv) {
  assert(argc == 2);
  std::string root(argv[1]);
  std::ofstream(root + "/straße-file").put('x');
  std::ofstream(root + "/é-file").put('x');

  // APFS volumes configured as case-sensitive do not resolve these aliases.
  if (!std::filesystem::exists(root + "/STRASSE-file") ||
      !std::filesystem::exists(root + "/e\xCC\x81-file")) {
    assert(!Glob("straße*").isIgnored("STRASSE-file", root));
    assert(!Glob("é*").isIgnored("e\xCC\x81-file", root));
    puts("SKIP");
    return 0;
  }
  assert(Glob("straße*").isIgnored("STRASSE-file", root));
  assert(Glob("é*").isIgnored("e\xCC\x81-file", root));
  return 0;
}
