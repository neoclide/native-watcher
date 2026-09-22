#include "Debounce.hh"

std::shared_ptr<Debounce> Debounce::getShared() {
  static std::weak_ptr<Debounce> sharedInstance;
  static std::mutex sharedMutex;
  std::lock_guard<std::mutex> lock(sharedMutex);
  std::shared_ptr<Debounce> shared = sharedInstance.lock();
  if (!shared) {
    shared = std::make_shared<Debounce>();
    sharedInstance = shared;
  }

  return shared;
}

Debounce::Debounce() {
  mRunning = true;
  mThread = std::thread([this] () {
    loop();
  });
}

Debounce::~Debounce() {
  mRunning = false;
  mWaitSignal.notify();
  mThread.join();
}

void Debounce::add(void *key, std::function<void()> cb) {
  std::unique_lock<std::mutex> lock(mMutex);
  mCallbacks.emplace(key, cb);
}

void Debounce::remove(void *key) {
  std::unique_lock<std::mutex> lock(mMutex);
  mCallbacks.erase(key);
}

void Debounce::trigger() {
  std::unique_lock<std::mutex> lock(mMutex);
  mWaitSignal.notify();
}

void Debounce::loop() {
  while (mRunning) {
    mWaitSignal.wait();
    if (!mRunning) {
      break;
    }

    notifyIfReady();
  }
}

void Debounce::notifyIfReady() {
  if (!mRunning) {
    return;
  }

  // If we haven't seen an event in more than the maximum wait time, notify callbacks immediately
  // to ensure that we don't wait forever. Otherwise, wait for the minimum wait time and batch
  // subsequent fast changes. This also means the first file change in a batch is notified immediately,
  // separately from the rest of the batch. This seems like an acceptable tradeoff if the common case
  // is that only a single file was updated at a time.
  auto time = std::chrono::steady_clock::now();
  if ((time - mLastTime) > std::chrono::milliseconds(MAX_WAIT_TIME)) {
    mLastTime = time;
    notify();
  } else {
    wait();
  }
}

void Debounce::wait() {
  auto status = mWaitSignal.waitFor(std::chrono::milliseconds(MIN_WAIT_TIME));
  if (mRunning && (status == std::cv_status::timeout)) {
    notify();
  }
}

void Debounce::notify() {
  std::unique_lock<std::mutex> lock(mMutex);

  mLastTime = std::chrono::steady_clock::now();
  for (auto it = mCallbacks.begin(); it != mCallbacks.end(); it++) {
    auto cb = it->second;
    cb();
  }

  mWaitSignal.reset();
}
