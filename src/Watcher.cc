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

static std::atomic<uint64_t>& getNextWatcherId() {
  static std::atomic<uint64_t> nextId {1};
  return nextId;
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

void Watcher::cancelEnvironment(napi_env env) {
  std::vector<WatcherRef> watchers;
  {
    std::unique_lock<std::mutex> registryLock(getSharedWatchersMutex());
    watchers.assign(getSharedWatchers().begin(), getSharedWatchers().end());
  }

  for (auto &watcher : watchers) {
    std::unique_lock<std::mutex> lock(watcher->mMutex);
    bool hasThisEnv = false;
    bool hasOtherEnv = false;
    for (const auto &cb : watcher->mCallbacks) {
      if (cb.env == env) {
        hasThisEnv = true;
      } else {
        hasOtherEnv = true;
      }
    }
    if (hasThisEnv && !hasOtherEnv) {
      watcher->mCancelled.store(true, std::memory_order_relaxed);
    }
  }
}

void Watcher::cleanupEnvironment(napi_env env) {
  std::vector<WatcherRef> watchers;
  {
    std::unique_lock<std::mutex> registryLock(getSharedWatchersMutex());
    watchers.assign(getSharedWatchers().begin(), getSharedWatchers().end());
  }

  struct PendingUnwatch {
    WatcherRef watcher;
    std::vector<std::shared_ptr<Backend>> backends;
  };
  std::vector<PendingUnwatch> toUnwatch;

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
        watcher->mCancelled.store(true, std::memory_order_relaxed);
        for (auto &weakBackend : watcher->mBackends) {
          if (auto backend = weakBackend.lock()) {
            backends.push_back(backend);
          }
        }
      }
    }

    if (becameEmpty) {
      toUnwatch.push_back(PendingUnwatch {watcher, std::move(backends)});
    }
  }

  for (auto &item : toUnwatch) {
    item.watcher->unref();
    for (auto &backend : item.backends) {
      backend->unwatch(item.watcher);
    }
  }
}

Watcher::Watcher(std::string dir, std::unordered_set<std::string> ignorePaths, std::unordered_set<Glob> ignoreGlobs)
  : mDir(dir),
    mIgnorePaths(ignorePaths),
    mIgnoreGlobs(ignoreGlobs),
    mId(getNextWatcherId().fetch_add(1, std::memory_order_relaxed)) {
  mDebounce = Debounce::getShared();
  std::unordered_set<Glob> deduplicated;
  for (const auto &glob : mIgnoreGlobs) {
    if (glob.mRaw.empty()) continue;
    std::string normalized = glob.mRaw;
    while (normalized.size() > 3 &&
           (normalized.compare(normalized.size() - 3, 3, "/**") == 0 ||
            normalized.compare(normalized.size() - 3, 3, "\\**") == 0)) {
      normalized.resize(normalized.size() - 3);
    }
    deduplicated.emplace(std::move(normalized));
  }
  mCompiledGlobs.assign(deduplicated.begin(), deduplicated.end());
  mAllGlobsUseBitmask = true;
  for (const auto &glob : mCompiledGlobs) {
    if (!glob.mUseBitmask) {
      mAllGlobsUseBitmask = false;
      break;
    }
  }
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
    napi_status status = napi_call_threadsafe_function(it->tsfn, data, napi_tsfn_nonblocking);
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
      napi_status status = napi_call_threadsafe_function(it->tsfn, data, napi_tsfn_nonblocking);
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

  mCancelled.store(false, std::memory_order_relaxed);

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
    if (becameEmpty) {
      mCancelled.store(true, std::memory_order_relaxed);
    }
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
    if (becameEmpty) {
      mCancelled.store(true, std::memory_order_relaxed);
    }
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
  mCancelled.store(true, std::memory_order_relaxed);
}

