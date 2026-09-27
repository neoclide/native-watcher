#include <cassert>
#include "CaseSensitivity.hh"
#include "Glob.hh"

DirectoryCaseSensitivity sensitivity = DirectoryCaseSensitivity::Sensitive;

DirectoryCaseSensitivity directoryCaseSensitivity(const std::string &) {
  return sensitivity;
}

void matches(const char *pattern, const char *path) {
  assert(Glob(pattern).isIgnored(path, "/root"));
}

void misses(const char *pattern, const char *path) {
  assert(!Glob(pattern).isIgnored(path, "/root"));
}

int main() {
  misses("", "");
  misses("/foo*", "foo");
  misses("foo//bar*", "foo/bar");
  misses("foo*/", "foo");
  matches("**/*.log", "root.log");
  matches("**/*.log", "nested/root.log");
  matches("src/**/cache", "src/cache");
  matches("src/**/cache", "src/a/b/cache");
  misses("src/**/cache", "cache");
  matches("cache/**", "cache");
  matches("cache/**", "cache/a/b");
  matches("**/node_modules", "node_modules");
  matches("**/node_modules", "a/node_modules");
  matches("*.log", ".hidden.log");
  misses("*.log", "nested/file.log");
  matches("a**b", "a-middle-b");
  matches("***", "anything");
  matches("literal/[x]{a,b}?!#+^$", "literal/[x]{a,b}?!#+^$");
  misses("literal/[x]{a,b}?!#+^$", "literal/xa");
  matches("資料/*", "資料/é.txt");
  assert(Glob("*a").isIgnored(std::string("\xc0") + "a", "/root"));
  assert(Glob("*.log").isIgnored(std::string("\xc0") + ".log", "/root"));
  misses("one/*", "one/two/three");
  sensitivity = DirectoryCaseSensitivity::Insensitive;
  matches("CACHE", "cache");
  sensitivity = DirectoryCaseSensitivity::Unknown;
  misses("CACHE", "cache");
  return 0;
}
