#ifndef EVENT_H
#define EVENT_H

#include <string>
#include <node_api.h>
#include <napi.h>
#include <mutex>
#include <map>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

using namespace Napi;

enum class EntryKind { File, Directory };

inline EntryKind entryKind(bool isDirectory) {
  return isDirectory ? EntryKind::Directory : EntryKind::File;
}

struct Event {
  std::string path;
  EntryKind kind;
  bool isCreated;
  bool isDeleted;
  std::optional<std::string> renameId;
  Event(std::string path, EntryKind kind) :
    path(path), kind(kind), isCreated(false), isDeleted(false) {}

  Value toJS(const Env& env) {
    EscapableHandleScope scope(env);
    Object res = Object::New(env);
    std::string type = isCreated ? "create" : isDeleted ? "delete" : "update";
    res.Set(String::New(env, "path"), String::New(env, path.c_str()));
    res.Set(String::New(env, "type"), String::New(env, type.c_str()));
    res.Set(String::New(env, "kind"), String::New(
      env, kind == EntryKind::Directory ? "directory" : "file"
    ));
    if (renameId.has_value()) {
      res.Set(String::New(env, "renameId"), String::New(env, renameId->c_str()));
    }
    return scope.Escape(res);
  }
};

struct EventBatch {
  std::string error;
  std::vector<Event> events;
};

class EventList {
public:
  void create(std::string path, EntryKind kind) {
    std::lock_guard<std::mutex> l(mMutex);
    Event *event = internalUpdate(path, kind);
    if (event->isDeleted) {
      // Assume update event when rapidly removed and created
      // https://github.com/parcel-bundler/watcher/issues/72
      event->isDeleted = false;
      event->renameId.reset();
    } else {
      event->isCreated = true;
    }
  }

  void rename(
    std::string oldPath,
    std::string newPath,
    std::string renameId,
    EntryKind kind
  ) {
    std::lock_guard<std::mutex> l(mMutex);
    auto old = mEvents.find({oldPath, kind});

    // A path created and then renamed within one debounce window should remain
    // a single create at its final location. If it was itself the result of a
    // rename, retain the original id so rename chains stay connected end to end.
    if (old != mEvents.end() && old->second.isCreated) {
      auto originalRenameId = old->second.renameId;
      mEvents.erase(old);
      Event *created = internalUpdate(newPath, kind);
      if (created->isDeleted) {
        // Match create(): replacing a path that was already deleted in this
        // batch is an update, so there is no delete/create pair to correlate.
        created->isDeleted = false;
        created->renameId.reset();
      } else {
        created->isCreated = true;
        created->isDeleted = false;
        created->renameId = originalRenameId;
      }
      return;
    }

    Event *removed = internalUpdate(oldPath, kind);
    removed->isDeleted = true;

    Event *created = internalUpdate(newPath, kind);
    if (created->isDeleted) {
      // Match create(): replacing a path that was already deleted in this
      // batch is an update, so there is no delete/create pair to correlate.
      created->isDeleted = false;
      removed->renameId.reset();
      created->renameId.reset();
    } else {
      created->isCreated = true;
      removed->renameId = renameId;
      created->renameId = renameId;
    }
  }

  Event *update(std::string path, EntryKind kind) {
    std::lock_guard<std::mutex> l(mMutex);
    return internalUpdate(path, kind);
  }

  void remove(std::string path, EntryKind kind) {
    std::lock_guard<std::mutex> l(mMutex);
    Event *event = internalUpdate(path, kind);
    event->isDeleted = true;
    event->renameId.reset();
  }

  size_t size() {
    std::lock_guard<std::mutex> l(mMutex);
    return mEvents.size();
  }

  EventBatch drain() {
    std::lock_guard<std::mutex> l(mMutex);
    EventBatch batch {mError.value_or(""), {}};
    struct RenameParts {
      size_t created = 0;
      size_t deleted = 0;
      size_t other = 0;
    };
    std::unordered_map<std::string, RenameParts> renameParts;
    for(auto it = mEvents.begin(); it != mEvents.end(); ++it) {
      if (!(it->second.isCreated && it->second.isDeleted)) {
        batch.events.push_back(it->second);
        if (it->second.renameId.has_value()) {
          auto &parts = renameParts[*it->second.renameId];
          if (it->second.isCreated) {
            parts.created++;
          } else if (it->second.isDeleted) {
            parts.deleted++;
          } else {
            parts.other++;
          }
        }
      }
    }

    // Coalescing may remove or change one side of a rename. Never expose an id
    // unless the final batch still contains exactly one delete and one create.
    for (auto &event : batch.events) {
      if (event.renameId.has_value()) {
        auto parts = renameParts[*event.renameId];
        if (parts.created != 1 || parts.deleted != 1 || parts.other != 0) {
          event.renameId.reset();
        }
      }
    }
    mEvents.clear();
    mError.reset();
    return batch;
  }

  void error(std::string err) {
    std::lock_guard<std::mutex> l(mMutex);
    if (!mError.has_value()) {
      mError.emplace(err);
    }
  }

  bool hasError() {
    std::lock_guard<std::mutex> l(mMutex);
    return mError.has_value();
  }

private:
  using EventKey = std::pair<std::string, EntryKind>;
  mutable std::mutex mMutex;
  std::map<EventKey, Event> mEvents;
  std::optional<std::string> mError;
  Event *internalUpdate(std::string path, EntryKind kind) {
    EventKey key {path, kind};
    auto found = mEvents.find(key);
    if (found == mEvents.end()) {
      auto it = mEvents.emplace(std::move(key), Event(path, kind));
      return &it.first->second;
    }

    return &found->second;
  }
};

#endif
