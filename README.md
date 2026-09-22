# native-watcher

A small Node.js native addon for recursive filesystem subscriptions. It adds
conservative rename correlation when the native backend has enough evidence.

Linux inotify and Windows `ReadDirectoryChangesW` use operating-system rename
pairs. macOS uses an in-memory `(device, inode)` identity index built after the
FSEvents stream starts. A correlated delete and create event have the same
opaque `renameId`:

```js
const watcher = require('native-watcher');

const subscription = await watcher.subscribe(directory, (error, events) => {
  if (error) throw error;
  console.log(events);
});

// Later:
await subscription.unsubscribe();
```

```js
[
  {type: 'delete', path: '/work/old.txt', renameId: 'inotify:1234'},
  {type: 'create', path: '/work/new.txt', renameId: 'inotify:1234'},
]
```

macOS emits `renameId` only when one indexed path disappeared and one new path
has the same unique identity. It deliberately falls back to ordinary events for
hard links, target replacement, one-sided moves, and other ambiguous changes.
If FSEvents requests a rescan, the addon rebuilds the index and reports the
unambiguous net change; intermediate rename steps may have been coalesced.

Build and test with:

```sh
npm install
npm test
```

## Origin

The native watcher implementation is derived from
[`@parcel/watcher`](https://github.com/parcel-bundler/watcher), version 2.6.0,
under its MIT license. This repository is an independent, reduced source
distribution rather than a fork: it contains only the realtime native watcher,
the rename correlation changes, and their tests.
