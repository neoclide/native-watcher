# native-watcher

[![npm](https://img.shields.io/npm/v/native-watcher.svg)](https://www.npmjs.com/package/native-watcher)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A minimal, high-performance Node.js native addon for recursive, real-time filesystem watching, built for [coc.nvim](https://github.com/neoclide/coc.nvim).

Derived from [`@parcel/watcher`](https://github.com/parcel-bundler/watcher) 2.6.0.

---

## Why native-watcher?

[coc.nvim](https://github.com/neoclide/coc.nvim) requires a reliable, lightweight filesystem watcher to track workspace changes in real time for buffer synchronization, diagnostics, and Language Server Protocol (LSP) workspace monitoring.

Existing solutions like `@parcel/watcher` are designed primarily for web bundlers and carry unnecessary complexity for editor integration:
- **Excessive overhead**: Bundler-specific features such as snapshot persistence (`writeSnapshot`), historical change queries (`getEventsSince`), Watchman fallbacks, and WASM backends increase binary size and maintenance burden.
- **Uncorrelated renames**: Standard watchers emit disconnected `delete` and `create` events when files or folders are moved or renamed, forcing editors to treat renames as complete file deletions followed by new file additions.
- **JavaScript regex overhead**: Processing ignore rules through JS regular expressions creates unnecessary serialization and IPC overhead during rapid file operations.

`native-watcher` was created to provide a lean, zero-bloat native watcher that focuses strictly on real-time subscriptions with native rename correlation and in-engine ignore matching.

---

## Improvements over `@parcel/watcher`

- **Minimalist API surface**: Stripped snapshot persistence (`writeSnapshot`, `getEventsSince`), Watchman integration, and WASM fallbacks. Exposes solely `subscribe` and `unsubscribe` with zero unnecessary runtime dependencies.
- **Conservative rename correlation (`renameId`)**: Correlates paired file and directory moves across all supported platforms, tagging matching `delete` (source) and `create` (target) events with a shared `renameId`:
  - **Linux (`inotify`)**: Correlated via `IN_MOVED_FROM` / `IN_MOVED_TO` cookie.
  - **macOS (`FSEvents`)**: Correlated via inode and device identity tracking.
  - **Windows (`ReadDirectoryChangesW`)**: Correlated via paired old/new rename records.
- **Native raw glob matcher**: Replaced the JS RegExp ignore pipeline with an in-engine glob engine (`*` and `**`) implemented in C++. Automatically queries per-component filesystem case sensitivity (macOS, Windows, and Linux ext4 casefold).
- **Platform robustness & edge-case fixes**:
  - *Linux*: Accurately reports rapid atomic replacements (safe-write / rename) as `update` events; properly invalidates subscriptions when ancestor directories are moved or removed; handles replacement by non-regular entries.
  - *macOS*: Preserves metadata updates during full-tree reconciliation; supports case-only renames; guarantees safe stream teardown.
  - *Windows*: Reports rapid atomic replacements as `update` events; prevents backend use-after-free and crash during stop/error teardown.
- **Optimized binary size**: Enabled hidden symbol visibility and link-time dead-code elimination on Linux and macOS, resulting in minimal memory and disk footprint.

---

## Installation

```sh
npm install native-watcher
```

> **Note**: Requires Node.js >= 20 and a C++17 compiler (`gcc`, `clang`, or MSVC) to build the native addon upon installation.

---

## Usage

```javascript
const watcher = require('native-watcher');

// Start watching recursively
const subscription = await watcher.subscribe(
  '/path/to/project',
  (error, events) => {
    if (error) {
      console.error('Watcher error:', error);
      return;
    }
    for (const event of events) {
      console.log(event.type, event.kind, event.path, event.renameId);
    }
  },
  {
    ignore: [
      'node_modules/**',
      '**/*.log',
      '.git/**',
    ],
  },
);

// Stop watching when done
await subscription.unsubscribe();
```

### Event Object (`WatchEvent`)

Each change triggers a batch of events:

```typescript
type WatchEvent = {
  path: string;                      // Absolute path
  type: 'create' | 'update' | 'delete';
  kind: 'file' | 'directory';        // Retained on delete even after removal
  renameId?: string;                 // Set when delete and create form a correlated rename
};
```

### Ignore Patterns

The `ignore` option accepts relative paths, absolute paths, or glob patterns:
- `*`: Matches zero or more characters within a single path component (including dotfiles).
- `**`: Matches zero or more complete path components (e.g., `**/*.log` matches root and nested `.log` files).
- Directory patterns exclude the entire subtree under that directory.

---

## Build

### Requirements

- Node.js >= 20
- C++17 compiler (`gcc`, `clang`, or MSVC)

### Building from Source

```sh
npm ci
npm run build
```

The compiled binary will be placed at `build/Release/native_watcher.node`.

### Prebuilt Binaries

Prebuilt binaries for Linux (x64/arm64, glibc/musl), macOS (x64/arm64), and Windows (x64/arm64) can be downloaded from GitHub Actions artifacts using:

```sh
./scripts/download-ci-binaries.sh
```

---

## Testing

Run the test suite locally:

```sh
npm test
```

> **Note for Windows**: Enable [Developer Mode](https://learn.microsoft.com/en-us/windows/advanced-settings/developer-mode) so non-admin processes can create symlinks required by the test suite.

Cross-platform test scripts:
- **Linux**: `./scripts/test-linux.sh` (rsyncs to remote Linux host and runs tests)
- **Windows**: `./scripts/test-windows.sh` (transfers to remote Windows host `win11` and runs tests)

---

## License

MIT (derived from [`@parcel/watcher`](https://github.com/parcel-bundler/watcher) 2.6.0).
