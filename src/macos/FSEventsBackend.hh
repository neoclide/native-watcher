#ifndef FS_EVENTS_H
#define FS_EVENTS_H

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include "../Backend.hh"
#include "../Signal.hh"

class FSEventsBackend : public Backend {
public:
  void start() override;
  ~FSEventsBackend();
  void subscribe(WatcherRef watcher) override;
  void unsubscribe(WatcherRef watcher) override;
  void finishUnsubscribe(
    WatcherRef watcher,
    std::shared_ptr<WatcherState> state
  ) override;
  void cleanupAfterError() override;
  void handleBackendError(std::exception &err);
private:
  void startStream(WatcherRef watcher, FSEventStreamEventId id);
  dispatch_queue_t mQueue = nullptr;
  Signal mStoppedSignal;
};

#endif
