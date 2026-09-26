#include <memory>
#include <vector>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include "InotifyBackend.hh"

#define INOTIFY_MASK \
  IN_ATTRIB | IN_CREATE | IN_DELETE | \
  IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | \
  IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR | IN_EXCL_UNLINK
#define BUFFER_SIZE 8192
#define MOVE_PAIR_GRACE_MS 100
#define CONVERT_TIME(ts) ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec)

namespace {

bool isPathOrDescendant(const std::string &candidate, const std::string &path) {
  return candidate == path || (
    candidate.size() > path.size() &&
    candidate.compare(0, path.size(), path) == 0 &&
    candidate[path.size()] == '/'
  );
}

} // namespace

void InotifyBackend::start() {
  // Create a pipe that we will write to when we want to end the thread.
  int err = pipe2(mPipe, O_CLOEXEC | O_NONBLOCK);
  if (err == -1) {
    throw std::runtime_error(std::string("Unable to open pipe: ") + strerror(errno));
  }

  // Init inotify file descriptor.
  mInotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (mInotify == -1) {
    int error = errno;
    closeDescriptors();
    throw std::runtime_error(std::string("Unable to initialize inotify: ") + strerror(error));
  }

  pollfd pollfds[2];
  pollfds[0].fd = mPipe[0];
  pollfds[0].events = POLLIN;
  pollfds[0].revents = 0;
  pollfds[1].fd = mInotify;
  pollfds[1].events = POLLIN;
  pollfds[1].revents = 0;

  mLoopStarted = true;
  notifyStarted();

  try {
    // Loop until we get an event from the pipe.
    while (true) {
      int result = poll(pollfds, 2, MOVE_PAIR_GRACE_MS);
      if (result < 0) {
        throw std::runtime_error(std::string("Unable to poll: ") + strerror(errno));
      }

      if (pollfds[0].revents) {
        break;
      }

      if (pollfds[1].revents) {
        handleEvents();
      }

      flushExpiredMoves();
    }
  } catch (...) {
    closeDescriptors();
    mEndedSignal.notify();
    throw;
  }

  closeDescriptors();
  mEndedSignal.notify();
}

InotifyBackend::~InotifyBackend() {
  if (mLoopStarted) {
    write(mPipe[1], "X", 1);
    mEndedSignal.wait();
  }
}

void InotifyBackend::closeDescriptors() {
  if (mPipe[0] != -1) {
    close(mPipe[0]);
    mPipe[0] = -1;
  }
  if (mPipe[1] != -1) {
    close(mPipe[1]);
    mPipe[1] = -1;
  }
  if (mInotify != -1) {
    close(mInotify);
    mInotify = -1;
  }
}

// This function is called by Backend::watch which takes a lock on mMutex
void InotifyBackend::subscribe(WatcherRef watcher) {
  // Install each directory watch before scanning its entries. Changes made
  // during the scan remain queued until Backend::watch releases mMutex.
  std::shared_ptr<DirTree> tree = getTree(watcher, false);

  try {
    // Ancestor renames do not produce IN_MOVE_SELF on the root. Watch each
    // ancestor before opening the root so those changes cannot go unnoticed.
    for (size_t slash = watcher->mDir.find('/', 1); slash != std::string::npos;
         slash = watcher->mDir.find('/', slash + 1)) {
      std::string ancestor = watcher->mDir.substr(0, slash);
      if (!watchDir(watcher, ancestor, tree)) {
        throw WatcherError(
          "Unable to watch ancestor '" + ancestor + "': " + strerror(errno),
          watcher
        );
      }
    }
    if (!watchDir(watcher, watcher->mDir, tree) ||
        !addCreatedTree(watcher, watcher->mDir, tree, false)) {
      throw WatcherError(
        std::string("Unable to watch '") + watcher->mDir +
        "': " + strerror(errno), watcher
      );
    }
    tree->isComplete = true;
  } catch (...) {
    removeSubscriptions(watcher.get(), watcher->mDir);
    throw;
  }
}

