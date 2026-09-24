#include <cassert>
#include <filesystem>
#include <fstream>
#include <utime.h>
#include "macos/FSEventsBackend.cc"

// This component fixture supplies merged flags directly; it does not claim
// that macOS always emits every tested flag combination.
Watcher::Watcher(
  std::string dir,
  std::unordered_set<std::string> ignorePaths,
  std::unordered_set<Glob> ignoreGlobs
) : mDir(std::move(dir)),
    mIgnorePaths(std::move(ignorePaths)),
    mIgnoreGlobs(std::move(ignoreGlobs)) {}

bool Watcher::isIgnored(std::string) { return false; }
void Watcher::notify() {}

void writeFile(const std::string &path, const char *contents) {
  std::ofstream file(path);
  assert(file.good());
  file << contents;
}

void assertEvent(
  const EventBatch &batch,
  const std::string &path,
  bool created,
  bool deleted,
  EntryKind kind
) {
  for (const auto &event : batch.events) {
    if (event.path == path && event.isCreated == created &&
        event.isDeleted == deleted && event.kind == kind) {
      return;
    }
  }
  assert(false);
}

std::shared_ptr<State> indexDirectory(const std::string &root) {
  auto state = std::make_shared<State>();
  state->tree = std::make_shared<DirTree>(root);
  state->identities = scanIdentityIndex(
    root,
    [](const std::string &) { return false; },
    [&state](const std::string &path, const IndexedPath &entry) {
      state->tree->add(path, entry.mtime, entry.isDirectory);
    }
  );
  return state;
}

void createDirectoryTarget(
  const std::string &target,
  const std::string &child
) {
  std::filesystem::remove_all(target);
  std::filesystem::create_directory(target);
  writeFile(child, "child");
}

void runTypeReplacement(
  WatcherRef watcher,
  const std::string &root,
  FSEventStreamEventFlags flags
) {
  std::string target = root + "/target";
  std::string child = target + "/child.txt";
  createDirectoryTarget(target, child);
  auto state = indexDirectory(root);

  std::filesystem::remove_all(target);
  writeFile(target, "file");
  processEvents(
    watcher,
    state,
    {PendingEvent {
      target, flags | kFSEventStreamEventFlagItemIsFile, 1
    }}
  );
  auto removed = watcher->mEvents.drain();
  assertEvent(removed, target, false, true, EntryKind::Directory);
  assertEvent(removed, target, true, false, EntryKind::File);
  assertEvent(removed, child, false, true, EntryKind::File);
  auto targetIndex = state->identities.find(target);
  assert(targetIndex != nullptr && !targetIndex->isDirectory);
  assert(state->identities.find(child) == nullptr);
  auto targetTree = state->tree->find(target);
  assert(targetTree.has_value() && !targetTree->isDir);
  assert(!state->tree->find(child).has_value());

  std::filesystem::remove(target);
  std::filesystem::create_directory(target);
  writeFile(child, "child again");
  processEvents(
    watcher,
    state,
    {PendingEvent {
      target, flags | kFSEventStreamEventFlagItemIsDir, 2
    }}
  );
  auto created = watcher->mEvents.drain();
  assertEvent(created, target, false, true, EntryKind::File);
  assertEvent(created, target, true, false, EntryKind::Directory);
  assertEvent(created, child, true, false, EntryKind::File);
  targetIndex = state->identities.find(target);
  assert(targetIndex != nullptr && targetIndex->isDirectory);
  assert(state->identities.find(child) != nullptr);
  targetTree = state->tree->find(target);
  assert(targetTree.has_value() && targetTree->isDir);
  assert(state->tree->find(child).has_value());
}

void runFileUpdate(WatcherRef watcher, const std::string &root) {
  std::string target = root + "/regular.txt";
  writeFile(target, "before");
  auto state = indexDirectory(root);
  auto before = readIndexedPath(target);
  assert(before.has_value());
  timespec times[2] = {{0, UTIME_OMIT}, {
    static_cast<time_t>(before->mtime / 1000000000 + 1), 123456789
  }};
  assert(utimensat(AT_FDCWD, target.c_str(), times, 0) == 0);
  processEvents(
    watcher,
    state,
    {PendingEvent {target, kFSEventStreamEventFlagItemModified, 3}}
  );
  assertEvent(
    watcher->mEvents.drain(), target, false, false, EntryKind::File
  );
}

void runAtomicReplacement(
  WatcherRef watcher,
  const std::string &root,
  FSEventStreamEventFlags flags,
  int index
) {
  std::string target = root + "/atomic-" + std::to_string(index) + ".txt";
  std::string temporary = target + ".tmp";
  writeFile(target, "before");
  writeFile(temporary, "after");
  auto state = indexDirectory(root);

  std::filesystem::rename(temporary, target);
  processEvents(watcher, state, {PendingEvent {target, flags, 10}});
  assertEvent(
    watcher->mEvents.drain(), target, false, false, EntryKind::File
  );
  const IndexedPath *indexed = state->identities.find(target);
  auto current = readIndexedPath(target);
  assert(indexed != nullptr && current.has_value());
  assert(indexed->identity == current->identity);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  std::string root(argv[1]);
  WatcherRef watcher(
    new Watcher(
      root,
      std::unordered_set<std::string> {},
      std::unordered_set<Glob> {}
    ),
    [](Watcher *) {}
  );

  const FSEventStreamEventFlags replacements[] = {
    kFSEventStreamEventFlagItemCreated |
      kFSEventStreamEventFlagItemRemoved |
      kFSEventStreamEventFlagItemModified,
    kFSEventStreamEventFlagItemModified,
    kFSEventStreamEventFlagItemCreated |
      kFSEventStreamEventFlagItemRemoved,
    kFSEventStreamEventFlagItemCreated,
    kFSEventStreamEventFlagItemInodeMetaMod,
  };
  for (auto flags : replacements) {
    runTypeReplacement(watcher, root, flags);
  }
  runFileUpdate(watcher, root);
  const FSEventStreamEventFlags atomicFlags[] = {
    kFSEventStreamEventFlagItemCreated,
    kFSEventStreamEventFlagItemRenamed,
    kFSEventStreamEventFlagItemCreated |
      kFSEventStreamEventFlagItemRenamed,
    kFSEventStreamEventFlagItemCreated |
      kFSEventStreamEventFlagItemRemoved,
    kFSEventStreamEventFlagItemModified,
    kFSEventStreamEventFlagMustScanSubDirs,
  };
  for (size_t index = 0; index < std::size(atomicFlags); ++index) {
    runAtomicReplacement(watcher, root, atomicFlags[index], index);
  }
}