namespace {

bool isPathOrDescendant(const std::string &path, const std::string &base) {
  return path == base ||
    (path.size() > base.size() &&
     path.compare(0, base.size(), base) == 0 &&
     path[base.size()] == DIR_SEP[0]);
}

#if defined(FS_EVENTS) || defined(WINDOWS)
bool isAsciiView(std::string_view value) {
  for (unsigned char ch : value) {
    if (ch >= 0x80) return false;
  }
  return true;
}

bool equalIgnoringCase(std::string_view left, std::string_view right) {
  if (isAsciiView(left) && isAsciiView(right)) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
      unsigned char a = static_cast<unsigned char>(left[i]);
      unsigned char b = static_cast<unsigned char>(right[i]);
      if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
      if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
      if (a != b) return false;
    }
    return true;
  }
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
  auto a = utf8ToUtf16(std::string(left));
  auto b = utf8ToUtf16(std::string(right));
  return CompareStringOrdinal(
    a.c_str(), static_cast<int>(a.size()),
    b.c_str(), static_cast<int>(b.size()), TRUE
  ) == CSTR_EQUAL;
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
#endif

struct DirGlobState {
  static constexpr size_t kInlineGlobs = 8;
  bool ignored = false;
  bool allDead = false;
  uint64_t inlineStates[kInlineGlobs] {};
  std::vector<uint64_t> overflowStates;

  void init(size_t count) {
    ignored = false;
    allDead = false;
    if (count > kInlineGlobs) {
      overflowStates.assign(count, 0);
    }
  }

  uint64_t *data(size_t count) {
    return count <= kInlineGlobs ? inlineStates : overflowStates.data();
  }

  const uint64_t *data(size_t count) const {
    return count <= kInlineGlobs ? inlineStates : overflowStates.data();
  }
};

DirGlobState makeRootGlobState(const Watcher &watcher) {
  DirGlobState root;
  const size_t count = watcher.mCompiledGlobs.size();
  root.init(count);
  uint64_t *states = root.data(count);
  bool anyAlive = false;
  for (size_t i = 0; i < count; ++i) {
    const Glob &glob = watcher.mCompiledGlobs[i];
    states[i] = glob.mInitialState;
    if ((states[i] & glob.mAcceptBit) != 0) {
      root.ignored = true;
      return root;
    }
    if (states[i] != 0) anyAlive = true;
  }
  root.allDead = !anyAlive;
  return root;
}

DirGlobState stepDirGlobState(
  const Watcher &watcher,
  const DirGlobState &parentState,
  std::string_view component,
  std::string_view relParent
) {
  DirGlobState next;
  if (parentState.ignored) {
    next.ignored = true;
    return next;
  }
  if (parentState.allDead) {
    next.allDead = true;
    return next;
  }
  const size_t count = watcher.mCompiledGlobs.size();
  next.init(count);
  const uint64_t *inStates = parentState.data(count);
  uint64_t *outStates = next.data(count);
  std::optional<DirectoryCaseSensitivity> sensitivity;
  bool anyAlive = false;
  for (size_t i = 0; i < count; ++i) {
    uint64_t s = inStates[i];
    if (s == 0) {
      outStates[i] = 0;
      continue;
    }
    const Glob &glob = watcher.mCompiledGlobs[i];
    uint64_t stepped = glob.stepBitmask(
      s, component, watcher.mDir, relParent, sensitivity
    );
    if ((stepped & glob.mAcceptBit) != 0) {
      next.ignored = true;
      return next;
    }
    outStates[i] = stepped;
    if (stepped != 0) anyAlive = true;
  }
  next.allDead = !anyAlive;
  return next;
}

bool isComponentIgnored(
  const Watcher &watcher,
  const DirGlobState &parentState,
  std::string_view component,
  std::string_view relParent
) {
  if (parentState.ignored) return true;
  if (parentState.allDead) return false;
  const size_t count = watcher.mCompiledGlobs.size();
  const uint64_t *inStates = parentState.data(count);
  std::optional<DirectoryCaseSensitivity> sensitivity;
  for (size_t i = 0; i < count; ++i) {
    uint64_t s = inStates[i];
    if (s == 0) continue;
    const Glob &glob = watcher.mCompiledGlobs[i];
    uint64_t stepped = glob.stepBitmask(
      s, component, watcher.mDir, relParent, sensitivity
    );
    if ((stepped & glob.mAcceptBit) != 0) {
      return true;
    }
  }
  return false;
}

struct ThreadIgnoreCache {
  uint64_t watcherId = 0;
  DirGlobState rootState;
  std::string lastRelDir;
  DirGlobState lastState;
  bool hasLast = false;
  std::unordered_map<std::string, DirGlobState> dirs;
};

