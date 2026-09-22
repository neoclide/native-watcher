#include "Watcher.hh"
#include "Backend.hh"
#include <unordered_set>

using namespace Napi;

struct WatcherHash {
  std::size_t operator() (WatcherRef const &k) const {
    return std::hash<std::string>()(k->mDir);
  }
};

struct WatcherCompare {
  size_t operator() (WatcherRef const &a, WatcherRef const &b) const {
    return *a == *b;
  }
};

static std::unordered_set<WatcherRef , WatcherHash, WatcherCompare>& getSharedWatchers() {
  static std::unordered_set<WatcherRef , WatcherHash, WatcherCompare>* sharedWatchers = 
    new std::unordered_set<WatcherRef , WatcherHash, WatcherCompare>();
  return *sharedWatchers;
}

static std::mutex& getSharedWatchersMutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}

WatcherRef Watcher::getShared(std::string dir, std::unordered_set<std::string> ignorePaths, std::unordered_set<Glob> ignoreGlobs) {
  WatcherRef watcher = std::make_shared<Watcher>(dir, ignorePaths, ignoreGlobs);
  std::unique_lock<std::mutex> lock(getSharedWatchersMutex());
  auto found = getSharedWatchers().find(watcher);
  if (found != getSharedWatchers().end()) {
    // Keep the registry identity alive until the caller has registered or
    // removed its callback.
    (*found)->mSharedReservations.fetch_add(1);
    return *found;
  }

  watcher->mSharedReservations.fetch_add(1);
  getSharedWatchers().insert(watcher);
  return watcher;
}

void removeSharedLocked(Watcher *watcher) {
  for (auto it = getSharedWatchers().begin(); it != getSharedWatchers().end(); it++) {
    if (it->get() == watcher) {
      getSharedWatchers().erase(it);
      break;
    }
  }

  // Free up memory.
  if (getSharedWatchers().size() == 0) {
    getSharedWatchers().rehash(0);
  }
}

void Watcher::cleanupEnvironment(napi_env env) {
  std::vector<WatcherRef> watchers;
  {
    std::unique_lock<std::mutex> registryLock(getSharedWatchersMutex());
    watchers.assign(getSharedWatchers().begin(), getSharedWatchers().end());
  }

  for (auto &watcher : watchers) {
    std::vector<std::shared_ptr<Backend>> backends;
    bool removedCallback = false;
    bool becameEmpty = false;
    {
      std::unique_lock<std::mutex> lock(watcher->mMutex);
      for (auto it = watcher->mCallbacks.begin(); it != watcher->mCallbacks.end();) {
        if (it->env == env) {
          it->tsfn.Abort();
          it->ref.Unref();
          it = watcher->mCallbacks.erase(it);
          removedCallback = true;
        } else {
          ++it;
        }
      }

      becameEmpty = removedCallback && watcher->mCallbacks.empty();
      if (becameEmpty) {
        for (auto &weakBackend : watcher->mBackends) {
          if (auto backend = weakBackend.lock()) {
            backends.push_back(backend);
          }
        }
      }
    }

    if (becameEmpty) {
      watcher->unref();
      for (auto &backend : backends) {
        backend->unwatch(watcher);
      }
    }
  }
}

Watcher::Watcher(std::string dir, std::unordered_set<std::string> ignorePaths, std::unordered_set<Glob> ignoreGlobs)
  : mDir(dir),
    mIgnorePaths(ignorePaths),
    mIgnoreGlobs(ignoreGlobs) {
      mDebounce = Debounce::getShared();
      mDebounce->add(this, [this] () {
        triggerCallbacks();
      });
    }

Watcher::~Watcher() {
  mDebounce->remove(this);
}

void Watcher::wait() {
  std::unique_lock<std::mutex> lk(mMutex);
  mCond.wait(lk);
}

void Watcher::notify() {
  std::unique_lock<std::mutex> lk(mMutex);
  mCond.notify_all();

  if (mCallbacks.size() > 0 && (mEvents.size() > 0 || mEvents.hasError())) {
    // We must release our lock before calling into the debouncer
    // to avoid a deadlock: the debouncer thread itself will require
    // our lock from its thread when calling into `triggerCallbacks`
    // while holding its own debouncer lock.
    lk.unlock();
    mDebounce->trigger();
  }
}

struct CallbackData {
  std::string error;
  std::vector<Event> events;
  WatcherRef watcher;
  uint64_t callbackId;
  CallbackData(std::string error, std::vector<Event> events)
    : error(error), events(events), callbackId(0) {}
  CallbackData(
    std::string error,
    WatcherRef watcher,
    uint64_t callbackId
  ) : error(error), watcher(watcher), callbackId(callbackId) {}
};

Value callbackEventsToJS(const Env &env, std::vector<Event> &events) {
  EscapableHandleScope scope(env);
  Array arr = Array::New(env, events.size());
  uint32_t currentEventIndex = 0;
  for (auto eventIterator = events.begin(); eventIterator != events.end(); eventIterator++) {
    arr.Set(currentEventIndex++, eventIterator->toJS(env));
  }
  return scope.Escape(arr);
}

void callJSFunction(Napi::Env env, Function jsCallback, CallbackData *data) {
  HandleScope scope(env);
  auto err = data->error.size() > 0 ? Error::New(env, data->error).Value() : env.Null();
  auto events = callbackEventsToJS(env, data->events);
  jsCallback.Call({err, events});
  auto watcher = data->watcher;
  auto callbackId = data->callbackId;
  delete data;

  if (watcher) {
    watcher->finishErrorCallback(callbackId);
  }

  // Throw errors from the callback as fatal exceptions
  // If we don't handle these node segfaults...
  if (env.IsExceptionPending()) {
    Napi::Error err = env.GetAndClearPendingException();
    napi_fatal_exception(env, err.Value());
  }
}

