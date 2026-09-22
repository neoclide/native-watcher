#ifndef INOTIFY_H
#define INOTIFY_H

#include <unordered_map>
#include <chrono>
#include <sys/inotify.h>
#include "../shared/BruteForceBackend.hh"
#include "../DirTree.hh"
#include "../Signal.hh"

struct InotifySubscription {
  std::shared_ptr<DirTree> tree;
  std::string path;
  WatcherRef watcher;
};

struct InotifyMoveKey {
  Watcher *watcher;
  uint32_t cookie;

  bool operator==(const InotifyMoveKey &other) const {
    return watcher == other.watcher && cookie == other.cookie;
  }
};

struct InotifyMoveKeyHash {
  size_t operator()(const InotifyMoveKey &key) const {
    return std::hash<Watcher *>()(key.watcher) ^ std::hash<uint32_t>()(key.cookie);
  }
};

struct PendingInotifyMove {
  WatcherRef watcher;
  std::shared_ptr<DirTree> tree;
  std::string path;
  bool isDirectory;
  std::chrono::steady_clock::time_point createdAt;
};

using PendingInotifyMoves = std::unordered_map<InotifyMoveKey, PendingInotifyMove, InotifyMoveKeyHash>;

class InotifyBackend : public BruteForceBackend {
public:
  void start() override;
  ~InotifyBackend();
  void subscribe(WatcherRef watcher) override;
  void unsubscribe(WatcherRef watcher) override;
private:
  int mPipe[2];
  int mInotify;
  std::unordered_multimap<int, std::shared_ptr<InotifySubscription>> mSubscriptions;
  PendingInotifyMoves mPendingMoves;
  Signal mEndedSignal;

  bool watchDir(WatcherRef watcher, std::string path, std::shared_ptr<DirTree> tree);
  bool addCreatedTree(WatcherRef watcher, const std::string &path, std::shared_ptr<DirTree> tree);
  void handleEvents();
  void flushExpiredMoves();
  void moveSubscriptions(Watcher *watcher, const std::string &oldPath, const std::string &newPath);
  void removeSubscriptions(Watcher *watcher, const std::string &path);
  void handleEvent(struct inotify_event *event, std::unordered_set<WatcherRef> &watchers, PendingInotifyMoves &pendingMoves);
  bool handleSubscription(struct inotify_event *event, std::shared_ptr<InotifySubscription> sub, PendingInotifyMoves &pendingMoves);
};

#endif
