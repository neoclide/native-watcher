#include <CoreServices/CoreServices.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
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

void flushStreamCallbacks(
  FSEventStreamRef stream,
  dispatch_queue_t queue
) {
  FSEventStreamFlushSync(stream);
  dispatch_sync(queue, ^ {});
}

struct PendingEvent {
  std::string path;
  FSEventStreamEventFlags flags;
  FSEventStreamEventId id;
};

class State: public WatcherState {
public:
  std::weak_ptr<FSEventsBackend> backend;
  std::atomic<FSEventStreamRef> stream {nullptr};
  dispatch_queue_t callbackQueue = nullptr;
  Signal cleanupComplete;
  std::shared_ptr<DirTree> tree;
  IdentityIndex identities;
  std::mutex initializationMutex;
  std::vector<PendingEvent> pendingEvents;
  uint64_t renameSequence = 0;
  bool initializing = true;
};

struct WatcherContext {
  std::atomic<size_t> references {1};
  WatcherRef watcher;
  std::shared_ptr<State> state;

  WatcherContext(WatcherRef watcher, std::shared_ptr<State> state)
    : watcher(watcher), state(std::move(state)) {}
};

const void *retainWatcherContext(const void *info) {
  auto *context = const_cast<WatcherContext *>(
    static_cast<const WatcherContext *>(info)
  );
  context->references.fetch_add(1);
  return info;
}

void releaseWatcherContext(const void *info) {
  auto *context = const_cast<WatcherContext *>(
    static_cast<const WatcherContext *>(info)
  );
  if (context->references.fetch_sub(1) == 1) {
    delete context;
  }
}

void stopStreamAfterCallback(
  std::shared_ptr<State> state,
  FSEventStreamRef stream
) {
  dispatch_queue_t queue = state->callbackQueue;
  dispatch_retain(queue);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    // Stop and invalidate do not wait for a callback already on this queue.
    dispatch_sync(queue, ^ {});
    FSEventStreamRelease(stream);
    state->cleanupComplete.notify();
    dispatch_release(queue);
  });
}

bool hasFlag(FSEventStreamEventFlags flags, FSEventStreamEventFlags flag) {
  return (flags & flag) == flag;
}

using RenameCandidates = std::unordered_map<
  FileIdentity,
  std::unordered_map<std::string, IndexedPath>,
  FileIdentityHash
>;

bool isDescendantPath(
  const std::string &path,
  const std::string &directory
) {
  return path.size() > directory.size() &&
    path.compare(0, directory.size(), directory) == 0 &&
    path[directory.size()] == '/';
}

void removeIndexedPath(State *state, EventList &events, const std::string &path) {
  const IndexedPath *indexed = state->identities.find(path);
  if (indexed == nullptr) return;
  if (!indexed->isDirectory) {
    events.remove(path, EntryKind::File);
    state->identities.remove(path);
    state->tree->remove(path);
    return;
  }
  for (const auto &entry : state->identities.entries()) {
    if (entry.first == path || isDescendantPath(entry.first, path)) {
      events.remove(entry.first, entryKind(entry.second.isDirectory));
    }
  }
  state->identities.remove(path);
  state->tree->remove(path);
}

void recordCreatedPath(
  State *state,
  EventList &events,
  const std::string &path,
  const IndexedPath &entry
) {
  const IndexedPath *previous = state->identities.find(path);
  bool sameIdentity = previous != nullptr &&
    previous->identity == entry.identity &&
    previous->isDirectory == entry.isDirectory;
  bool modified = sameIdentity && !entry.isDirectory &&
    previous->mtime != entry.mtime;
  state->identities.add(path, entry);
  if (!sameIdentity) state->tree->remove(path);
  if (!state->tree->update(path, entry.mtime)) {
    state->tree->add(path, entry.mtime, entry.isDirectory);
  }
  // Created flags can recur after the first create was delivered. Classify
  // against the index so a subsequent write is not emitted as another create.
  if (!sameIdentity) {
    events.create(path, entryKind(entry.isDirectory));
  } else if (modified) {
    events.update(path, EntryKind::File);
  }
}

