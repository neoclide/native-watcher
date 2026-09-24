#include <cassert>
#include <cstdio>
#include "DirTree.hh"

int main() {
  FILE *file = tmpfile();
  assert(file != nullptr);

  DirTree original("root");
  original.add("123_data", 42, false);
  original.add("plain", 43, true);
  original.add("9 dir/sub.txt", 44, false);
  original.add("7:report.txt", 45, false);
  original.write(file);
  rewind(file);

  DirTree restored("root", file);
  assert(restored.entries.size() == 4);
  assert(restored.find("123_data") != nullptr);
  assert(restored.find("123_data")->mtime == 42);
  assert(!restored.find("123_data")->isDir);
  assert(restored.find("plain") != nullptr);
  assert(restored.find("plain")->mtime == 43);
  assert(restored.find("plain")->isDir);
  assert(restored.find("9 dir/sub.txt") != nullptr);
  assert(restored.find("9 dir/sub.txt")->mtime == 44);
  assert(!restored.find("9 dir/sub.txt")->isDir);
  assert(restored.find("7:report.txt") != nullptr);
  assert(restored.find("7:report.txt")->mtime == 45);
  assert(!restored.find("7:report.txt")->isDir);
  fclose(file);
}
