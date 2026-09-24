#include <cassert>
#include <string>
#include "Event.hh"

int main(int argc, char **argv) {
  assert(argc == 2);
  bool fileToDirectory = std::string(argv[1]) == "file-to-directory";
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
