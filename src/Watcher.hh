#ifndef WATCHER_H
#define WATCHER_H

#include <condition_variable>
#include <atomic>
#include <stdexcept>
#include <unordered_set>
#include <set>
#include <node_api.h>
#include "Glob.hh"
#include "Event.hh"
#include "Debounce.hh"
#include "DirTree.hh"
#include "Signal.hh"

struct Watcher;
class Backend;
using WatcherRef = std::shared_ptr<Watcher>;

struct Callback {
  uint64_t id;
  napi_threadsafe_function tsfn;
  Napi::FunctionReference ref;
  napi_env env;
  std::thread::id threadId;
  bool closing;
};

class WatcherState {
public:
    virtual ~WatcherState() = default;
};

struct Watcher : public std::enable_shared_from_this<Watcher> {
  std::string mDir;
  std::unordered_set<std::string> mIgnorePaths;
  std::unordered_set<Glob> mIgnoreGlobs;
  std::vector<Glob> mCompiledGlobs;
  uint64_t mId = 0;
  bool mAllGlobsUseBitmask = false;
  EventList mEvents;
  std::shared_ptr<WatcherState> state;
  std::atomic<bool> mNeedsResubscribe {false};
  std::atomic<bool> mCancelled {false};
  std::atomic<bool> mHasIgnoreAliases {false};

  Watcher(std::string dir, std::unordered_set<std::string> ignorePaths, std::unordered_set<Glob> ignoreGlobs);
  ~Watcher();

  bool operator==(const Watcher &other) const {
    return mDir == other.mDir && mIgnorePaths == other.mIgnorePaths && mIgnoreGlobs == other.mIgnoreGlobs;
  }

  static std::atomic<bool> &processExitingFlag() {
    static std::atomic<bool> exiting {false};
    return exiting;
  }

  static void notifyProcessExiting() {
    processExitingFlag().store(true, std::memory_order_relaxed);
  }

  static bool isProcessExiting() {
    return processExitingFlag().load(std::memory_order_relaxed);
  }

  bool isCancelled() const {
    return isProcessExiting() || mCancelled.load(std::memory_order_relaxed);
  }

  void wait();
  void notify();
  void notifyError(std::exception &err);
  bool watch(Napi::Function callback);
  bool unwatch(Napi::Function callback);
  bool hasCallbacks();
  bool hasCallbacksForEnvironment(napi_env env);
  void addBackend(std::shared_ptr<Backend> backend);
  void removeBackend(Backend *backend);
  void releaseShared();
  void unref();
  bool isIgnored(const std::string &path);
  void destroy();
  void finishErrorCallback(uint64_t callbackId);

  static WatcherRef getShared(std::string dir, std::unordered_set<std::string> ignorePaths, std::unordered_set<Glob> ignoreGlobs);
  static void cancelEnvironment(napi_env env);
  static void cleanupEnvironment(napi_env env);

private:
  std::mutex mMutex;
  std::condition_variable mCond;
  std::vector<Callback> mCallbacks;
  uint64_t mNextCallbackId = 1;
  std::vector<std::weak_ptr<Backend>> mBackends;
  std::shared_ptr<Debounce> mDebounce;
  std::atomic<size_t> mSharedReservations {0};
  std::mutex mIgnoreAliasesMutex;
  std::unordered_set<std::string> mIgnoreAliases;

  std::vector<Callback>::iterator findCallback(Napi::Function callback);
  void clearCallbacks();
  void triggerCallbacks();
};

class WatcherError : public std::runtime_error {
public:
  WatcherRef mWatcher;
  WatcherError(std::string msg, WatcherRef watcher) : std::runtime_error(msg), mWatcher(watcher) {}
  WatcherError(const char *msg, WatcherRef watcher) : std::runtime_error(msg), mWatcher(watcher) {}
};

#endif
