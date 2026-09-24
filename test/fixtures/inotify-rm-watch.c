#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int inotify_rm_watch(int fd, int wd) {
  static int (*real_inotify_rm_watch)(int, int) = 0;
  const char *mode = getenv("NATIVE_WATCHER_RM_WATCH_MODE");
  if (mode != 0 && strcmp(mode, "einval") == 0) {
    if (real_inotify_rm_watch == 0) {
      real_inotify_rm_watch = dlsym(RTLD_NEXT, "inotify_rm_watch");
    }
    int result = real_inotify_rm_watch(fd, wd);
    if (result == 0) {
      fprintf(stderr, "inotify-rm-watch-shim: EINVAL\n");
      errno = EINVAL;
      return -1;
    }
    return result;
  }

  if (mode != 0 && strcmp(mode, "eio") == 0) {
    fprintf(stderr, "inotify-rm-watch-shim: EIO\n");
    errno = EIO;
    return -1;
  }

  if (real_inotify_rm_watch == 0) {
    real_inotify_rm_watch = dlsym(RTLD_NEXT, "inotify_rm_watch");
  }
  return real_inotify_rm_watch(fd, wd);
}