const DirGlobState &resolveDirGlobState(
  const Watcher &watcher,
  std::string_view relDir
) {
  static thread_local ThreadIgnoreCache cache;
  if (cache.watcherId != watcher.mId) {
    cache.watcherId = watcher.mId;
    cache.rootState = makeRootGlobState(watcher);
    cache.lastRelDir.clear();
    cache.hasLast = false;
    cache.dirs.clear();
  }
  if (relDir.empty()) {
    return cache.rootState;
  }
  if (cache.hasLast && cache.lastRelDir == relDir) {
    return cache.lastState;
  }
  std::string key(relDir);
  auto found = cache.dirs.find(key);
  if (found != cache.dirs.end()) {
    cache.lastRelDir = std::move(key);
    cache.lastState = found->second;
    cache.hasLast = true;
    return cache.lastState;
  }

  if (cache.dirs.size() >= 16384) {
    cache.dirs.clear();
  }

  DirGlobState state = cache.rootState;
  size_t pos = 0;
  size_t searchEnd = relDir.rfind(DIR_SEP[0]);
  while (searchEnd != std::string_view::npos) {
    auto ancestorIt = cache.dirs.find(std::string(relDir.substr(0, searchEnd)));
    if (ancestorIt != cache.dirs.end()) {
      state = ancestorIt->second;
      pos = searchEnd + 1;
      break;
    }
    if (searchEnd == 0) break;
    searchEnd = relDir.rfind(DIR_SEP[0], searchEnd - 1);
  }

  while (pos < relDir.size()) {
    size_t sep = relDir.find(DIR_SEP[0], pos);
    if (sep == std::string_view::npos) sep = relDir.size();
    std::string_view component = relDir.substr(pos, sep - pos);
    std::string_view relParent = pos == 0
      ? std::string_view()
      : relDir.substr(0, pos - 1);
    state = stepDirGlobState(watcher, state, component, relParent);
    cache.dirs.emplace(std::string(relDir.substr(0, sep)), state);
    pos = sep + 1;
  }

  cache.lastRelDir = std::move(key);
  cache.lastState = state;
  cache.hasLast = true;
  return cache.lastState;
}

} // namespace

bool Watcher::isIgnored(const std::string &path) {
  if (!mIgnorePaths.empty()) {
    for (const auto &ignored : mIgnorePaths) {
      if (isPathOrDescendant(path, ignored)) return true;
    }
    if (mHasIgnoreAliases.load(std::memory_order_acquire)) {
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
      std::string_view ancestor(path);
      while (!ancestor.empty()) {
        if (equalIgnoringCase(ancestor, ignored)) {
          std::string ancestorStr(ancestor);
          if (refersToSameEntry(ancestorStr, ignored)) {
            std::lock_guard<std::mutex> lock(mIgnoreAliasesMutex);
            mIgnoreAliases.insert(ancestorStr);
            mHasIgnoreAliases.store(true, std::memory_order_release);
            return true;
          }
        }
        size_t separator = ancestor.find_last_of(DIR_SEP);
        if (separator == std::string_view::npos) break;
        ancestor = ancestor.substr(0, separator);
      }
    }
#endif
  }

  if (mCompiledGlobs.empty()) {
    return false;
  }

  if (path.size() <= mDir.size() ||
      path.compare(0, mDir.size(), mDir) != 0 ||
      path[mDir.size()] != DIR_SEP[0]) {
    return false;
  }

  std::string_view relativePath(
    path.data() + mDir.size() + 1,
    path.size() - mDir.size() - 1
  );
  if (relativePath.empty()) {
    return false;
  }

  if (mAllGlobsUseBitmask && mId != 0) {
    size_t relSep = relativePath.rfind(DIR_SEP[0]);
    std::string_view relDir = relSep == std::string_view::npos
      ? std::string_view()
      : relativePath.substr(0, relSep);
    std::string_view leafName = relSep == std::string_view::npos
      ? relativePath
      : relativePath.substr(relSep + 1);
    const DirGlobState &parentState = resolveDirGlobState(*this, relDir);
    return isComponentIgnored(*this, parentState, leafName, relDir);
  }

  for (const auto &glob : mCompiledGlobs) {
    if (glob.isPathOrAncestorIgnored(relativePath, mDir)) {
      return true;
    }
  }

  return false;
}
