#ifdef FS_EVENTS
#include "macos/FSEventsBackend.hh"
#endif
#ifdef WINDOWS
#include "windows/WindowsBackend.hh"
#endif
#ifdef INOTIFY
#include "linux/InotifyBackend.hh"
#endif
#include "Backend.hh"
#include <unordered_map>

static std::unordered_map<std::string, std::shared_ptr<Backend>>& getSharedBackends() {
  static std::unordered_map<std::string, std::shared_ptr<Backend>>* sharedBackends = 
    new std::unordered_map<std::string, std::shared_ptr<Backend>>();
  return *sharedBackends;
}

static std::mutex& getSharedBackendsMutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}

std::shared_ptr<Backend> getBackend(std::string backend) {
  // Use the operating system backend directly. Exact rename correlation is
  // available from inotify and ReadDirectoryChangesW.
  #ifdef FS_EVENTS
    if (backend == "fs-events" || backend == "default") {
      return std::make_shared<FSEventsBackend>();
    }
  #endif
  #ifdef WINDOWS
    if (backend == "windows" || backend == "default") {
      return std::make_shared<WindowsBackend>();
    }
  #endif
  #ifdef INOTIFY
    if (backend == "inotify" || backend == "default") {
      return std::make_shared<InotifyBackend>();
    }
  #endif
  return nullptr;
}

std::shared_ptr<Backend> Backend::getShared(std::string backend) {
  {
    std::unique_lock<std::mutex> lock(getSharedBackendsMutex());
    auto found = getSharedBackends().find(backend);
    if (found != getSharedBackends().end()) {
      found->second->mSharedReservations.fetch_add(1);
      return found->second;
    }
  }

  auto result = getBackend(backend);
  if (!result) {
    return getShared("default");
  }

  result->run();
  std::shared_ptr<Backend> selected;
  {
    std::unique_lock<std::mutex> lock(getSharedBackendsMutex());
    auto inserted = getSharedBackends().emplace(backend, result);
    selected = inserted.first->second;
    selected->mSharedReservations.fetch_add(1);
  }
  return selected;
}

void removeSharedLocked(Backend *backend) {
  for (auto it = getSharedBackends().begin(); it != getSharedBackends().end(); it++) {
    if (it->second.get() == backend) {
      getSharedBackends().erase(it);
      break;
    }
  }

  // Free up memory.
  if (getSharedBackends().size() == 0) {
    getSharedBackends().rehash(0);
  }
}

void removeShared(Backend *backend) {
  std::unique_lock<std::mutex> lock(getSharedBackendsMutex());
  removeSharedLocked(backend);
}

void Backend::run() {
  mThread = std::thread([this] () {
    try {
      start();
    } catch (std::exception &err) {
      // Keep this instance alive while removing it from the shared registry.
      auto self = shared_from_this();
      notifyStartupFailed(err.what());
      handleError(err);
    }
  });

  std::unique_lock<std::mutex> lock(mStartupMutex);
  mStartupCondition.wait(lock, [this] () {
    return mStartupComplete;
  });
  if (!mStartupError.empty()) {
    throw std::runtime_error(mStartupError);
  }
}

void Backend::notifyStarted() {
  std::unique_lock<std::mutex> lock(mStartupMutex);
  if (!mStartupComplete) {
    mStartupComplete = true;
    mStartupCondition.notify_all();
  }
}

void Backend::notifyStartupFailed(const std::string &error) {
  std::unique_lock<std::mutex> lock(mStartupMutex);
  if (!mStartupComplete) {
    mStartupError = error;
    mStartupComplete = true;
    mStartupCondition.notify_all();
  }
}

void Backend::start() {
  notifyStarted();
}

Backend::~Backend() {
  // Wait for thread to stop
  if (mThread.joinable()) {
    // If the backend is being destroyed from the thread itself, detach, otherwise join.
    if (mThread.get_id() == std::this_thread::get_id()) {
      mThread.detach();
    } else {
      mThread.join();
    }
  }
}

