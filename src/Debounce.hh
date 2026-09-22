#ifndef DEBOUNCE_H
#define DEBOUNCE_H

#include <thread>
#include <unordered_map>
#include <functional>
#include "Signal.hh"

#define MIN_WAIT_TIME 50
#define MAX_WAIT_TIME 500

class Debounce {
public:
  static std::shared_ptr<Debounce> getShared();

  Debounce();
  ~Debounce();

  void add(void *key, std::function<void()> cb);
  void remove(void *key);
  void trigger();
  void notify();

private:
  bool mRunning;
  std::mutex mMutex;
  Signal mWaitSignal;
  std::thread mThread;
  std::unordered_map<void *, std::function<void()>> mCallbacks;
  std::chrono::time_point<std::chrono::steady_clock> mLastTime;

  void loop();
  void notifyIfReady();
  void wait();
};

#endif
