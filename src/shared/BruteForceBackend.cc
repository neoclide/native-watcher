#include <string>
#include "../DirTree.hh"
#include "../Event.hh"
#include "./BruteForceBackend.hh"

std::shared_ptr<DirTree> BruteForceBackend::getTree(WatcherRef watcher, bool shouldRead) {
  // Tree contents depend on the watcher's ignore rules, so subscriptions for
  // the same root must not share a cache keyed only by the root path.
  auto tree = std::make_shared<DirTree>(watcher->mDir);

  if (shouldRead) {
    readTree(watcher, tree);
    tree->isComplete = true;
  }

  return tree;
}
