# native-watcher

A small Node.js native addon for recursive filesystem subscriptions. It adds
exact rename correlation when the operating system exposes a reliable pair.

Linux inotify and Windows `ReadDirectoryChangesW` emit a delete and create event
with the same opaque `renameId`:

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

macOS FSEvents does not provide exact rename pairs, so its events omit
`renameId`. Events also omit the id when only one side of a move is visible.

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