void Watcher::notifyError(std::exception &err) {
  std::unique_lock<std::mutex> lk(mMutex);
  auto watcher = shared_from_this();
  for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
    if (it->closing) {
      continue;
    }

    it->closing = true;
    CallbackData *data = new CallbackData(err.what(), watcher, it->id);
    napi_status status = it->tsfn.BlockingCall(data, callJSFunction);
    if (status != napi_ok) {
      delete data;
    }
  }
}

// This function is called from the debounce thread.
void Watcher::triggerCallbacks() {
  std::unique_lock<std::mutex> lk(mMutex);
  if (mCallbacks.size() > 0) {
    auto batch = mEvents.drain();
    if (batch.events.empty() && batch.error.empty()) {
      return;
    }

    for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
      if (it->closing) {
        continue;
      }

      auto data = new CallbackData(batch.error, batch.events);
      napi_status status = it->tsfn.BlockingCall(data, callJSFunction);
      if (status != napi_ok) {
        delete data;
        if (status == napi_closing) {
          it->closing = true;
        }
      }
    }
  }
}

// This should be called from the JavaScript thread.
bool Watcher::watch(Function callback) {
  std::unique_lock<std::mutex> lk(mMutex);

  auto it = findCallback(callback);
  if (it != mCallbacks.end()) {
    return false;
  }

  auto tsfn = ThreadSafeFunction::New(
    callback.Env(),
    callback,
    "Watcher callback",
    0, // Unlimited queue
    1 // Initial thread count
  );

  mCallbacks.push_back(Callback {
    mNextCallbackId++,
    tsfn,
    Napi::Persistent(callback),
    callback.Env(),
    std::this_thread::get_id(),
    false
  });

  return true;
}

// This is called by a ThreadSafeFunction on the callback's JavaScript thread.
void Watcher::finishErrorCallback(uint64_t callbackId) {
  bool becameEmpty;
  {
    std::unique_lock<std::mutex> lock(mMutex);
    for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
      if (it->id == callbackId) {
        it->tsfn.Release();
        it->ref.Unref();
        mCallbacks.erase(it);
        break;
      }
    }
    becameEmpty = mCallbacks.empty();
  }

  if (becameEmpty) unref();
}

bool Watcher::hasCallbacksForEnvironment(napi_env env) {
  std::unique_lock<std::mutex> lock(mMutex);
  for (auto &callback : mCallbacks) {
    if (callback.env == env) {
      return true;
    }
  }
  return false;
}

bool Watcher::hasCallbacks() {
  std::unique_lock<std::mutex> lock(mMutex);
  return !mCallbacks.empty();
}

void Watcher::addBackend(std::shared_ptr<Backend> backend) {
  std::unique_lock<std::mutex> lock(mMutex);
  for (auto &weakBackend : mBackends) {
    if (auto existing = weakBackend.lock(); existing.get() == backend.get()) {
      return;
    }
  }
  mBackends.push_back(backend);
}

void Watcher::removeBackend(Backend *backend) {
  std::unique_lock<std::mutex> lock(mMutex);
  for (auto it = mBackends.begin(); it != mBackends.end();) {
    auto existing = it->lock();
    if (!existing || existing.get() == backend) {
      it = mBackends.erase(it);
    } else {
      ++it;
    }
  }
}

// This should be called from the JavaScript thread.
std::vector<Callback>::iterator Watcher::findCallback(Function callback) {
  for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
    // Only consider callbacks created by the same thread, or V8 will panic.
    if (it->threadId == std::this_thread::get_id() && it->ref.Value() == callback) {
      return it;
    }
  }

  return mCallbacks.end();
}

// This should be called from the JavaScript thread.
bool Watcher::unwatch(Function callback) {
  bool removed = false;
  bool becameEmpty = false;
  {
    std::unique_lock<std::mutex> lk(mMutex);
    auto it = findCallback(callback);
    if (it != mCallbacks.end()) {
      it->tsfn.Release();
      it->ref.Unref();
      mCallbacks.erase(it);
      removed = true;
    }
    becameEmpty = removed && mCallbacks.empty();
  }

  if (becameEmpty) unref();
  return becameEmpty;
}

void Watcher::releaseShared() {
  if (mSharedReservations.fetch_sub(1) == 1) {
    unref();
  }
}

void Watcher::unref() {
  std::unique_lock<std::mutex> registryLock(getSharedWatchersMutex());
  std::unique_lock<std::mutex> lock(mMutex);
  if (mCallbacks.empty() && mSharedReservations.load() == 0) {
    removeSharedLocked(this);
  }
}

void Watcher::destroy() {
  {
    std::unique_lock<std::mutex> lk(mMutex);
    clearCallbacks();
  }
  unref();
}

// Private because it doesn't lock.
void Watcher::clearCallbacks() {
  for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
    it->tsfn.Release();
    it->ref.Unref();
  }

  mCallbacks.clear();
}

bool Watcher::isIgnored(std::string path) {
  for (auto it = mIgnorePaths.begin(); it != mIgnorePaths.end(); it++) {
    auto dir = *it + DIR_SEP;
    if (*it == path || path.compare(0, dir.size(), dir) == 0) {
      return true;
    }
  }

  auto basePath = mDir + DIR_SEP;

  if (path.rfind(basePath, 0) != 0) {
    return false;
  }

  auto relativePath = path.substr(basePath.size());

  for (auto it = mIgnoreGlobs.begin(); it != mIgnoreGlobs.end(); it++) {
    if (it->isIgnored(relativePath)) {
      return true;
    }
  }

  return false;
}
