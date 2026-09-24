# native-watcher

`native-watcher` is a minimal Node.js native addon for recursive, real-time filesystem watching, built for [coc.nvim](https://github.com/neoclide/coc.nvim).

Derived from [`@parcel/watcher`](https://github.com/parcel-bundler/watcher) 2.6.0, it focuses solely on the `subscribe` API with conservative rename correlation across Linux, macOS, and Windows.

## Installation & Build

Requires Node.js >= 20 and a C++17 compiler.

```sh
git clone https://github.com/neoclide/native-watcher.git
cd native-watcher
npm ci
npm test
```

The compiled binary will be placed at `build/Release/native_watcher.node`.

Prebuilt binaries for Linux (x64/arm64, glibc/musl), macOS (x64/arm64), and Windows (x64/arm64) are also available from GitHub Actions artifacts. To use a prebuilt binary:

```sh
npm ci --ignore-scripts
mkdir -p build/Release
cp /path/to/native_watcher.node build/Release/native_watcher.node
```

## Usage

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

// Unsubscribe when done
await subscription.unsubscribe();
```

## API

### `subscribe(directory, callback, options?)`

Starts watching `directory` recursively. Returns a `Promise<Subscription>`.

- **`directory`**: Directory to watch (relative paths resolve against `process.cwd()`).
- **`callback(error, events)`**: Callback receiving an `Error` or an array of `WatchEvent`.
- **`options.ignore`**: Array of relative/absolute paths, glob strings, or regular expressions to ignore.
- **`subscription.unsubscribe()`**: Stops watching and returns a `Promise<void>`.

### `WatchEvent`

Each event has this shape:

```ts
type WatchEvent = {
  type: 'create' | 'update' | 'delete';
  kind: 'file' | 'directory';
  path: string;       // absolute path
  renameId?: string;  // opaque correlation id
};
```

Events are batched and coalesced by path and kind:

- **`kind`**: `'file' | 'directory'`, retained on `delete` even after the path is removed. Symlinks and special entries are skipped.
- **`renameId`**: Paired on `delete` (old path) and `create` (new path) when the backend correlates an unambiguous rename.

### Ignore Rules

```js
await watcher.subscribe(root, callback, {
  ignore: [
    'cache',                 // Relative path
    '/absolute/tmp/output',  // Absolute path
    '**/*.log',              // Glob (relative to root)
    /node_modules/,          // RegExp (ECMAScript syntax without flags)
  ],
});
```

## Platform Backends

| Platform | Backend | Rename Correlation |
| --- | --- | --- |
| **Linux** | `inotify` | Paired `IN_MOVED_FROM` / `IN_MOVED_TO` cookie |
| **macOS** | `FSEvents` | Matched by inode & device identity index |
| **Windows** | `ReadDirectoryChangesW` | Paired old/new rename records |

## Differences from `@parcel/watcher`

- **Minimal API surface**: Provides only `subscribe` / `unsubscribe`. Snapshot queries (`writeSnapshot`, `getEventsSince`) and Watchman/WASM fallbacks are omitted.
- **Rename correlation**: Associates renames via `renameId` across paired delete/create events.
- **Tailored for [coc.nvim](https://github.com/neoclide/coc.nvim)**: Lightweight, zero unnecessary dependencies, and streamlined startup scanning.

## Testing

```sh
npm test
```

## License

MIT (derived from [`@parcel/watcher`](https://github.com/parcel-bundler/watcher) 2.6.0).
