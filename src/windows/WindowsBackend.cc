#include <string>
#include <stack>
#include <cwchar>
#include <atomic>
#include <cstddef>
#include <unordered_set>
#include "../DirTree.hh"
#include "../shared/BruteForceBackend.hh"
#include "./WindowsBackend.hh"
#include "./win_utils.hh"

#define DEFAULT_BUF_SIZE 1024 * 1024
#define NETWORK_BUF_SIZE 64 * 1024
#define CONVERT_TIME(ft) ULARGE_INTEGER{ft.dwLowDateTime, ft.dwHighDateTime}.QuadPart

void BruteForceBackend::readTree(WatcherRef watcher, std::shared_ptr<DirTree> tree) {
  std::stack<std::string> directories;

  directories.push(watcher->mDir);

  while (!directories.empty()) {
    HANDLE hFind = INVALID_HANDLE_VALUE;

    std::string path = directories.top();
    directories.pop();

    WIN32_FIND_DATAW ffd;
    hFind = FindFirstFileW(utf8ToUtf16(path + "\\*").data(), &ffd);

    if (hFind == INVALID_HANDLE_VALUE)  {
      if (path == watcher->mDir) {
        throw WatcherError("Error opening directory", watcher);
      }

      tree->remove(path);
      continue;
    }

    do {
      std::string name = utf16ToUtf8(
        ffd.cFileName,
        static_cast<DWORD>(wcslen(ffd.cFileName))
      );
      if (name != "." && name != "..") {
        std::string fullPath = path + "\\" + name;
        if (watcher->isIgnored(fullPath)) {
          continue;
        }
        if (ffd.dwFileAttributes &
            (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
          continue;
        }

        tree->add(fullPath, CONVERT_TIME(ffd.ftLastWriteTime), ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (
          (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          !(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        ) {
          directories.push(fullPath);
        }
      }
    } while (FindNextFileW(hFind, &ffd) != 0);

    FindClose(hFind);
  }
}

void WindowsBackend::start() {
  mRunning = true;
  notifyStarted();

  while (mRunning) {
    SleepEx(INFINITE, true);
  }
}

WindowsBackend::~WindowsBackend() {
  // Mark as stopped, and queue a noop function in the thread to break the loop
  mRunning = false;
  QueueUserAPC([](__in ULONG_PTR) {}, mThread.native_handle(), (ULONG_PTR)this);
}

class Subscription:
  public WatcherState,
  public std::enable_shared_from_this<Subscription> {
public:
  Subscription(WindowsBackend *backend, WatcherRef watcher, std::shared_ptr<DirTree> tree) {
    mRunning = true;
    mPollPending = false;
    mBackend = backend;
    mWatcher = watcher;
    mTree = tree;
    ZeroMemory(&mOverlapped, sizeof(OVERLAPPED));
    mOverlapped.hEvent = this;
    mReadBuffer.resize(DEFAULT_BUF_SIZE);
    mWriteBuffer.resize(DEFAULT_BUF_SIZE);

    mDirectoryHandle = CreateFileW(
      utf8ToUtf16(watcher->mDir).data(),
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      NULL
    );

    if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
      throw WatcherError("Invalid handle", mWatcher);
    }

    // Ensure that the path is a directory
    BY_HANDLE_FILE_INFORMATION info;
    bool success = GetFileInformationByHandle(
      mDirectoryHandle,
      &info
    );

    if (!success) {
      CloseHandle(mDirectoryHandle);
      mDirectoryHandle = INVALID_HANDLE_VALUE;
      throw WatcherError("Could not get file information", mWatcher);
    }

    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      CloseHandle(mDirectoryHandle);
      mDirectoryHandle = INVALID_HANDLE_VALUE;
      throw WatcherError("Not a directory", mWatcher);
    }
  }

  virtual ~Subscription() {
    if (mDirectoryHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(mDirectoryHandle);
    }
  }

  void run() {
    try {
      poll();
      mBackend->readTree(mWatcher, mTree);
      mTree->isComplete = true;
      mStartSignal.notify();
    } catch (WatcherError &err) {
      if (mPollPending) {
        CancelIoEx(mDirectoryHandle, &mOverlapped);
      }
      mStartError = err.what();
      mStartSignal.notify();
    } catch (std::exception &err) {
      if (mPollPending) {
        CancelIoEx(mDirectoryHandle, &mOverlapped);
      }
      mStartError = err.what();
      mStartSignal.notify();
    }
  }

  void waitForStart() {
    mStartSignal.wait();
    if (!mStartError.empty()) {
      throw WatcherError(mStartError, mWatcher);
    }
  }

  void requestStop() {
    if (mStopRequested.exchange(true)) {
      return;
    }

    if (mBackend->isBackendThread()) {
      beginStop();
      return;
    }

    bool success = QueueUserAPC([](__in ULONG_PTR ptr) {
      auto sub = reinterpret_cast<Subscription *>(ptr);
      auto keepAlive = sub->shared_from_this();
      sub->beginStop();
    }, mBackend->mThread.native_handle(), (ULONG_PTR)this);
    if (!success) {
      mStopRequested = false;
      throw std::runtime_error("Unable to queue subscription stop");
    }
  }

  void waitForStop() {
    if (!mStopped) {
      mStoppedSignal.wait();
    }
  }

  void poll() {
    if (!mRunning) {
      return;
    }

    // Asynchronously wait for changes.
    int success = ReadDirectoryChangesW(
      mDirectoryHandle,
      mWriteBuffer.data(),
      static_cast<DWORD>(mWriteBuffer.size()),
      TRUE, // recursive
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES
        | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
      NULL,
      &mOverlapped,
      [](DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
        auto subscription = reinterpret_cast<Subscription *>(overlapped->hEvent);
        auto keepAlive = subscription->shared_from_this();
        subscription->mPollPending = false;
        try {
          subscription->processEvents(errorCode, numBytes);
        } catch (WatcherError &err) {
          subscription->mBackend->handleWatcherError(err);
        }
      }
    );

    if (!success) {
      throw WatcherError("Failed to read changes", mWatcher);
    }
    mPollPending = true;
  }

  void processEvents(DWORD errorCode, DWORD numBytes) {
    if (mStopRequested) {
      finishStop();
      return;
    }

    switch (errorCode) {
      case ERROR_OPERATION_ABORTED:
        return;
      case ERROR_INVALID_PARAMETER:
        if (mReadBuffer.size() <= NETWORK_BUF_SIZE) {
          throw WatcherError("Invalid parameter watching directory", mWatcher);
        }
        // resize buffers to network size (64kb), and try again
        mReadBuffer.resize(NETWORK_BUF_SIZE);
        mWriteBuffer.resize(NETWORK_BUF_SIZE);
        poll();
        return;
      case ERROR_NOTIFY_ENUM_DIR:
        failOverflow();
        return;
      case ERROR_ACCESS_DENIED: {
        // This can happen if the watched directory is deleted. Check if that is the case,
        // and if so emit a delete event. Otherwise, fall through to default error case.
        DWORD attrs = GetFileAttributesW(utf8ToUtf16(mWatcher->mDir).data());
        bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        if (!isDir) {
          removePath(mWatcher->mDir);
          mWatcher->notify();
          invalidateCurrent();
          requestStop();
          return;
        }
      }
      default:
        if (errorCode != ERROR_SUCCESS) {
          throw WatcherError("Unknown error", mWatcher);
        }
    }

    if (numBytes == 0) {
      failOverflow();
      return;
    }
    if (numBytes > mWriteBuffer.size()) {
      failOverflow();
      return;
    }

    // Swap read and write buffers, and poll again
    std::swap(mWriteBuffer, mReadBuffer);
    poll();

    // Read change events
    mPreviousRemovedPaths = std::move(mRemovedPaths);
    mRemovedPaths.clear();
    BYTE *base = mReadBuffer.data();
    BYTE *end = base + numBytes;
    while (base < end) {
      size_t remaining = static_cast<size_t>(end - base);
      if (remaining < offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
        failOverflow();
        return;
      }

      PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)base;
      size_t recordSize = offsetof(FILE_NOTIFY_INFORMATION, FileName) +
        info->FileNameLength;
      if (
        info->FileNameLength % sizeof(WCHAR) != 0 ||
        recordSize > remaining
      ) {
        failOverflow();
        return;
      }
      processEvent(info);

      if (info->NextEntryOffset == 0) {
        break;
      }

      if (
        info->NextEntryOffset < recordSize ||
        info->NextEntryOffset >= remaining
      ) {
        failOverflow();
        return;
      }

      base += info->NextEntryOffset;
    }

    flushPendingRename();
    mPreviousRemovedPaths.clear();

    mWatcher->notify();
  }

  void processEvent(PFILE_NOTIFY_INFORMATION info) {
    std::string path = mWatcher->mDir + "\\" + utf16ToUtf8(info->FileName, info->FileNameLength / sizeof(WCHAR));
    if (mWatcher->isIgnored(path)) {
      flushPendingRename();
      return;
    }

    switch (info->Action) {
      case FILE_ACTION_ADDED: {
        flushPendingRename();
        mRemovedPaths.erase(path);
        mPreviousRemovedPaths.erase(path);
        addPath(path);
        break;
      }
      case FILE_ACTION_RENAMED_NEW_NAME: {
        WIN32_FILE_ATTRIBUTE_DATA data;
        bool hasAttributes = GetFileAttributesExW(
          utf8ToUtf16(path).data(),
          GetFileExInfoStandard,
          &data
        );
        bool supported = !hasAttributes || !(data.dwFileAttributes &
          (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE));
        // A later rename may already have removed this intermediate path.
        // The paired notification still provides enough information to move
        // the known tree; attributes only refresh its current root entry.
        if (mPendingRenamePath.has_value()) {
          bool targetExisted = mTree->find(path).has_value() ||
            mRemovedPaths.erase(path) > 0 ||
            mPreviousRemovedPaths.erase(path) > 0;
          std::string oldPath = *mPendingRenamePath;
          auto moved = mTree->extract(oldPath);
          if (targetExisted) mTree->remove(path);
          std::string renameId =
            "windows:" + std::to_string(++mRenameSequence);
          for (const auto &entry : moved) {
            std::string newPath = path + entry.path.substr(oldPath.size());
            EntryKind kind = entryKind(entry.isDir);
            if (targetExisted || !supported || mWatcher->isIgnored(newPath)) {
              mWatcher->mEvents.remove(entry.path, kind);
              if (supported && !mWatcher->isIgnored(newPath)) {
                mWatcher->mEvents.create(newPath, kind);
              }
            } else {
              mWatcher->mEvents.rename(
                entry.path,
                newPath,
                renameId + ":" + entry.path.substr(oldPath.size()),
                kind
              );
            }
          }
          if (supported) {
            std::vector<DirEntry> visible;
            for (const auto &entry : moved) {
              if (!mWatcher->isIgnored(
                path + entry.path.substr(oldPath.size())
              )) visible.push_back(entry);
            }
            mTree->restore(std::move(visible), oldPath, path);
          }
          if (hasAttributes && supported &&
              !mTree->update(path, CONVERT_TIME(data.ftLastWriteTime))) {
            mTree->add(
              path,
              CONVERT_TIME(data.ftLastWriteTime),
              data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY
            );
          }
          // A preceding removal may already have consumed the source tree.
          // Rebuild all descendants after clearing the apparent target above.
          if (hasAttributes && supported &&
              (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
              (moved.empty() || !mWatcher->mIgnorePaths.empty() ||
               !mWatcher->mIgnoreGlobs.empty())) {
            addPath(path);
          }
          mPendingRenamePath.reset();
        } else if (hasAttributes && supported) {
          addPath(path);
        }
        break;
      }
      case FILE_ACTION_MODIFIED: {
        flushPendingRename();
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(utf8ToUtf16(path).data(), GetFileExInfoStandard, &data) &&
            !(data.dwFileAttributes &
              (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
            mTree->find(path)) {
          mTree->update(path, CONVERT_TIME(data.ftLastWriteTime));
          if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            mWatcher->mEvents.update(path, EntryKind::File);
          }
        }
        break;
      }
      case FILE_ACTION_REMOVED: {
        flushPendingRename();
        mRemovedPaths.insert(path);
        removePath(path);
        // NTFS can report a removal before the case-only rename pair. Recover
        // the on-disk spelling, including when no paired notification follows.
        WIN32_FIND_DATAW data;
        HANDLE search = FindFirstFileW(utf8ToUtf16(path).c_str(), &data);
        if (search != INVALID_HANDLE_VALUE) {
          FindClose(search);
          size_t separator = path.find_last_of('\\');
          std::string name = utf16ToUtf8(
            data.cFileName, static_cast<DWORD>(wcslen(data.cFileName))
          );
          auto oldName = utf8ToUtf16(path.substr(separator + 1));
          if (name != path.substr(separator + 1) &&
              CompareStringOrdinal(oldName.c_str(), -1, data.cFileName, -1,
                                   TRUE) == CSTR_EQUAL) {
            addPath(path.substr(0, separator + 1) + name);
          }
        }
        break;
      }
      case FILE_ACTION_RENAMED_OLD_NAME:
        flushPendingRename();
        mPendingRenamePath = path;
        break;
    }
  }

  void addPath(const std::string &path) {
    std::stack<std::string> pending;
    pending.push(path);

    while (!pending.empty()) {
      std::string candidate = pending.top();
      pending.pop();
      if (mWatcher->isIgnored(candidate)) continue;

      WIN32_FILE_ATTRIBUTE_DATA data;
      if (!GetFileAttributesExW(
        utf8ToUtf16(candidate).data(),
        GetFileExInfoStandard,
        &data
      )) {
        continue;
      }
      if (data.dwFileAttributes &
          (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) continue;

      bool isDirectory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
      if (!mTree->find(candidate)) {
        mWatcher->mEvents.create(candidate, entryKind(isDirectory));
        mTree->add(candidate, CONVERT_TIME(data.ftLastWriteTime), isDirectory);
      }
      if (!isDirectory || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        continue;
      }

      WIN32_FIND_DATAW entry;
      HANDLE search = FindFirstFileW(
        utf8ToUtf16(candidate + "\\*").data(),
        &entry
      );
      if (search == INVALID_HANDLE_VALUE) continue;
      do {
        std::string name = utf16ToUtf8(
          entry.cFileName,
          static_cast<DWORD>(wcslen(entry.cFileName))
        );
        if (name != "." && name != "..") {
          pending.push(candidate + "\\" + name);
        }
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
  }

  void flushPendingRename() {
    if (mPendingRenamePath.has_value()) {
      removePath(*mPendingRenamePath);
      mPendingRenamePath.reset();
    }
  }

  void removePath(const std::string &path) {
    for (const auto &entry : mTree->extract(path)) {
      mWatcher->mEvents.remove(entry.path, entryKind(entry.isDir));
    }
  }

private:
  void invalidateCurrent() {
    std::lock_guard<std::mutex> lock(mBackend->mMutex);
    mBackend->invalidate(mWatcher);
  }

  void failOverflow() {
    mOverflowed = true;
    mWatcher->mEvents.error(
      "ReadDirectoryChangesW buffer overflow. The subscription can no "
      "longer guarantee complete filesystem events."
    );
    invalidateCurrent();
    requestStop();
  }

  void beginStop() {
    mRunning = false;
    if (mPollPending) {
      CancelIoEx(mDirectoryHandle, &mOverlapped);
    } else {
      finishStop();
    }
  }

  void finishStop() {
    if (mStopped.exchange(true)) {
      return;
    }
    if (mDirectoryHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(mDirectoryHandle);
      mDirectoryHandle = INVALID_HANDLE_VALUE;
    }
    if (mOverflowed) mWatcher->notify();
    mStoppedSignal.notify();
  }

  WindowsBackend *mBackend;
  std::shared_ptr<Watcher> mWatcher;
  std::shared_ptr<DirTree> mTree;
  bool mRunning;
  bool mPollPending;
  std::atomic<bool> mStopRequested {false};
  std::atomic<bool> mStopped {false};
  bool mOverflowed = false;
  Signal mStartSignal;
  std::string mStartError;
  Signal mStoppedSignal;
  std::optional<std::string> mPendingRenamePath;
  std::unordered_set<std::string> mRemovedPaths;
  std::unordered_set<std::string> mPreviousRemovedPaths;
  uint64_t mRenameSequence = 0;
  HANDLE mDirectoryHandle;
  std::vector<BYTE> mReadBuffer;
  std::vector<BYTE> mWriteBuffer;
  OVERLAPPED mOverlapped;
};

// This function is called by Backend::watch which takes a lock on mMutex
void WindowsBackend::subscribe(WatcherRef watcher) {
  // Create a subscription for this watcher
  auto sub = std::make_shared<Subscription>(this, watcher, getTree(watcher, false));
  watcher->state = sub;

  // Queue polling for this subscription in the correct thread.
  bool success = QueueUserAPC([](__in ULONG_PTR ptr) {
    auto sub = reinterpret_cast<Subscription *>(ptr);
    auto keepAlive = sub->shared_from_this();
    sub->run();
  }, mThread.native_handle(), (ULONG_PTR)sub.get());

  if (!success) {
    watcher->state = nullptr;
    throw std::runtime_error("Unable to queue APC");
  }
  try {
    sub->waitForStart();
  } catch (...) {
    watcher->state = nullptr;
    throw;
  }
}

// Start cancellation while Backend::unwatch still retains the watcher state.
void WindowsBackend::unsubscribe(WatcherRef watcher) {
  auto sub = std::static_pointer_cast<Subscription>(watcher->state);
  if (sub != nullptr) {
    sub->requestStop();
  }
}

// Wait outside the Backend lock so the completion callback can report errors.
void WindowsBackend::finishUnsubscribe(WatcherRef watcher, std::shared_ptr<WatcherState> state) {
  auto sub = std::static_pointer_cast<Subscription>(state);
  if (sub != nullptr) {
    sub->waitForStop();
  }
  std::lock_guard<std::mutex> lock(mMutex);
  if (watcher->state == state) watcher->state = nullptr;
}

bool WindowsBackend::isBackendThread() const {
  return mThread.get_id() == std::this_thread::get_id();
}