void addCreatedPath(
  WatcherRef watcher,
  State *state,
  EventList &events,
  const std::string &path
) {
  auto entry = readIndexedPath(path);
  if (!entry.has_value()) {
    return;
  }

  if (!entry->isDirectory) {
    recordCreatedPath(state, events, path, *entry);
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
      recordCreatedPath(state, events, candidate, candidateEntry);
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
        oldEntry.isDirectory || newEntry.isDirectory ||
        oldEntry.linkCount != 1 || newEntry.linkCount != 1) {
      continue;
    }

    std::string renameId =
      "fsevents:" + std::to_string(++state->renameSequence);
    watcher->mEvents.rename(
      oldCandidate.first,
      newCandidate.first,
      renameId,
      entryKind(oldEntry.isDirectory)
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

    if (coveredByDirectory) continue;
    acceptedRenames.push_back(candidate);
    watcher->mEvents.rename(
      candidate.oldPath,
      candidate.newPath,
      "fsevents:" + std::to_string(++state->renameSequence),
      entryKind(candidate.entry.isDirectory)
    );
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
            newEntry->identity == path.second.identity &&
            newEntry->isDirectory == path.second.isDirectory) {
          watcher->mEvents.rename(
            path.first,
            newPath,
            "fsevents:" + std::to_string(++state->renameSequence),
            entryKind(path.second.isDirectory)
          );
          handledRemoved.insert(path.first);
          handledCreated.insert(newPath);
        }
      }
    }
  }

  for (const auto &entry : removed) {
    for (const auto &path : entry.second) {
      if (handledRemoved.count(path.first) == 0) {
        watcher->mEvents.remove(
          path.first, entryKind(path.second.isDirectory)
        );
      }
    }
  }
  for (const auto &entry : created) {
    for (const auto &path : entry.second) {
      if (handledCreated.count(path.first) == 0) {
        watcher->mEvents.create(
          path.first, entryKind(path.second.isDirectory)
        );
      }
    }
  }
  for (const auto &after : current.entries()) {
    const IndexedPath *before = state->identities.find(after.first);
    if (before != nullptr && before->identity == after.second.identity &&
        !after.second.isDirectory && before->mtime != after.second.mtime) {
      watcher->mEvents.update(after.first, EntryKind::File);
    }
  }

  state->identities = std::move(current);
  state->tree = std::move(newTree);
}

