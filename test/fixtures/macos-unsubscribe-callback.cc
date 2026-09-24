#include <cassert>
#include <chrono>
#include <thread>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include "Signal.hh"

void testStreamStop(FSEventStreamRef stream);
void testStreamInvalidate(FSEventStreamRef stream);
void testStreamRelease(FSEventStreamRef stream);

#define FSEventStreamStop testStreamStop
#define FSEventStreamInvalidate testStreamInvalidate
#define FSEventStreamRelease testStreamRelease
#include "macos/FSEventsBackend.cc"
#undef FSEventStreamStop
#undef FSEventStreamInvalidate
#undef FSEventStreamRelease
#include "Backend.cc"

struct TestBackend : FSEventsBackend {
  void addSubscription(WatcherRef watcher) {
    std::lock_guard<std::mutex> lock(mMutex);
    mSubscriptions.insert(watcher);
  }
};

static std::shared_ptr<TestBackend> backend;
static Signal callbackEntered;
static Signal allowCallbackLock;
static Signal callbackLocked;
static Signal allowCallbackReturn;
static Signal callbackReturned;
static Signal stopCalled;
static Signal allowStop;
static Signal unwatchEntered;
static Signal unwatchFinished;
static std::atomic<bool> pauseCallback {false};
static std::atomic<bool> blockStop {false};
static std::atomic<size_t> stops {0};
static std::atomic<size_t> invalidates {0};
static std::atomic<size_t> releases {0};

Watcher::Watcher(
  std::string dir,
  std::unordered_set<std::string> ignorePaths,
  std::unordered_set<Glob> ignoreGlobs
) : mDir(std::move(dir)),
    mIgnorePaths(std::move(ignorePaths)),
    mIgnoreGlobs(std::move(ignoreGlobs)) {}

bool Watcher::isIgnored(std::string) {
  return false;
}

bool Watcher::hasCallbacks() {
  unwatchEntered.notify();
  return false;
}

void Watcher::removeBackend(Backend *) {}

void Watcher::notifyError(std::exception &) {}

void Watcher::notify() {
  if (!pauseCallback.load()) return;
  callbackEntered.notify();
  allowCallbackLock.wait();
  {
    std::lock_guard<std::mutex> lock(backend->mMutex);
  }
  callbackLocked.notify();
  allowCallbackReturn.wait();
}

void testStreamStop(FSEventStreamRef) {
  stops.fetch_add(1);
  stopCalled.notify();
  if (blockStop.load()) allowStop.wait();
}

void testStreamInvalidate(FSEventStreamRef) {
  invalidates.fetch_add(1);
}

void testStreamRelease(FSEventStreamRef) {
  releases.fetch_add(1);
}

void runCallback(
  dispatch_queue_t queue,
  WatcherContext *context,
  FSEventStreamRef stream,
  char **paths,
  const FSEventStreamEventFlags *flags,
  const FSEventStreamEventId *ids
) {
  dispatch_async(queue, ^{
    FSEventsCallback(stream, context, 1, paths, flags, ids);
    callbackReturned.notify();
  });
}

void resetSignals() {
  callbackReturned.reset();
  stopCalled.reset();
  unwatchEntered.reset();
  unwatchFinished.reset();
}

int main() {
  backend = std::make_shared<TestBackend>();
  WatcherRef watcher(
    new Watcher("/test", std::unordered_set<std::string> {}, std::unordered_set<Glob> {}),
    [](Watcher *) {}
  );
  dispatch_queue_t queue = dispatch_queue_create(
    "native-watcher.unsubscribe-component", DISPATCH_QUEUE_SERIAL
  );
  auto stream = reinterpret_cast<FSEventStreamRef>(0x1);

  auto state = std::make_shared<State>();
  state->callbackQueue = queue;
  state->stream = stream;
  state->initializing = false;
  watcher->state = state;
  backend->addSubscription(watcher);
  auto *context = new WatcherContext(watcher, state);
  char donePath[] = "/test";
  char *donePaths[] = {donePath};
  FSEventStreamEventFlags doneFlags[] = {kFSEventStreamEventFlagHistoryDone};
  FSEventStreamEventId doneIds[] = {1};

  pauseCallback = true;
  runCallback(queue, context, stream, donePaths, doneFlags, doneIds);
  callbackEntered.wait();
  std::thread unwatcher([&] {
    backend->unwatch(watcher);
    unwatchFinished.notify();
  });
  unwatchEntered.wait();
  stopCalled.wait();

  allowCallbackLock.notify();
  callbackLocked.wait();
  assert(releases.load() == 0);
  assert(unwatchFinished.waitFor(std::chrono::milliseconds(100)) ==
    std::cv_status::timeout);
  allowCallbackReturn.notify();
  callbackReturned.wait();
  unwatchFinished.wait();
  unwatcher.join();
  assert(stops.load() == 1);
  assert(invalidates.load() == 1);
  assert(releases.load() == 1);
  assert(watcher->state == nullptr);
  releaseWatcherContext(context);

  resetSignals();
  pauseCallback = false;
  blockStop = true;
  auto rootState = std::make_shared<State>();
  rootState->callbackQueue = queue;
  rootState->stream = stream;
  rootState->initializing = false;
  watcher->state = rootState;
  backend->addSubscription(watcher);
  auto *rootContext = new WatcherContext(watcher, rootState);
  char rootPath[] = "/test";
  char *rootPaths[] = {rootPath};
  FSEventStreamEventFlags rootFlags[] = {kFSEventStreamEventFlagItemRemoved};
  FSEventStreamEventId rootIds[] = {2};
  runCallback(queue, rootContext, stream, rootPaths, rootFlags, rootIds);
  callbackReturned.wait();
  stopCalled.wait();
  assert(watcher->mNeedsResubscribe.load());

  std::thread rootUnwatcher([&] {
    backend->unwatch(watcher);
    unwatchFinished.notify();
  });
  unwatchEntered.wait();
  assert(unwatchFinished.waitFor(std::chrono::milliseconds(100)) ==
    std::cv_status::timeout);
  auto replacement = std::make_shared<State>();
  {
    std::lock_guard<std::mutex> lock(backend->mMutex);
    watcher->state = replacement;
  }
  allowStop.notify();
  unwatchFinished.wait();
  rootUnwatcher.join();
  assert(stops.load() == 2);
  assert(invalidates.load() == 2);
  assert(releases.load() == 2);
  assert(watcher->state == replacement);
  FSEventsCallback(stream, rootContext, 1, rootPaths, rootFlags, rootIds);
  assert(watcher->state == replacement);
  releaseWatcherContext(rootContext);

  dispatch_release(queue);
}
