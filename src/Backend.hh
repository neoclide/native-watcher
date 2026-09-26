#ifndef BACKEND_H
#define BACKEND_H

#include "Event.hh"
#include "Watcher.hh"
#include <atomic>
#include <condition_variable>
#include <thread>

class Backend : public std::enable_shared_from_this<Backend> {
public:
  virtual ~Backend();
  void run();
  void notifyStarted();

  virtual void start();
  virtual void subscribe(WatcherRef watcher) = 0;
  virtual void finishSubscribe(WatcherRef watcher, std::shared_ptr<WatcherState> state) {}
  virtual void unsubscribe(WatcherRef watcher) = 0;
  virtual void finishUnsubscribe(WatcherRef watcher, std::shared_ptr<WatcherState> state) {}
  virtual void cleanupAfterError() {}

  static std::shared_ptr<Backend> getShared(std::string backend);
  void releaseShared();

  void watch(WatcherRef watcher);
  void unwatch(WatcherRef watcher, bool force = false);
  void unref();
  void handleWatcherError(WatcherError &err);
  void invalidate(WatcherRef watcher);

  std::mutex mMutex;
  std::thread mThread;
protected:
  std::unordered_set<WatcherRef> mSubscriptions;
  void handleError(std::exception &err);
  virtual void stop() {}
private:
  std::unordered_set<WatcherRef> mInvalidSubscriptions;
  std::unordered_set<WatcherRef> mPendingSubscriptions;
  std::condition_variable mPendingCondition;
  std::atomic<size_t> mSharedReservations {0};
  std::mutex mStartupMutex;
  std::condition_variable mStartupCondition;
  bool mStartupComplete = false;
  std::string mStartupError;

  void notifyStartupFailed(const std::string &error);
};

#endif
