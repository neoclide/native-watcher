#include <atomic>
#include <cassert>
#include "Backend.cc"

// Exercise the real Backend registry and terminal WatcherError path. The
// probe loop mirrors WindowsBackend: it continues after the completion path
// returns and needs an explicit stop to leave the thread.
static Signal fireError;
static Signal loopReturned;
static Signal destroyed;
static Signal stopCalled;
static Signal errorDelivered;
static WatcherRef watched;
static std::atomic<size_t> stops {0};

Watcher::Watcher(
  std::string dir,
  std::unordered_set<std::string> paths,
  std::unordered_set<Glob> globs
) : mDir(std::move(dir)),
    mIgnorePaths(std::move(paths)),
    mIgnoreGlobs(std::move(globs)) {}

Watcher::~Watcher() {}
bool Watcher::hasCallbacks() { return false; }
void Watcher::removeBackend(Backend *) {}
void Watcher::notifyError(std::exception &) { errorDelivered.notify(); }

extern "C" napi_status napi_delete_reference(napi_env, napi_ref) {
  assert(false);
  return napi_ok;
}

class ProbeBackend : public Backend {
public:
  ~ProbeBackend() override {
    destroyed.notify();
  }

  void subscribe(WatcherRef) override {}
  void unsubscribe(WatcherRef) override {}

  void start() override {
    auto self = shared_from_this();
    notifyStarted();
    fireError.wait();
    WatcherError error("terminal directory read error", watched);
    handleWatcherError(error);
    while (mRunning) {
      mLoopWake.wait();
    }
    loopReturned.notify();
  }

protected:
  void stop() {
    mRunning = false;
    stops.fetch_add(1);
    stopCalled.notify();
    mLoopWake.notify();
  }

private:
  std::atomic<bool> mRunning {true};
  Signal mLoopWake;
};

void resetSignals() {
  fireError.reset();
  loopReturned.reset();
  destroyed.reset();
  stopCalled.reset();
  errorDelivered.reset();
}

void runScenario(bool keepAnotherSubscription) {
  resetSignals();
  watched = std::make_shared<Watcher>(
    "/fixture", std::unordered_set<std::string> {}, std::unordered_set<Glob> {}
  );
  auto other = std::make_shared<Watcher>(
    "/other", std::unordered_set<std::string> {}, std::unordered_set<Glob> {}
  );
  auto backend = std::make_shared<ProbeBackend>();
  getSharedBackends().emplace(
    keepAnotherSubscription ? "two-subscriptions" : "last-subscription", backend
  );
  backend->watch(watched);
  if (keepAnotherSubscription) backend->watch(other);
  backend->run();

  size_t before = stops.load();
  if (!keepAnotherSubscription) backend.reset();
  fireError.notify();
  errorDelivered.wait();
  if (keepAnotherSubscription) {
    assert(stops.load() == before);
    backend->unwatch(other, true);
  }
  stopCalled.wait();
  loopReturned.wait();
  if (keepAnotherSubscription) backend.reset();
  destroyed.wait();
}

int main() {
  runScenario(false);
  runScenario(true);
}