void Backend::watch(WatcherRef watcher) {
  std::unique_lock<std::mutex> lock(mMutex);
  mPendingCondition.wait(lock, [this, &watcher]() {
    return mPendingSubscriptions.find(watcher) == mPendingSubscriptions.end();
  });

  auto res = mSubscriptions.find(watcher);
  bool isNew = res == mSubscriptions.end();
  bool needsResubscribe = watcher->mNeedsResubscribe.exchange(false);
  bool wasInvalid = !isNew &&
    (mInvalidSubscriptions.erase(watcher) > 0 || needsResubscribe);
  if (isNew || wasInvalid) {
    mPendingSubscriptions.insert(watcher);
    std::shared_ptr<WatcherState> state;
    try {
      this->subscribe(watcher);
      state = watcher->state;
    } catch (...) {
      mPendingSubscriptions.erase(watcher);
      mPendingCondition.notify_all();
      if (wasInvalid) {
        mInvalidSubscriptions.insert(watcher);
      } else {
        lock.unlock();
        unref();
      }
      throw;
    }

    lock.unlock();
    try {
      this->finishSubscribe(watcher, state);
    } catch (...) {
      lock.lock();
      if (watcher->state == state) {
        watcher->state = nullptr;
      }
      mPendingSubscriptions.erase(watcher);
      mPendingCondition.notify_all();
      if (wasInvalid) {
        mInvalidSubscriptions.insert(watcher);
      } else {
        lock.unlock();
        unref();
      }
      throw;
    }

    lock.lock();
    mPendingSubscriptions.erase(watcher);
    mPendingCondition.notify_all();
    mSubscriptions.insert(watcher);
  }
}

void Backend::unwatch(WatcherRef watcher, bool force) {
  std::unique_lock<std::mutex> lock(mMutex);
  mPendingCondition.wait(lock, [this, &watcher]() {
    return mPendingSubscriptions.find(watcher) == mPendingSubscriptions.end();
  });

  if (!force && watcher->hasCallbacks()) {
    return;
  }
  size_t deleted = mSubscriptions.erase(watcher);
  bool wasInvalid = mInvalidSubscriptions.erase(watcher) > 0;
  if (deleted > 0) {
    auto state = watcher->state;
    try {
      this->unsubscribe(watcher);
    } catch (...) {
      mSubscriptions.insert(watcher);
      if (wasInvalid) {
        mInvalidSubscriptions.insert(watcher);
      }
      throw;
    }
    lock.unlock();
    this->finishUnsubscribe(watcher, state);
    lock.lock();
    watcher->removeBackend(this);
    lock.unlock();
    unref();
  }
}

// This function must be called while holding mMutex.
void Backend::invalidate(WatcherRef watcher) {
  if (mSubscriptions.count(watcher) > 0) {
    mInvalidSubscriptions.insert(watcher);
  }
}

void Backend::unref() {
  std::unique_lock<std::mutex> registryLock(getSharedBackendsMutex());
  std::unique_lock<std::mutex> lock(mMutex);
  if (mSubscriptions.size() == 0 && mPendingSubscriptions.size() == 0 && mSharedReservations.load() == 0) {
    removeSharedLocked(this);
  }
}

void Backend::releaseShared() {
  if (mSharedReservations.fetch_sub(1) == 1) {
    unref();
  }
}

void Backend::handleWatcherError(WatcherError &err) {
  unwatch(err.mWatcher, true);
  err.mWatcher->notifyError(err);
}

void Backend::handleError(std::exception &err) {
  {
    std::unique_lock<std::mutex> lock(mMutex);
    for (auto it = mSubscriptions.begin(); it != mSubscriptions.end(); it++) {
      (*it)->notifyError(err);
    }
    for (auto it = mPendingSubscriptions.begin(); it != mPendingSubscriptions.end(); it++) {
      (*it)->notifyError(err);
    }
    cleanupAfterError();
  }

  removeShared(this);
}
