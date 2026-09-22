#include <CoreServices/CoreServices.h>
#include <sys/stat.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../Event.hh"
#include "../Backend.hh"
#include "./FSEventsBackend.hh"
#include "./IdentityIndex.hh"
#include "../Watcher.hh"

#define CONVERT_TIME(ts) ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec)
#define IGNORED_FLAGS (kFSEventStreamEventFlagItemIsHardlink | kFSEventStreamEventFlagItemIsLastHardlink | kFSEventStreamEventFlagItemIsSymlink | kFSEventStreamEventFlagItemIsDir | kFSEventStreamEventFlagItemIsFile)

void stopStream(FSEventStreamRef stream, CFRunLoopRef runLoop) {
  FSEventStreamStop(stream);
  FSEventStreamUnscheduleFromRunLoop(stream, runLoop, kCFRunLoopDefaultMode);
  FSEventStreamInvalidate(stream);
  FSEventStreamRelease(stream);
}

struct PendingEvent {
  std::string path;
  FSEventStreamEventFlags flags;
  FSEventStreamEventId id;
};

class State: public WatcherState {
public:
  FSEventStreamRef stream;
  std::shared_ptr<DirTree> tree;
  IdentityIndex identities;
  std::mutex initializationMutex;
  std::vector<PendingEvent> pendingEvents;
  uint64_t renameSequence = 0;
  bool initializing = true;
};

bool hasFlag(FSEventStreamEventFlags flags, FSEventStreamEventFlags flag) {
  return (flags & flag) == flag;
}

using RenameCandidates = std::unordered_map<
  FileIdentity,
  std::unordered_map<std::string, IndexedPath>,
  FileIdentityHash
>;

void addCreatedPath(
  WatcherRef watcher,
  State *state,
  EventList &events,
  const std::string &path,
  bool isDirectoryHint
) {
  auto entry = readIndexedPath(path);
  if (!entry.has_value()) {
    state->tree->add(path, 0, isDirectoryHint);
    events.create(path);
    return;
  }

  if (!entry->isDirectory) {
    state->identities.add(path, *entry);
    state->tree->add(path, entry->mtime, false);
    events.create(path);
    return;
  }

  // FSEvents may report only the top-level directory when a populated tree is
  // moved into the watched root. Index and report the full subtree now so a
  // later rename of an existing child can still be correlated by identity.
  scanIdentityIndex(
    path,
    [watcher](const std::string &candidate) {
      return watcher->isIgnored(candidate);
    },
    [state, &events](
      const std::string &candidate,
      const IndexedPath &candidateEntry
    ) {
      state->identities.add(candidate, candidateEntry);
      state->tree->add(
        candidate,
        candidateEntry.mtime,
        candidateEntry.isDirectory
      );
      events.create(candidate);
    }
  );
}

std::unordered_set<std::string> correlateRenames(
  WatcherRef watcher,
  State *state,
  const std::vector<PendingEvent> &events
) {
  RenameCandidates removed;
  RenameCandidates created;

  for (const auto &event : events) {
    if (!hasFlag(event.flags, kFSEventStreamEventFlagItemRenamed) ||
        watcher->isIgnored(event.path)) {
      continue;
    }

    const IndexedPath *before = state->identities.find(event.path);
    auto after = readIndexedPath(event.path);
    if (before != nullptr &&
        (!after.has_value() || !(before->identity == after->identity))) {
      removed[before->identity].insert_or_assign(event.path, *before);
    }
    if (after.has_value() &&
        (before == nullptr || !(before->identity == after->identity))) {
      created[after->identity].insert_or_assign(event.path, *after);
    }
  }

  std::unordered_set<std::string> handled;
  for (const auto &removedIdentity : removed) {
    auto matchingCreated = created.find(removedIdentity.first);
    if (removedIdentity.second.size() != 1 ||
        matchingCreated == created.end() ||
        matchingCreated->second.size() != 1 ||
        state->identities.pathCount(removedIdentity.first) != 1) {
      continue;
    }

    const auto &oldCandidate = *removedIdentity.second.begin();
    const auto &newCandidate = *matchingCreated->second.begin();
    const IndexedPath &oldEntry = oldCandidate.second;
    const IndexedPath &newEntry = newCandidate.second;

    // A pre-existing target is a replacement, and multiple links make inode
    // identity insufficient to prove which directory entry was renamed.
    if (state->identities.find(newCandidate.first) != nullptr ||
        oldEntry.isDirectory != newEntry.isDirectory ||
        (!oldEntry.isDirectory &&
         (oldEntry.linkCount != 1 || newEntry.linkCount != 1))) {
      continue;
    }

    std::string renameId =
      "fsevents:" + std::to_string(++state->renameSequence);
    watcher->mEvents.rename(
      oldCandidate.first,
      newCandidate.first,
      renameId
    );

    state->identities.rename(oldCandidate.first, newCandidate.first);
    state->identities.add(newCandidate.first, newEntry);
    state->tree->remove(oldCandidate.first);
    state->tree->add(
      newCandidate.first,
      newEntry.mtime,
      newEntry.isDirectory
    );
    handled.insert(oldCandidate.first);
    handled.insert(newCandidate.first);
  }

  return handled;
}

