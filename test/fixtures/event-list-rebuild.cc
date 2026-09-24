#include <cassert>
#include <string>
#include "Event.hh"

static void testRenameRebuild() {
  EventList events;
  events.rename("/root/a", "/root/b", "test:1", EntryKind::File);
  events.remove("/root/b", EntryKind::File);
  events.create("/root/b", EntryKind::File);

  EventBatch batch = events.drain();
  assert(batch.events.size() == 2);
  for (const auto &event : batch.events) {
    assert(!event.renameId.has_value());
  }
}

static void testTypeReplacement(bool fileToDirectory) {
  EntryKind oldKind = fileToDirectory ? EntryKind::File : EntryKind::Directory;
  EntryKind newKind = fileToDirectory ? EntryKind::Directory : EntryKind::File;

  EventList events;
  events.remove("/root/replaced", oldKind);
  events.create("/root/replaced", newKind);

  EventBatch batch = events.drain();
  assert(batch.events.size() == 2);
  bool deletedOldKind = false;
  bool createdNewKind = false;
  for (const auto &event : batch.events) {
    assert(event.path == "/root/replaced");
    if (event.kind == oldKind && event.isDeleted && !event.isCreated) {
      deletedOldKind = true;
    }
    if (event.kind == newKind && event.isCreated && !event.isDeleted) {
      createdNewKind = true;
    }
  }
  assert(deletedOldKind && createdNewKind);
}

int main() {
  testRenameRebuild();
  testTypeReplacement(true);
  testTypeReplacement(false);
  return 0;
}
