#include <cassert>
#include <fcntl.h>
#include <unistd.h>
#include "macos/FSEventsBackend.cc"

// The fixture exercises event classification without starting a native stream.
bool Watcher::isIgnored(std::string) { return false; }

int main(int argc, char **argv) {
  assert(argc == 2);
  std::string path = std::string(argv[1]) + "/file.txt";
  int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
  assert(fd >= 0);
  assert(write(fd, "one", 3) == 3);
  close(fd);

  State state;
  state.tree = std::make_shared<DirTree>(argv[1]);
  EventList events;
  addCreatedPath(nullptr, &state, events, path);
  auto created = events.drain();
  assert(created.events.size() == 1 && created.events[0].isCreated);

  // A later Created notification for an already delivered file must not
  // classify the next write as another creation.
  auto before = readIndexedPath(path);
  assert(before.has_value());
  timespec times[2] = {{0, UTIME_OMIT}, {
    static_cast<time_t>(before->mtime / 1000000000 + 1), 123456789
  }};
  assert(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
  addCreatedPath(nullptr, &state, events, path);
  auto updated = events.drain();
  assert(updated.events.size() == 1);
  assert(!updated.events[0].isCreated && !updated.events[0].isDeleted);
  assert(state.tree->find(path)->mtime == readIndexedPath(path)->mtime);

  addCreatedPath(nullptr, &state, events, path);
  assert(events.drain().events.empty());
}
