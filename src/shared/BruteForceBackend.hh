#ifndef BRUTE_FORCE_H
#define BRUTE_FORCE_H

#include <stdexcept>
#include "../Backend.hh"
#include "../DirTree.hh"
#include "../Watcher.hh"

class BruteForceBackend : public Backend {
public:
  void subscribe(WatcherRef watcher) override {
    throw std::runtime_error("Brute force backend doesn't support subscriptions.");
  }

  void unsubscribe(WatcherRef watcher) override {
    throw std::runtime_error("Brute force backend doesn't support subscriptions.");
  }

  std::shared_ptr<DirTree> getTree(WatcherRef watcher, bool shouldRead = true);
private:
  void readTree(WatcherRef watcher, std::shared_ptr<DirTree> tree);
};

#endif
