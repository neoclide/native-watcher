# native-watcher

`native-watcher` is a small Node.js native addon for recursive, realtime
filesystem subscriptions. It contains the native watcher needed by coc.nvim and
adds conservative rename correlation. It is derived from `@parcel/watcher`
2.6.0, but is maintained as an independent minimal addon rather than a fork of
the Parcel project.

The package supports Linux, macOS, and Windows. It exposes one API: `subscribe`.

## Install and build

Node.js 20 or newer and a C++17 toolchain are required. Installing from a source
checkout builds the addon with `node-gyp`:

```sh
git clone https://github.com/neoclide/native-watcher.git
cd native-watcher
npm ci
npm test
```

`npm ci` runs the install script and writes the addon to:

```text
build/Release/native_watcher.node
```

Every GitHub Actions run uploads a tested `native_watcher.node` artifact for
these targets:

| Artifact | Runtime |
| --- | --- |
| `native-watcher-linux-x64-glibc` | Linux x64, glibc |
| `native-watcher-linux-arm64-glibc` | Linux arm64, glibc |
| `native-watcher-linux-x64-musl` | Linux x64, musl |
| `native-watcher-darwin-x64` | macOS x64 |
| `native-watcher-darwin-arm64` | macOS arm64 |
| `native-watcher-win32-x64` | Windows x64 |
| `native-watcher-win32-arm64` | Windows arm64 |

To use a downloaded artifact with a checkout, install the JavaScript
dependencies without building, then place the matching file at the loader path:

```sh
npm ci --ignore-scripts
mkdir -p build/Release
cp /path/to/native_watcher.node build/Release/native_watcher.node
```

The artifact must match the operating system, CPU architecture, and on Linux the
C library. The addon uses Node-API 8.

## API

```js
const watcher = require('native-watcher');

const subscription = await watcher.subscribe(
  '/absolute/project/path',
  (error, events) => {
    if (error) {
      console.error(error);
      return;
    }

    for (const event of events) {
      console.log(event.type, event.kind, event.path, event.renameId);
    }
  },
  {
    ignore: [
      'node_modules',
      '**/*.generated.js',
      /(^|\/)dist(\/|$)/,
    ],
  },
);

// Later. Calling unsubscribe more than once is safe.
await subscription.unsubscribe();
```

### `subscribe(directory, callback, options?)`

Starts a recursive subscription and resolves to a `Subscription` object.

- `directory` is the directory to watch. Relative paths are resolved against
  `process.cwd()`. On macOS, a symlink used as the subscription root is resolved
  to its real path before ignore rules and events are processed.
- `callback(error, events)` receives an error or a batch of events.
- `options.ignore` is an optional array of paths, glob strings, and regular
  expressions.
- `subscription.unsubscribe()` stops this callback and returns a promise.

The promise rejects when the watched path does not exist or is not a directory.
More than one subscription can watch the same directory, including with
different ignore options.

### Events

Each event has this shape:

```ts
type WatchEvent = {
  type: 'create' | 'update' | 'delete';
  kind: 'file' | 'directory';
  path: string;       // absolute path
  renameId?: string;  // opaque correlation id
};
```

Events are batched and coalesced by path and kind. Consumers must not depend on
event ordering or on seeing every intermediate filesystem operation. For example, a
rapid create followed by updates can be delivered as one `create`, and a rapid
update followed by deletion can be delivered as one `delete`.

`kind` is included on every event, including `delete` after the path no longer
exists. Only regular files and directories are reported. Symlinks, junctions,
FIFOs, and other special entries are skipped and are not traversed. Consumers
that need file-only notifications can filter for `kind === 'file'`.

Moving a populated directory into the watched tree reports `create` for the
directory and all existing descendants. The addon also starts watching every
new subdirectory; on macOS the full subtree is added to the identity index so an
immediate rename of an existing child can be correlated.
Deleting or moving a directory out reports `delete` for every indexed file and
directory below it. Renaming a directory within the watched tree reports a
delete/create pair for each visible descendant, with a separate `renameId` for
each pair. Ignore rules are applied to both old and new paths.

### Rename correlation

A rename still uses the familiar delete/create representation. When the native
backend can correlate both sides without ambiguity, those two events contain
the same opaque `renameId`:

```js
[
  {type: 'delete', kind: 'file', path: '/work/old.txt', renameId: 'opaque-id'},
  {type: 'create', kind: 'file', path: '/work/new.txt', renameId: 'opaque-id'},
]
```

Treat `renameId` only as equality data within an event batch. Its format is an
implementation detail.

| Platform | Correlation method |
| --- | --- |
| Linux | The inotify move cookie pairs `IN_MOVED_FROM` and `IN_MOVED_TO`. |
| Windows | Adjacent old/new rename records from `ReadDirectoryChangesW`. |
| macOS | A startup identity index matches a unique old and new path by `(device, inode)`. |

Linux and Windows use rename information supplied directly by the operating
system. macOS FSEvents does not supply an old/new pair, so the addon starts the
stream, scans the tree to build a file identity index, and reconciles subsequent
events against that index.