void processEvents(
  WatcherRef watcher,
  const std::shared_ptr<State> &stateGuard,
  const std::vector<PendingEvent> &events
) {
  State *state = stateGuard.get();
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
  if (!requiresRescan) {
    // A directory rename can change paths for every descendant even when
    // FSEvents reports only the parent. Compare the complete indexed trees.
    requiresRescan = std::any_of(
      events.begin(), events.end(),
      [state](const PendingEvent &event) {
        if (!hasFlag(event.flags, kFSEventStreamEventFlagItemRenamed)) {
          return false;
        }
        const IndexedPath *before = state->identities.find(event.path);
        auto after = readIndexedPath(event.path);
        return hasFlag(event.flags, kFSEventStreamEventFlagItemIsDir) ||
          (before != nullptr && before->isDirectory) ||
          (after.has_value() && after->isDirectory);
      }
    );
  }
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
    bool hasMetadataChange =
      hasFlag(event.flags, kFSEventStreamEventFlagItemInodeMetaMod) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemFinderInfoMod) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemChangeOwner) ||
      hasFlag(event.flags, kFSEventStreamEventFlagItemXattrMod);
    bool isRenamed = hasFlag(event.flags, kFSEventStreamEventFlagItemRenamed);
    bool isDone = hasFlag(event.flags, kFSEventStreamEventFlagHistoryDone);

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
        addCreatedPath(watcher, state, list, event.path);
      } catch (const std::exception &error) {
        list.error(error.what());
      }
    } else if (isRemoved && !(isCreated || isModified || isRenamed)) {
      removeIndexedPath(state, list, event.path);
      if (event.path == watcher->mDir) deletedRoot = true;
    } else if (isModified && !(isCreated || isRemoved || isRenamed)) {
      auto indexed = readIndexedPath(event.path);
      if (!indexed.has_value()) {
        removeIndexedPath(state, list, event.path);
        if (event.path == watcher->mDir) deletedRoot = true;
        continue;
      }

      const IndexedPath *previousIdentity =
        state->identities.find(event.path);
      bool sameIdentity = previousIdentity != nullptr &&
        previousIdentity->identity == indexed->identity;
      auto entry = state->tree->find(event.path);
      if (previousIdentity == nullptr || !entry) {
        try {
          addCreatedPath(watcher, state, list, event.path);
        } catch (const std::exception &error) {
          list.error(error.what());
        }
        continue;
      }
      if (!hasMetadataChange && entry && sameIdentity && entry->mtime == indexed->mtime &&
          indexed->mtime % 1000000000 != 0) {
        continue;
      }

      state->identities.add(event.path, *indexed);
      if (!state->tree->update(event.path, indexed->mtime)) {
        state->tree->add(
          event.path,
          indexed->mtime,
          indexed->isDirectory
        );
      }
      list.update(event.path, entryKind(indexed->isDirectory));
    } else {
      auto indexed = readIndexedPath(event.path);
      if (!indexed.has_value()) {
        removeIndexedPath(state, list, event.path);
        if (event.path == watcher->mDir) deletedRoot = true;
        continue;
      }

      const IndexedPath *previousIdentity =
        state->identities.find(event.path);
      bool sameIdentity = previousIdentity != nullptr &&
        previousIdentity->identity == indexed->identity;
      auto entry = state->tree->find(event.path);
      if (!hasMetadataChange && entry && sameIdentity && entry->mtime == indexed->mtime &&
          indexed->mtime % 1000000000 != 0) {
        continue;
      }

      if ((isModified || sameIdentity) && entry) {
        state->identities.add(event.path, *indexed);
        if (!state->tree->update(event.path, indexed->mtime)) {
          state->tree->add(event.path, indexed->mtime, indexed->isDirectory);
        }
        list.update(event.path, entryKind(indexed->isDirectory));
      } else if (!sameIdentity && indexed->isDirectory) {
        try {
          addCreatedPath(watcher, state, list, event.path);
        } catch (const std::exception &error) {
          list.error(error.what());
        }
      } else {
        state->identities.add(event.path, *indexed);
        state->tree->add(
          event.path,
          indexed->mtime,
          indexed->isDirectory
        );
        list.create(event.path, entryKind(indexed->isDirectory));
      }
    }
  }

  watcher->notify();
  if (deletedRoot) {
    auto stream = state->stream.exchange(nullptr);
    if (stream != nullptr) {
      watcher->mNeedsResubscribe = true;
      stopStreamAfterCallback(stateGuard, stream);
    }
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
  auto *context = static_cast<WatcherContext *>(clientCallBackInfo);
  WatcherRef &watcher = context->watcher;
  const auto &stateGuard = context->state;
  auto *state = stateGuard.get();
  if (state->stream.load() != streamRef) return;

  try {
    std::vector<PendingEvent> events;
    events.reserve(numEvents);
    for (size_t i = 0; i < numEvents; ++i) {
      events.push_back(PendingEvent {paths[i], eventFlags[i], eventIds[i]});
    }

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

    processEvents(watcher, stateGuard, events);
  } catch (std::exception &err) {
    if (state != nullptr) {
      auto backend = state->backend.lock();
      if (backend != nullptr) {
        backend->handleBackendError(err);
      }
    }
  }
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

  auto stateGuard = std::static_pointer_cast<State>(watcher->state);
  State *state = stateGuard.get();
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

  // FSEvents retains this context and releases it with the stream.
  auto *callbackWatcher = new WatcherContext(watcher, stateGuard);
  FSEventStreamContext callbackInfo {
    0,
    callbackWatcher,
    retainWatcherContext,
    releaseWatcherContext,
    nullptr
  };
  FSEventStreamRef stream = FSEventStreamCreate(
    NULL,
    &FSEventsCallback,
    &callbackInfo,
    pathsToWatch,
    id,
    latency,
    kFSEventStreamCreateFlagFileEvents
  );
  releaseWatcherContext(callbackWatcher);
  if (stream == nullptr) {
    CFRelease(pathsToWatch);
    CFRelease(fileWatchPath);
    throw WatcherError("Error creating FSEvents stream", watcher);
  }

  CFMutableArrayRef exclusions = CFArrayCreateMutable(
    NULL,
    watcher->mIgnorePaths.size(),
    &kCFTypeArrayCallBacks
  );
  for (auto it = watcher->mIgnorePaths.begin(); it != watcher->mIgnorePaths.end(); it++) {
    CFStringRef path = CFStringCreateWithCString(
      NULL,
      it->c_str(),
      kCFStringEncodingUTF8
    );

    CFArrayAppendValue(exclusions, (const void *)path);
    CFRelease(path);
  }

  FSEventStreamSetExclusionPaths(stream, exclusions);
  CFRelease(exclusions);

  FSEventStreamSetDispatchQueue(stream, mQueue);
  state->stream = stream;
  bool started = FSEventStreamStart(stream);

  CFRelease(pathsToWatch);
  CFRelease(fileWatchPath);

  if (!started) {
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    state->stream = nullptr;
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

  // Events can arrive while the initial identity index is being scanned. The
  // recorded paths may already be stale after a rename chain, so reconcile
  // against the current filesystem until a scan completes without receiving
  // another event batch.
  while (state->stream.load() == stream) {
    flushStreamCallbacks(stream, mQueue);
    {
      std::lock_guard<std::mutex> lock(state->initializationMutex);
      if (state->pendingEvents.empty()) {
        state->initializing = false;
        break;
      }
      state->pendingEvents.clear();
    }
    reconcileFullTree(watcher, state);
    watcher->notify();
  }
}

void FSEventsBackend::start() {
  mQueue = dispatch_queue_create("native-watcher.fsevents", DISPATCH_QUEUE_SERIAL);
  notifyStarted();
  mStoppedSignal.wait();
}

FSEventsBackend::~FSEventsBackend() {
  mStoppedSignal.notify();
  // The thread uses mStoppedSignal, which is destroyed before Backend's
  // destructor can join the thread.
  if (mThread.joinable()) {
    if (mThread.get_id() == std::this_thread::get_id()) {
      mThread.detach();
    } else {
      mThread.join();
    }
  }
  std::unique_lock<std::mutex> lock(mMutex);
  if (mQueue != nullptr) {
    dispatch_release(mQueue);
    mQueue = nullptr;
  }
}

// Backend::handleError holds mMutex while retiring the failed backend.
void FSEventsBackend::cleanupAfterError() {
  for (const auto &watcher : mSubscriptions) {
    auto state = std::static_pointer_cast<State>(watcher->state);
    if (state != nullptr && state->backend.lock().get() == this) {
      unsubscribe(watcher);
    }
  }
}

void FSEventsBackend::handleBackendError(std::exception &err) {
  auto self = shared_from_this();
  handleError(err);
  mStoppedSignal.notify();
}

// This function is called by Backend::watch which takes a lock on mMutex
void FSEventsBackend::subscribe(WatcherRef watcher) {
  auto s = std::make_shared<State>();
  s->backend = std::static_pointer_cast<FSEventsBackend>(shared_from_this());
  s->callbackQueue = mQueue;
  watcher->state = s;
  try {
    startStream(watcher, kFSEventStreamEventIdSinceNow);
  } catch (...) {
    auto stream = s->stream.exchange(nullptr);
    if (stream != nullptr) {
      stopStreamAfterCallback(s, stream);
    }
    watcher->state = nullptr;
    throw;
  }
}

// This function is called by Backend::unwatch which takes a lock on mMutex
void FSEventsBackend::unsubscribe(WatcherRef watcher) {
  auto stateGuard = watcher->state;
  auto state = std::static_pointer_cast<State>(stateGuard);
  if (state != nullptr) {
    auto stream = state->stream.exchange(nullptr);
    if (stream != nullptr) {
      stopStreamAfterCallback(state, stream);
    }
  }
}

void FSEventsBackend::finishUnsubscribe(
  WatcherRef watcher,
  std::shared_ptr<WatcherState> stateGuard
) {
  auto state = std::static_pointer_cast<State>(stateGuard);
  if (state == nullptr) return;

  state->cleanupComplete.wait();
  std::unique_lock<std::mutex> lock(mMutex);
  if (watcher->state == stateGuard) {
    watcher->state = nullptr;
  }
}
