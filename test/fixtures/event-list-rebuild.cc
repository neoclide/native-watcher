#include <cassert>
#include <string>
#include "Event.hh"

int main() {
  EventList events;
  events.rename("/root/a", "/root/b", "test:1");
  events.remove("/root/b");
  events.create("/root/b");

  EventBatch batch = events.drain();
  assert(batch.events.size() == 2);
  for (const auto &event : batch.events) {
    assert(!event.renameId.has_value());
  }
}