The addon omits `renameId` when only one side is visible, either side is ignored,
a hard link makes identity ambiguous, an existing target is replaced, or event
coalescing removes one side. FSEvents may merge changes or require a rescan, so a
rapid `a -> b -> c` sequence can appear as the final `a -> c` state. Rename
correlation describes the observed net change; it is not an audit log of every
filesystem operation.

### Ignore rules

Ignore rules are applied before events are delivered and before ignored
directories are recursively scanned.

```js
await watcher.subscribe(root, callback, {
  ignore: [
    'cache',                 // path relative to root
    '/absolute/tmp/output',  // absolute path
    '**/*.log',              // glob, relative to root
    /node_modules/,          // RegExp matched against the relative path
  ],
});
```

- A string without glob syntax is a path. Relative paths use the watched
  directory as their base. Ignoring a directory also ignores its descendants.
- A glob is matched against the path relative to the watched directory. Dotfiles
  are included in glob matching.
- A `RegExp` matches anywhere in the relative path. Flags are rejected; use the
  pattern itself to express the match. Its source must use syntax supported by
  C++ `std::regex` in ECMAScript mode; JavaScript-only constructs such as
  lookbehind are not supported. An unsupported pattern rejects the
  `subscribe()` promise.
- A move across an ignore boundary is a one-sided create or delete and has no
  `renameId`.

## Native startup work

The subscription promise resolves after the native watcher has started and its
required initial tree state has been built. Linux needs the scan to install an
inotify watch for each existing directory. Windows keeps directory state used by
the native backend. macOS builds both directory state and the identity index
used for rename correlation, and starts the FSEvents stream before that scan so
changes during initialization can be reconciled. Very large trees therefore add
work to `subscribe`; these scans run inside the native addon.

### Performance and threading

The startup scan is currently single-threaded per subscription. It runs through
N-API async work on a libuv worker, so JavaScript timers and the main event loop
continue to run. The practical effects are:

- `await subscribe()` resolves after the scan, so a large tree increases
  subscription latency.
- One libuv worker-pool slot is occupied while a subscription is initialized.
- JavaScript execution is not synchronously blocked by the traversal.
- A populated directory discovered later is scanned on the native backend
  thread before its complete create batch is delivered.

On 2026-09-22, a warm-cache macOS x64 measurement on a 6-core/12-thread machine
gave these results. These numbers describe that machine and filesystem cache,
not a cross-machine guarantee.

| Tree | Entries | Median subscribe | Range | Maximum 1ms timer lag | First RSS increase |
| --- | ---: | ---: | ---: | ---: | ---: |
| coc.nvim checkout | about 7,039 | 65ms | 58-85ms | 0.89ms | about 6 MiB |
| generated tree | 50,501 | 228ms | 211-248ms | 1.38ms | about 24 MiB |

Reproduce the subscription measurement against any existing directory with:

```sh
npm run benchmark:subscribe -- /path/to/tree 7
```

The addon does not currently create an indexing thread per CPU core. At the
measured scale, the serial scan is short and the JavaScript event loop remains
responsive. Unbounded core-based traversal would increase filesystem contention
and would make the stream-start/scan/event-reconciliation boundary harder to
keep correct. If substantially larger cold-cache trees become a demonstrated
startup bottleneck, the safe next design is a bounded pool partitioned by
top-level directory, capped by physical cores and a small fixed maximum, followed
by a single deterministic merge into the identity index.

## Differences from `@parcel/watcher`

This addon keeps a compatible realtime event vocabulary and compatible ignore
inputs, then deliberately narrows the product surface.

| Area | `native-watcher` | `@parcel/watcher` |
| --- | --- | --- |
| Realtime API | `subscribe` / `unsubscribe` only | `subscribe` / `unsubscribe` |
| Rename result | delete/create plus optional paired `renameId` | delete/create, without a correlation field |
| Historical queries | Not included | `writeSnapshot` and `getEventsSince` |
| Backends | Direct FSEvents, inotify, and `ReadDirectoryChangesW` | Native backends plus Watchman support and other platform fallbacks |
| Platforms built here | Linux, macOS, Windows | Also supports additional upstream targets such as FreeBSD |
| WebAssembly | Not included | WASM build available upstream |
| Backend selection | Automatic for the current OS | Public `backend` option |
| Distribution | Source build and per-platform GitHub Actions artifacts | Published npm package with prebuild packages |
| Intended scope | Minimal coc.nvim native watcher | General-purpose Parcel watcher package |

It is therefore not a drop-in replacement for applications that call snapshot
APIs, select a backend, rely on Watchman, need WASM, or expect the upstream npm
distribution layout.

## Tests

```sh
npm test
```

The suite covers file and directory create/update/delete behavior, recursive
trees present before and after subscription, populated directory moves and
indexing, symlinks, subscription lifecycle, invalid roots, independent ignore
configurations, path/glob/RegExp ignores, ignore-boundary moves, and
platform-specific rename correlation and ambiguity cases. GitHub Actions runs
the suite on all targets listed in the artifact table and uploads the tested
addon from each job.

## Origin and license

The native watcher implementation is derived from
[`@parcel/watcher`](https://github.com/parcel-bundler/watcher), version 2.6.0,
under its MIT license. This repository contains the reduced realtime native
implementation, rename correlation changes, tests, and build automation.
