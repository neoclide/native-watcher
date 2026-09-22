#ifndef WINDOWS_H
#define WINDOWS_H

#include <winsock2.h>
#include <windows.h>
#include "../shared/BruteForceBackend.hh"

class WindowsBackend : public BruteForceBackend {
public:
  void start() override;
  ~WindowsBackend();
  void subscribe(WatcherRef watcher) override;
  void unsubscribe(WatcherRef watcher) override;
  void finishUnsubscribe(WatcherRef watcher, std::shared_ptr<WatcherState> state) override;
  bool isBackendThread() const;
private:
  bool mRunning;
};

#endif