bool isDescendantPath(
  const std::string &path,
  const std::string &directory
) {
  return path.size() > directory.size() &&
    path.compare(0, directory.size(), directory) == 0 &&
    path[directory.size()] == '/';
}

struct ReconciledRename {
  std::string oldPath;
  std::string newPath;
  IndexedPath entry;
};

void reconcileFullTree(WatcherRef watcher, State *state) {
  auto newTree = std::make_shared<DirTree>(watcher->mDir);
  IdentityIndex current = scanIdentityIndex(
    watcher->mDir,
    [watcher](const std::string &path) {
      return watcher->isIgnored(path);
    },
    [newTree](const std::string &path, const IndexedPath &entry) {
      newTree->add(path, entry.mtime, entry.isDirectory);
    }
  );

  RenameCandidates removed;
  RenameCandidates created;
  for (const auto &before : state->identities.entries()) {
    const IndexedPath *after = current.find(before.first);
    if (after == nullptr || !(after->identity == before.second.identity)) {
      removed[before.second.identity].insert_or_assign(
        before.first,
        before.second
      );
    }
  }
  for (const auto &after : current.entries()) {
    const IndexedPath *before = state->identities.find(after.first);
    if (before == nullptr || !(before->identity == after.second.identity)) {
      created[after.second.identity].insert_or_assign(
        after.first,
        after.second
      );
    }
  }

  std::vector<ReconciledRename> possibleRenames;
  for (const auto &removedIdentity : removed) {
    auto matchingCreated = created.find(removedIdentity.first);
    if (removedIdentity.second.size() != 1 ||
        matchingCreated == created.end() ||
        matchingCreated->second.size() != 1 ||
        state->identities.pathCount(removedIdentity.first) != 1 ||
        current.pathCount(removedIdentity.first) != 1) {
      continue;
    }

    const auto &oldCandidate = *removedIdentity.second.begin();
    const auto &newCandidate = *matchingCreated->second.begin();
    if (state->identities.find(newCandidate.first) != nullptr ||
        oldCandidate.second.isDirectory != newCandidate.second.isDirectory ||
        (!oldCandidate.second.isDirectory &&
         (oldCandidate.second.linkCount != 1 ||
          newCandidate.second.linkCount != 1))) {
      continue;
    }

    possibleRenames.push_back(ReconciledRename {
      oldCandidate.first,
      newCandidate.first,
      newCandidate.second
    });
  }

  std::sort(
    possibleRenames.begin(),
    possibleRenames.end(),
    [](const ReconciledRename &left, const ReconciledRename &right) {
      if (left.entry.isDirectory != right.entry.isDirectory) {
        return left.entry.isDirectory;
      }
      return left.oldPath.size() < right.oldPath.size();
    }
  );

  std::vector<ReconciledRename> acceptedRenames;
  std::unordered_set<std::string> handledRemoved;
  std::unordered_set<std::string> handledCreated;
  for (const auto &candidate : possibleRenames) {
    bool coveredByDirectory = false;
    for (const auto &accepted : acceptedRenames) {
      if (!accepted.entry.isDirectory ||
          !isDescendantPath(candidate.oldPath, accepted.oldPath)) {
        continue;
      }
      std::string expectedNewPath =
        accepted.newPath + candidate.oldPath.substr(accepted.oldPath.size());
      if (candidate.newPath == expectedNewPath) {
        coveredByDirectory = true;
        break;
      }
    }

    if (!coveredByDirectory) {
      acceptedRenames.push_back(candidate);
      watcher->mEvents.rename(
        candidate.oldPath,
        candidate.newPath,
        "fsevents:" + std::to_string(++state->renameSequence)
      );
    }
    handledRemoved.insert(candidate.oldPath);
    handledCreated.insert(candidate.newPath);
  }

  // A directory rename accounts for unchanged descendants even when a full
  // rescan observed each child as a removed and added path.
  for (const auto &accepted : acceptedRenames) {
    if (!accepted.entry.isDirectory) continue;
    for (const auto &entry : removed) {
      for (const auto &path : entry.second) {
        if (!isDescendantPath(path.first, accepted.oldPath)) continue;
        std::string newPath =
          accepted.newPath + path.first.substr(accepted.oldPath.size());
        const IndexedPath *newEntry = current.find(newPath);
        if (newEntry != nullptr &&
            newEntry->identity == path.second.identity) {
          handledRemoved.insert(path.first);
          handledCreated.insert(newPath);
        }
      }
    }
  }

  for (const auto &entry : removed) {
    for (const auto &path : entry.second) {
      if (handledRemoved.count(path.first) == 0) {
        watcher->mEvents.remove(path.first);
      }
    }
  }
  for (const auto &entry : created) {
    for (const auto &path : entry.second) {
      if (handledCreated.count(path.first) == 0) {
        watcher->mEvents.create(path.first);
      }
    }
  }
  for (const auto &after : current.entries()) {
    const IndexedPath *before = state->identities.find(after.first);
    if (before != nullptr && before->identity == after.second.identity &&
        !after.second.isDirectory && before->mtime != after.second.mtime) {
      watcher->mEvents.update(after.first);
    }
  }

  state->identities = std::move(current);
  state->tree = std::move(newTree);
}

