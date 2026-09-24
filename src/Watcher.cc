#ifdef FS_EVENTS
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "Watcher.hh"
#include "Backend.hh"
#include <filesystem>
#include <unordered_set>
#ifdef WINDOWS
#include "windows/win_utils.hh"
#endif

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
  try {
    watcher->mDebounce->add(watcher.get(), [watcher] () {
      watcher->triggerCallbacks();
    });
  } catch (...) {
    getSharedWatchers().erase(watcher);
    throw;
  }
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
          napi_release_threadsafe_function(it->tsfn, napi_tsfn_abort);
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

void callJSFunction(napi_env rawEnv, napi_value rawCallback, void *, void *rawData) {
  auto *data = static_cast<CallbackData *>(rawData);
  if (rawEnv == nullptr) {
    delete data;
    return;
  }
  Napi::Env env(rawEnv);
  Function jsCallback(env, rawCallback);
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
    napi_status status = napi_call_threadsafe_function(it->tsfn, data, napi_tsfn_blocking);
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
      napi_status status = napi_call_threadsafe_function(it->tsfn, data, napi_tsfn_blocking);
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

  napi_threadsafe_function tsfn;
  napi_status status = napi_create_threadsafe_function(
    callback.Env(), callback, nullptr,
    String::New(callback.Env(), "Watcher callback"),
    0, 1, nullptr, nullptr, nullptr, callJSFunction, &tsfn
  );
  if (status != napi_ok) {
    throw Error::New(callback.Env(), "Unable to create watcher callback");
  }

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
        napi_release_threadsafe_function(it->tsfn, napi_tsfn_release);
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
      napi_release_threadsafe_function(it->tsfn, napi_tsfn_release);
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
    lock.unlock();
    // Release the Debounce callback's shared ownership before the registry's.
    mDebounce->remove(this);
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
    napi_release_threadsafe_function(it->tsfn, napi_tsfn_release);
    it->ref.Unref();
  }

  mCallbacks.clear();
}

namespace {

bool isPathOrDescendant(const std::string &path, const std::string &base) {
  return path == base ||
    (path.size() > base.size() &&
     path.compare(0, base.size(), base) == 0 &&
     path[base.size()] == DIR_SEP[0]);
}

bool equalIgnoringCase(const std::string &left, const std::string &right) {
#ifdef FS_EVENTS
  CFStringRef a = CFStringCreateWithBytes(
    nullptr, reinterpret_cast<const UInt8 *>(left.data()), left.size(),
    kCFStringEncodingUTF8, false
  );
  CFStringRef b = CFStringCreateWithBytes(
    nullptr, reinterpret_cast<const UInt8 *>(right.data()), right.size(),
    kCFStringEncodingUTF8, false
  );
  bool equal = a != nullptr && b != nullptr &&
    CFStringCompare(a, b, kCFCompareCaseInsensitive | kCFCompareNonliteral) ==
      kCFCompareEqualTo;
  if (a != nullptr) CFRelease(a);
  if (b != nullptr) CFRelease(b);
  return equal;
#elif defined(WINDOWS)
  auto a = utf8ToUtf16(left);
  auto b = utf8ToUtf16(right);
  return CompareStringOrdinal(
    a.c_str(), static_cast<int>(a.size()),
    b.c_str(), static_cast<int>(b.size()), TRUE
  ) == CSTR_EQUAL;
#else
  return left == right;
#endif
}

bool refersToSameEntry(const std::string &left, const std::string &right) {
  std::error_code error;
  auto a = std::filesystem::u8path(left);
  auto b = std::filesystem::u8path(right);
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(a, error)) ||
      error) return false;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(b, error)) ||
      error) return false;
  bool equivalent = std::filesystem::equivalent(a, b, error);
  if (error || !equivalent) return false;
#ifdef FS_EVENTS
  // A case-sensitive volume can contain two hard links whose names differ
  // only by case. Equal file identity alone must not turn them into aliases.
  auto canonicalA = std::filesystem::canonical(a, error);
  if (error) return false;
  auto canonicalB = std::filesystem::canonical(b, error);
  return !error && canonicalA == canonicalB;
#else
  return true;
#endif
}

} // namespace

bool Watcher::isIgnored(std::string path) {
  for (const auto &ignored : mIgnorePaths) {
    if (isPathOrDescendant(path, ignored)) return true;
  }
  {
    std::lock_guard<std::mutex> lock(mIgnoreAliasesMutex);
    for (const auto &alias : mIgnoreAliases) {
      if (isPathOrDescendant(path, alias)) return true;
    }
  }

#if defined(FS_EVENTS) || defined(WINDOWS)
  // Compare complete path components before asking the filesystem whether
  // differently cased spellings really identify the same entry. This keeps
  // distinct names distinct on case-sensitive volumes.
  for (const auto &ignored : mIgnorePaths) {
    std::string ancestor = path;
    while (!ancestor.empty()) {
      if (equalIgnoringCase(ancestor, ignored) &&
          refersToSameEntry(ancestor, ignored)) {
        std::lock_guard<std::mutex> lock(mIgnoreAliasesMutex);
        mIgnoreAliases.insert(ancestor);
        return true;
      }
      size_t separator = ancestor.find_last_of(DIR_SEP);
      if (separator == std::string::npos) break;
      ancestor.resize(separator);
    }
  }
#endif

  auto basePath = mDir + DIR_SEP;

  if (path.rfind(basePath, 0) != 0) {
    return false;
  }

  auto relativePath = path.substr(basePath.size());

  for (auto it = mIgnoreGlobs.begin(); it != mIgnoreGlobs.end(); it++) {
    if (it->isIgnored(relativePath)) {
      return true;
    }
    for (size_t end = relativePath.find(DIR_SEP); end != std::string::npos;
         end = relativePath.find(DIR_SEP, end + 1)) {
      if (it->isIgnored(relativePath.substr(0, end))) {
        return true;
      }
    }
  }

  return false;
}