bool InotifyBackend::watchDir(WatcherRef watcher, std::string path, std::shared_ptr<DirTree> tree) {
  uint32_t mask = path != watcher->mDir && isPathOrDescendant(watcher->mDir, path)
    ? IN_MOVE_SELF | IN_DELETE_SELF | IN_ONLYDIR
    : INOTIFY_MASK;
  // A recursive subscription may already share this ancestor's descriptor.
  int wd = inotify_add_watch(mInotify, path.c_str(), mask | IN_MASK_ADD);
  if (wd == -1) {
    return false;
  }

  bool alreadyTracked = false;
  std::unordered_set<int> staleDescriptors;
  for (auto it = mSubscriptions.begin(); it != mSubscriptions.end();) {
    if (
      it->second->watcher.get() == watcher.get() &&
      it->second->path == path
    ) {
      if (it->first == wd) {
        alreadyTracked = true;
        ++it;
      } else {
        staleDescriptors.insert(it->first);
        it = mSubscriptions.erase(it);
      }
    } else {
      ++it;
    }
  }
  for (int descriptor : staleDescriptors) {
    if (mSubscriptions.count(descriptor) == 0) {
      inotify_rm_watch(mInotify, descriptor);
    }
  }
  if (alreadyTracked) return true;

  std::shared_ptr<InotifySubscription> sub = std::make_shared<InotifySubscription>();
  sub->tree = tree;
  sub->path = path;
  sub->watcher = watcher;
  mSubscriptions.emplace(wd, sub);

  return true;
}

bool InotifyBackend::addCreatedTree(
  WatcherRef watcher,
  const std::string &path,
  std::shared_ptr<DirTree> tree,
  bool reportEvents
) {
  DIR *directory = opendir(path.c_str());
  if (directory == nullptr) return false;

  if (!tree->find(path)) {
    struct stat attributes;
    if (fstat(dirfd(directory), &attributes) != 0) {
      closedir(directory);
      return false;
    }
    tree->add(path, CONVERT_TIME(attributes.st_mtim), true);
  }

  std::vector<std::string> subdirectories;

  while (dirent *item = readdir(directory)) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }

    std::string candidate = path + "/" + item->d_name;
    if (watcher->isIgnored(candidate)) continue;

    struct stat attributes;
    if (lstat(candidate.c_str(), &attributes) != 0) continue;
    if (!S_ISREG(attributes.st_mode) && !S_ISDIR(attributes.st_mode)) {
      continue;
    }
    bool isDirectory = S_ISDIR(attributes.st_mode);
    bool existed = tree->find(candidate).has_value();
    if (!existed) {
      tree->add(candidate, CONVERT_TIME(attributes.st_mtim), isDirectory);
      if (reportEvents) {
        watcher->mEvents.create(candidate, entryKind(isDirectory));
      }
    }

    if (isDirectory) {
      subdirectories.push_back(std::move(candidate));
    }
  }

  closedir(directory);

  for (const auto &candidate : subdirectories) {
    if (!watchDir(watcher, candidate, tree) ||
        !addCreatedTree(watcher, candidate, tree, reportEvents)) {
      int error = errno;
      if (error == ENOENT || error == ENOTDIR) {
        // The entry moved or changed type between lstat and opening it.
        // Its queued parent event will reconcile the final path.
        removeSubscriptions(watcher.get(), candidate);
        tree->remove(candidate);
        continue;
      }
      errno = error;
      return false;
    }
  }

  return true;
}

bool InotifyBackend::reconcileMovedTree(
  WatcherRef watcher,
  const std::string &path,
  std::shared_ptr<DirTree> tree
) {
  // Events from a directory in flight cannot safely use its old path. Build
  // its final state at the destination, then report only the net changes.
  auto previous = tree->extract(path);
  if (!addCreatedTree(watcher, path, tree, false)) return false;

  std::unordered_map<std::string, DirEntry> before;
  for (auto &entry : previous) before.emplace(entry.path, entry);

  for (const auto &current : tree->entries) {
    if (!isPathOrDescendant(current.first, path)) continue;
    auto old = before.find(current.first);
    if (old == before.end()) {
      watcher->mEvents.create(current.first, entryKind(current.second.isDir));
    } else {
      if (old->second.isDir != current.second.isDir) {
        if (old->second.isDir) {
          removeSubscriptions(watcher.get(), old->first);
        }
        watcher->mEvents.update(current.first, entryKind(current.second.isDir));
      } else if (!current.second.isDir &&
          old->second.mtime != current.second.mtime) {
        watcher->mEvents.update(current.first, EntryKind::File);
      }
      before.erase(old);
    }
  }

  for (const auto &old : before) {
    if (!watcher->isIgnored(old.first)) {
      watcher->mEvents.remove(old.first, entryKind(old.second.isDir));
    }
    if (old.second.isDir) {
      removeSubscriptions(watcher.get(), old.first);
    }
  }
  return true;
}