void processEvents(
  ConstFSEventStreamRef streamRef,
  WatcherRef watcher,
  const std::vector<PendingEvent> &events
) {
  if (watcher->state == nullptr) return;

  auto stateGuard = watcher->state;
  auto *state = static_cast<State *>(stateGuard.get());
  EventList &list = watcher->mEvents;
  bool deletedRoot = false;

  bool requiresRescan = std::any_of(
    events.begin(),
    events.end(),
    [](const PendingEvent &event) {
      return hasFlag(
        event.flags,
        kFSEventStreamEventFlagMustScanSubDirs
      );
    }
  );
  if (requiresRescan) {
    try {
      reconcileFullTree(watcher, state);
    } catch (const std::exception &error) {
      list.error(error.what());
    }
    watcher->notify();
    return;
  }

  auto correlatedPaths = correlateRenames(watcher, state, events);

  for (const auto &event : events) {
    bool isCreated = hasFlag(event.flags, kFSEventStreamEventFlagItemCreated);
    bool isRemoved = hasFlag(event.flags, kFSEventStreamEventFlagItemRemoved);
    bool isModified =
      hasFlag(event.flags, kFSEventStreamEventFlagItemModified) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemInodeMetaMod) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemFinderInfoMod) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemChangeOwner) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemXattrMod);
    bool isRenamed = hasFlag(event.flags, kFSEventStreamEventFlagItemRenamed);
    bool isDone = hasFlag(event.flags, kFSEventStreamEventFlagHistoryDone);
    bool isDir = hasFlag(event.flags, kFSEventStreamEventFlagItemIsDir);

    if (isDone) {
      watcher->notify();
      break;
    }

    auto ignoredFlags = IGNORED_FLAGS;
    if (__builtin_available(macOS 10.13, *)) {
      ignoredFlags |= kFSEventStreamEventFlagItemCloned;
    }
    if ((event.flags & ~ignoredFlags) == 0 ||
        watcher->isIgnored(event.path) ||
        correlatedPaths.count(event.path) > 0) {
      continue;
    }

    if (isCreated && !(isRemoved || isModified || isRenamed)) {
      try {
        addCreatedPath(watcher, state, list, event.path, isDir);
      } catch (const std::exception &error) {
        list.error(error.what());
      }
    } else if (isRemoved && !(isCreated || isModified || isRenamed)) {
      state->identities.remove(event.path);
      state->tree->remove(event.path);
      list.remove(event.path);
      if (event.path == watcher->mDir) deletedRoot = true;
    } else if (isModified && !(isCreated || isRemoved || isRenamed)) {
      auto indexed = readIndexedPath(event.path);
      if (!indexed.has_value()) continue;

      const IndexedPath *previousIdentity =
        state->identities.find(event.path);
      bool sameIdentity = previousIdentity != nullptr &&
        previousIdentity->identity == indexed->identity;
      DirEntry *entry = state->tree->find(event.path);
      if (entry && sameIdentity && entry->mtime == indexed->mtime &&
          indexed->mtime % 1000000000 != 0) {
        continue;
      }

      state->identities.add(event.path, *indexed);
      if (entry) {
        entry->mtime = indexed->mtime;
      } else {
        state->tree->add(
          event.path,
          indexed->mtime,
          indexed->isDirectory
        );
      }
      list.update(event.path);
    } else {
      auto indexed = readIndexedPath(event.path);
      if (!indexed.has_value()) {
        state->identities.remove(event.path);
        state->tree->remove(event.path);
        list.remove(event.path);
        if (event.path == watcher->mDir) deletedRoot = true;
        continue;
      }

      const IndexedPath *previousIdentity =
        state->identities.find(event.path);
      bool sameIdentity = previousIdentity != nullptr &&
        previousIdentity->identity == indexed->identity;
      DirEntry *entry = state->tree->find(event.path);
      if (entry && sameIdentity && entry->mtime == indexed->mtime &&
          indexed->mtime % 1000000000 != 0) {
        continue;
      }

      state->identities.add(event.path, *indexed);
      if (isModified && entry) {
        state->tree->update(event.path, indexed->mtime);
        list.update(event.path);
      } else if (!sameIdentity && indexed->isDirectory) {
        try {
          addCreatedPath(watcher, state, list, event.path, true);
        } catch (const std::exception &error) {
          list.error(error.what());
        }
      } else {
        state->tree->add(
          event.path,
          indexed->mtime,
          indexed->isDirectory
        );
        list.create(event.path);
      }
    }
  }

  watcher->notify();
  if (deletedRoot) {
    stopStream((FSEventStreamRef)streamRef, CFRunLoopGetCurrent());
    watcher->state = nullptr;
  }
}

