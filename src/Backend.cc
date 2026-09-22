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

void removeShared(Backend *backend) {
  std::unique_lock<std::mutex> lock(getSharedBackendsMutex());
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

void Backend::run() {
  mThread = std::thread([this] () {
    try {
      start();
    } catch (std::exception &err) {
      handleError(err);
    }
  });

  if (mThread.joinable()) {
    mStartedSignal.wait();
  }
}

void Backend::notifyStarted() {
  mStartedSignal.notify();
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
  auto res = mSubscriptions.find(watcher);
  if (res == mSubscriptions.end()) {
    try {
      this->subscribe(watcher);
      mSubscriptions.insert(watcher);
    } catch (std::exception&) {
      unref();
      throw;
    }
  }
}

void Backend::unwatch(WatcherRef watcher) {
  std::unique_lock<std::mutex> lock(mMutex);
  size_t deleted = mSubscriptions.erase(watcher);
  if (deleted > 0) {
    try {
      this->unsubscribe(watcher);
    } catch (...) {
      mSubscriptions.insert(watcher);
      throw;
    }
    lock.unlock();
    this->finishUnsubscribe(watcher);
    lock.lock();
    watcher->removeBackend(this);
    unref();
  }
}

void Backend::unref() {
  if (mSubscriptions.size() == 0 && mSharedReservations.load() == 0) {
    removeShared(this);
  }
}

void Backend::releaseShared() {
  if (mSharedReservations.fetch_sub(1) == 1) {
    std::unique_lock<std::mutex> lock(mMutex);
    unref();
  }
}

void Backend::handleWatcherError(WatcherError &err) {
  unwatch(err.mWatcher);
  err.mWatcher->notifyError(err);
}

void Backend::handleError(std::exception &err) {
  std::unique_lock<std::mutex> lock(mMutex);
  for (auto it = mSubscriptions.begin(); it != mSubscriptions.end(); it++) {
    (*it)->notifyError(err);
  }

  removeShared(this);
}
