#include <cassert>
#include <string>
#include "Event.hh"

int main() {
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