void FSEventsCallback(
  ConstFSEventStreamRef streamRef,
  void *clientCallBackInfo,
  size_t numEvents,
  void *eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
) {
  char **paths = (char **)eventPaths;
  std::shared_ptr<Watcher>& watcher = *static_cast<std::shared_ptr<Watcher> *>(clientCallBackInfo);
  if (watcher->state == nullptr) return;

  std::vector<PendingEvent> events;
  events.reserve(numEvents);
  for (size_t i = 0; i < numEvents; ++i) {
    events.push_back(PendingEvent {paths[i], eventFlags[i], eventIds[i]});
  }

  auto stateGuard = watcher->state;
  auto *state = static_cast<State *>(stateGuard.get());
  {
    std::lock_guard<std::mutex> lock(state->initializationMutex);
    if (state->initializing) {
      state->pendingEvents.insert(
        state->pendingEvents.end(),
        events.begin(),
        events.end()
      );
      return;
    }
  }

  processEvents(streamRef, watcher, events);
}

void checkWatcher(WatcherRef watcher) {
  struct stat file;
  if (stat(watcher->mDir.c_str(), &file)) {
    throw WatcherError(strerror(errno), watcher);
  }

  if (!S_ISDIR(file.st_mode)) {
    throw WatcherError(strerror(ENOTDIR), watcher);
  }
}