void InotifyBackend::handleEvents() {
  char buf[BUFFER_SIZE] __attribute__ ((aligned(__alignof__(struct inotify_event))));;
  struct inotify_event *event;

  // Track all of the watchers that are touched so we can notify them at the end of the events.
  std::unordered_set<WatcherRef> watchers;
  while (true) {
    int n = read(mInotify, &buf, BUFFER_SIZE);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      throw std::runtime_error(std::string("Error reading from inotify: ") + strerror(errno));
    }

    if (n == 0) {
      break;
    }

    for (char *ptr = buf; ptr < buf + n; ptr += sizeof(*event) + event->len) {
      event = (struct inotify_event *)ptr;

      if ((event->mask & IN_Q_OVERFLOW) == IN_Q_OVERFLOW) {
        handleOverflow(watchers);
        continue;
      }

      handleEvent(event, watchers, mPendingMoves);
    }
  }

  for (auto it = watchers.begin(); it != watchers.end(); it++) {
    (*it)->notify();
  }
}

void InotifyBackend::handleOverflow(std::unordered_set<WatcherRef> &watchers) {
  std::unique_lock<std::mutex> lock(mMutex);
  std::unordered_set<WatcherRef> overflowed;
  for (const auto &subscription : mSubscriptions) {
    overflowed.insert(subscription.second->watcher);
  }

  for (const auto &watcher : overflowed) {
    invalidate(watcher);
    watcher->mEvents.error(
      "inotify queue overflow. The subscription can no longer guarantee "
      "complete filesystem events."
    );
    removeSubscriptions(watcher.get(), watcher->mDir);
    for (auto it = mPendingMoves.begin(); it != mPendingMoves.end();) {
      if (it->second.watcher.get() == watcher.get()) {
        it = mPendingMoves.erase(it);
      } else {
        ++it;
      }
    }
    watchers.insert(watcher);
  }
}

void InotifyBackend::flushExpiredMoves() {
  std::unordered_set<WatcherRef> watchers;
  auto now = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(mMutex);

  for (auto it = mPendingMoves.begin(); it != mPendingMoves.end();) {
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - it->second.createdAt
    );
    if (age.count() < MOVE_PAIR_GRACE_MS) {
      ++it;
      continue;
    }

    auto &move = it->second;
    // A new entry may have reused this path while the old move was waiting
    // for its pair. Suppress the old deletion only when it has the same kind
    // as the replacement; a type change needs both typed events.
    for (const auto &entry : move.entries) {
      auto replacement = move.tree->find(entry.path);
      if (!replacement || replacement->isDir != entry.isDir) {
        move.watcher->mEvents.remove(entry.path, entryKind(entry.isDir));
      }
    }
    if (move.isDirectory) {
      removeSubscriptions(move);
    }
    watchers.insert(move.watcher);
    it = mPendingMoves.erase(it);
  }

  lock.unlock();
  for (const auto &watcher : watchers) watcher->notify();
}

void InotifyBackend::moveSubscriptions(
  const PendingInotifyMove &move,
  const std::string &newPath
) {
  for (const auto &subscription : move.subscriptions) {
    subscription->path = newPath + subscription->path.substr(move.path.size());
  }
}

void InotifyBackend::removeSubscriptions(const PendingInotifyMove &move) {
  std::unordered_set<InotifySubscription *> removed;
  for (const auto &subscription : move.subscriptions) {
    removed.insert(subscription.get());
  }

  std::unordered_set<int> removedDescriptors;
  for (auto it = mSubscriptions.begin(); it != mSubscriptions.end();) {
    if (removed.count(it->second.get()) > 0) {
      removedDescriptors.insert(it->first);
      it = mSubscriptions.erase(it);
    } else {
      ++it;
    }
  }

  for (int descriptor : removedDescriptors) {
    if (mSubscriptions.count(descriptor) == 0) {
      inotify_rm_watch(mInotify, descriptor);
    }
  }
}

void InotifyBackend::removeSubscriptions(
  Watcher *watcher,
  const std::string &path
) {
  std::unordered_set<int> removedDescriptors;
  for (auto it = mSubscriptions.begin(); it != mSubscriptions.end();) {
    if (
      it->second->watcher.get() == watcher &&
      (path == watcher->mDir || isPathOrDescendant(it->second->path, path))
    ) {
      removedDescriptors.insert(it->first);
      it = mSubscriptions.erase(it);
    } else {
      ++it;
    }
  }

  for (int descriptor : removedDescriptors) {
    if (mSubscriptions.count(descriptor) == 0) {
      inotify_rm_watch(mInotify, descriptor);
    }
  }
}

void InotifyBackend::handleEvent(struct inotify_event *event, std::unordered_set<WatcherRef> &watchers, PendingInotifyMoves &pendingMoves) {
  std::unique_lock<std::mutex> lock(mMutex);

  // Find the subscriptions for this watch descriptor
  auto range = mSubscriptions.equal_range(event->wd);
  std::unordered_set<std::shared_ptr<InotifySubscription>> set;
  for (auto it = range.first; it != range.second; it++) {
    set.insert(it->second);
  }

  for (auto it = set.begin(); it != set.end(); it++) {
    if (handleSubscription(event, *it, pendingMoves)) {
      watchers.insert((*it)->watcher);
    }
  }
}

