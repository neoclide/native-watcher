#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>

struct dirent *readdir(DIR *directory) {
  static struct dirent *(*real_readdir)(DIR *) = 0;
  if (real_readdir == 0) {
    real_readdir = dlsym(RTLD_NEXT, "readdir");
  }

  struct dirent *entry = real_readdir(directory);
  if (entry != 0) {
    entry->d_type = DT_UNKNOWN;
  }
  return entry;
}