void FSEventsBackend::startStream(WatcherRef watcher, FSEventStreamEventId id) {
  checkWatcher(watcher);

  auto stateGuard = watcher->state;
  State *state = static_cast<State *>(stateGuard.get());
  state->tree = std::make_shared<DirTree>(watcher->mDir);

  CFAbsoluteTime latency = 0.001;
  CFStringRef fileWatchPath = CFStringCreateWithCString(
    NULL,
    watcher->mDir.c_str(),
    kCFStringEncodingUTF8
  );

  CFArrayRef pathsToWatch = CFArrayCreate(
    NULL,
    (const void **)&fileWatchPath,
    1,
    NULL
  );

  // Make a watcher reference we can pass into the callback. This ensures bumped ref-count.
  std::shared_ptr<Watcher>* callbackWatcher = new std::shared_ptr<Watcher> (watcher);
  FSEventStreamContext callbackInfo {0, static_cast<void*> (callbackWatcher), nullptr, nullptr, nullptr};
  FSEventStreamRef stream = FSEventStreamCreate(
    NULL,
    &FSEventsCallback,
    &callbackInfo,
    pathsToWatch,
    id,
    latency,
    kFSEventStreamCreateFlagFileEvents
  );

  CFMutableArrayRef exclusions = CFArrayCreateMutable(NULL, watcher->mIgnorePaths.size(), NULL);
  for (auto it = watcher->mIgnorePaths.begin(); it != watcher->mIgnorePaths.end(); it++) {
    CFStringRef path = CFStringCreateWithCString(
      NULL,
      it->c_str(),
      kCFStringEncodingUTF8
    );

    CFArrayAppendValue(exclusions, (const void *)path);
  }

  FSEventStreamSetExclusionPaths(stream, exclusions);

  FSEventStreamScheduleWithRunLoop(stream, mRunLoop, kCFRunLoopDefaultMode);
  state->stream = stream;
  bool started = FSEventStreamStart(stream);

  CFRelease(pathsToWatch);
  CFRelease(fileWatchPath);

  if (!started) {
    FSEventStreamRelease(stream);
    throw WatcherError("Error starting FSEvents stream", watcher);
  }

  IdentityIndex identities = scanIdentityIndex(
    watcher->mDir,
    [watcher](const std::string &path) {
      return watcher->isIgnored(path);
    },
    [state](const std::string &path, const IndexedPath &entry) {
      state->tree->add(path, entry.mtime, entry.isDirectory);
    }
  );

  {
    std::lock_guard<std::mutex> lock(state->initializationMutex);
    state->identities = std::move(identities);
  }

  // Events can arrive while the initial identity index is being scanned.
  // Drain those batches before allowing the callback to process live events.
  while (watcher->state != nullptr) {
    std::vector<PendingEvent> pending;
    {
      std::lock_guard<std::mutex> lock(state->initializationMutex);
      if (state->pendingEvents.empty()) {
        state->initializing = false;
        break;
      }
      pending.swap(state->pendingEvents);
    }
    processEvents(stream, watcher, pending);
  }
}

void FSEventsBackend::start() {
  mRunLoop = CFRunLoopGetCurrent();
  CFRetain(mRunLoop);

  // Unlock once run loop has started.
  CFRunLoopPerformBlock(mRunLoop, kCFRunLoopDefaultMode, ^ {
    notifyStarted();
  });

  CFRunLoopWakeUp(mRunLoop);
  CFRunLoopRun();
}

FSEventsBackend::~FSEventsBackend() {
  std::unique_lock<std::mutex> lock(mMutex);
  CFRunLoopStop(mRunLoop);
  CFRelease(mRunLoop);
}

// This function is called by Backend::watch which takes a lock on mMutex
void FSEventsBackend::subscribe(WatcherRef watcher) {
  auto s = std::make_shared<State>();
  watcher->state = s;
  startStream(watcher, kFSEventStreamEventIdSinceNow);
}

// This function is called by Backend::unwatch which takes a lock on mMutex
void FSEventsBackend::unsubscribe(WatcherRef watcher) {
  auto stateGuard = watcher->state;
  State* s = static_cast<State*>(stateGuard.get());
  if (s != nullptr) {
    stopStream(s->stream, mRunLoop);
    watcher->state = nullptr;
  }
}
