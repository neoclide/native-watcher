#include <string>
#include "../DirTree.hh"
#include "../Event.hh"
#include "./BruteForceBackend.hh"

std::shared_ptr<DirTree> BruteForceBackend::getTree(WatcherRef watcher, bool shouldRead) {
  auto tree = DirTree::getCached(watcher->mDir);

  // If the tree is not complete, read it if needed.
  if (!tree->isComplete && shouldRead) {
    readTree(watcher, tree);
    tree->isComplete = true;
  }

  return tree;
}
