#ifndef FS_EVENTS_H
#define FS_EVENTS_H

#include <CoreServices/CoreServices.h>
#include "../Backend.hh"

class FSEventsBackend : public Backend {
public:
  void start() override;
  ~FSEventsBackend();
  void subscribe(WatcherRef watcher) override;
  void unsubscribe(WatcherRef watcher) override;
private:
  void startStream(WatcherRef watcher, FSEventStreamEventId id);
  CFRunLoopRef mRunLoop;
};

#endif