bool InotifyBackend::handleSubscription(struct inotify_event *event, std::shared_ptr<InotifySubscription> sub, PendingInotifyMoves &pendingMoves) {
  // Build full path and check if its in our ignore list.
  std::shared_ptr<Watcher> watcher = sub->watcher;
  std::string path = std::string(sub->path);
  bool isDir = event->mask & IN_ISDIR;

  if (path != watcher->mDir && isPathOrDescendant(watcher->mDir, path)) {
    // Shared ancestor descriptors can also deliver unrelated child events.
    if (!(event->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED))) {
      return false;
    }
    path = watcher->mDir;
  }

  if (path == watcher->mDir &&
      (event->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED))) {
    for (const auto &entry : sub->tree->extract(path)) {
      watcher->mEvents.remove(entry.path, entryKind(entry.isDir));
    }
    // Entries waiting for a move pair are no longer in the tree.
    for (auto it = pendingMoves.begin(); it != pendingMoves.end();) {
      if (it->second.watcher.get() == watcher.get()) {
        for (const auto &entry : it->second.entries) {
          watcher->mEvents.remove(entry.path, entryKind(entry.isDir));
        }
        it = pendingMoves.erase(it);
      } else {
        ++it;
      }
    }
    invalidate(watcher);
    removeSubscriptions(watcher.get(), path);
    return true;
  }

  if (event->len > 0) {
    path += "/" + std::string(event->name);
  }

  for (auto &pending : pendingMoves) {
    auto &move = pending.second;
    if (move.watcher.get() == watcher.get() && move.isDirectory) {
      for (const auto &movingSub : move.subscriptions) {
        if (movingSub == sub) {
          move.suppressedEvents = true;
          return false;
        }
      }
    }
  }

  if (watcher->isIgnored(path)) {
    return false;
  }

  // If this is a create, check if it's a directory and start watching if it is.
  // In any case, keep the directory tree up to date.
  if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
    auto pending = pendingMoves.end();
    if ((event->mask & IN_MOVED_TO) && event->cookie != 0) {
      pending = pendingMoves.find({watcher.get(), event->cookie});
    }

    bool isMoveWithinRoot = pending != pendingMoves.end();
    bool missingSource = false;
    bool suppressedEvents = false;
    std::string oldPath;
    PendingInotifyMove *move = nullptr;
    if (isMoveWithinRoot) {
      oldPath = pending->second.path;
      move = &pending->second;
      suppressedEvents = move->suppressedEvents;
      bool targetExisted = sub->tree->find(path).has_value();
      for (const auto &entry : move->entries) {
        std::string newEntryPath = path + entry.path.substr(oldPath.size());
        EntryKind kind = entryKind(entry.isDir);
        if (targetExisted || watcher->isIgnored(newEntryPath)) {
          watcher->mEvents.remove(entry.path, kind);
          if (!watcher->isIgnored(newEntryPath)) {
            if (sub->tree->find(newEntryPath).has_value()) {
              watcher->mEvents.update(newEntryPath, kind);
            } else {
              watcher->mEvents.create(newEntryPath, kind);
            }
          }
        } else {
          watcher->mEvents.rename(
            entry.path,
            newEntryPath,
            "inotify:" + std::to_string(event->cookie) +
              ":" + entry.path.substr(oldPath.size()),
            kind
          );
        }
      }
    }

    if (isMoveWithinRoot) {
      missingSource = move->entries.empty();
      // Unindexed sources include symlinks and special files. Keep the old
      // target until the type check below can report its deletion.
      if (!missingSource && sub->tree->find(path)) {
        sub->tree->remove(path);
      }
      sub->tree->restore(std::move(move->entries), oldPath, path);
      moveSubscriptions(*move, path);
      pendingMoves.erase(pending);
    }

    struct stat st;
    // Use lstat to avoid resolving symbolic links that we cannot watch anyway
    // https://github.com/parcel-bundler/watcher/issues/76
    if (lstat(path.c_str(), &st) != 0) {
      return false;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
      auto removed = sub->tree->extract(path);
      for (const auto &entry : removed) {
        watcher->mEvents.remove(entry.path, entryKind(entry.isDir));
        if (entry.isDir) removeSubscriptions(watcher.get(), entry.path);
      }
      return !removed.empty();
    }
    if (!isMoveWithinRoot) {
      if (sub->tree->find(path).has_value()) {
        watcher->mEvents.update(path, entryKind(S_ISDIR(st.st_mode)));
      } else {
        watcher->mEvents.create(path, entryKind(S_ISDIR(st.st_mode)));
      }
    } else if (missingSource) {
      sub->tree->remove(path);
      watcher->mEvents.rename(
        oldPath,
        path,
        "inotify:" + std::to_string(event->cookie),
        entryKind(S_ISDIR(st.st_mode))
      );
    }
    bool isDirectory = S_ISDIR(st.st_mode);
    if (isMoveWithinRoot) {
      if (!sub->tree->update(path, CONVERT_TIME(st.st_mtim))) {
        sub->tree->add(path, CONVERT_TIME(st.st_mtim), isDirectory);
        missingSource = true;
      }
    } else {
      sub->tree->add(path, CONVERT_TIME(st.st_mtim), isDirectory);
    }

    if (isDirectory) {
      // A move queued during the initial scan may have no indexed source or
      // directory watch to carry to its destination.
      bool rescanMovedTree = isMoveWithinRoot && (
        missingSource || !watcher->mIgnorePaths.empty() ||
        !watcher->mIgnoreGlobs.empty()
      );
      bool success = (isMoveWithinRoot && !missingSource) ||
        watchDir(watcher, path, sub->tree);
      if (success && (suppressedEvents || rescanMovedTree)) {
        success = reconcileMovedTree(watcher, path, sub->tree);
      } else if (success && !isMoveWithinRoot) {
        success = addCreatedTree(watcher, path, sub->tree);
      }
      if (!success) {
        if (!isMoveWithinRoot) sub->tree->remove(path);
        int error = errno;
        invalidate(watcher);
        watcher->mEvents.error(
          std::string("inotify_add_watch below '") + path +
          std::string("' failed: ") + strerror(error)
        );
        removeSubscriptions(watcher.get(), watcher->mDir);
        for (auto it = pendingMoves.begin(); it != pendingMoves.end();) {
          if (it->second.watcher.get() == watcher.get()) {
            it = pendingMoves.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
  } else if (event->mask & (IN_MODIFY | IN_ATTRIB)) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 ||
        (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
      return false;
    }
    if (!sub->tree->find(path)) return false;
    watcher->mEvents.update(path, entryKind(S_ISDIR(st.st_mode)));
    sub->tree->update(path, CONVERT_TIME(st.st_mtim));
  } else if (event->mask & (IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVE_SELF)) {
    bool isSelfEvent = (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF));
    // Root and ancestor self events were handled above.
    if (isSelfEvent) {
      return false;
    }

    if ((event->mask & IN_MOVED_FROM) && event->cookie != 0) {
      std::vector<std::shared_ptr<InotifySubscription>> movedSubscriptions;
      if (isDir) {
        for (const auto &subscription : mSubscriptions) {
          auto &candidate = subscription.second;
          if (
            candidate->watcher.get() == watcher.get() &&
            isPathOrDescendant(candidate->path, path)
          ) {
            movedSubscriptions.push_back(candidate);
          }
        }
      }
      pendingMoves.insert_or_assign(
        {watcher.get(), event->cookie},
        PendingInotifyMove {
          watcher,
          sub->tree,
          path,
          isDir,
          sub->tree->extract(path),
          std::move(movedSubscriptions),
          std::chrono::steady_clock::now()
        }
      );
    } else {
      for (const auto &entry : sub->tree->extract(path)) {
        watcher->mEvents.remove(entry.path, entryKind(entry.isDir));
      }
      if (isDir) {
        removeSubscriptions(watcher.get(), path);
      }
    }
  }

  return true;
}

// This function is called by Backend::unwatch which takes a lock on mMutex
void InotifyBackend::unsubscribe(WatcherRef watcher) {
  for (auto it = mPendingMoves.begin(); it != mPendingMoves.end();) {
    if (it->second.watcher.get() == watcher.get()) {
      it = mPendingMoves.erase(it);
    } else {
      ++it;
    }
  }

  // Find any subscriptions pointing to this watcher, and remove them.
  for (auto it = mSubscriptions.begin(); it != mSubscriptions.end();) {
    if (it->second->watcher.get() == watcher.get()) {
      if (mSubscriptions.count(it->first) == 1) {
        int err = inotify_rm_watch(mInotify, it->first);
        if (err == -1 && errno != EINVAL) {
          throw WatcherError(std::string("Unable to remove watcher: ") + strerror(errno), watcher);
        }
      }

      it = mSubscriptions.erase(it);
    } else {
      it++;
    }
  }
}
