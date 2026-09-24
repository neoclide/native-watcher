'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {execFile: execFileCallback} = require('node:child_process');
const {promisify} = require('node:util');
const watcher = require('..');

const execFileAsync = promisify(execFileCallback);

const exactRenamePlatform =
  process.platform === 'darwin' ||
  process.platform === 'linux' ||
  process.platform === 'win32';

test(
  'preserves event kinds and clears rename identities during batch modifications',
  {skip: process.platform === 'win32'},
  async () => {
    const tempDirectory = await fs.mkdtemp(
      path.join(await fs.realpath(os.tmpdir()), 'native-watcher-event-list-'),
    );
    try {
      const addonInclude = require('node-addon-api').include.replace(/^"|"$/g, '');
      const nodeInclude = path.resolve(process.execPath, '..', '..', 'include', 'node');
      const source = path.join(__dirname, 'fixtures', 'event-list-rebuild.cc');
      const binary = path.join(tempDirectory, 'event-list-rebuild');
      await execFileAsync('c++', [
        '-std=c++17',
        '-ffunction-sections',
        '-fdata-sections',
        process.platform === 'darwin' ? '-Wl,-dead_strip' : '-Wl,--gc-sections',
        `-I${addonInclude}`,
        `-I${nodeInclude}`,
        `-I${path.join(__dirname, '..', 'src')}`,
        source,
        '-o',
        binary,
      ]);
      await execFileAsync(binary);
    } finally {
      await fs.rm(tempDirectory, {recursive: true, force: true});
    }
  },
);

function waitForEvents(queue, predicate = () => true, timeout = 5000) {
  if (queue.error) return Promise.reject(queue.error);
  return new Promise((resolve, reject) => {
    const waiter = {
      events: [],
      predicate,
      resolve(events) {
        clearTimeout(timer);
        resolve(events);
      },
      reject(error) {
        clearTimeout(timer);
        reject(error);
      },
    };
    const timer = setTimeout(() => reject(new Error(
      `timed out waiting for events: ${JSON.stringify(waiter.events)}`,
    )), timeout);
    queue.push(waiter);
  });
}

function dispatchEvents(queue, error, events) {
  let failure = error;
  if (!failure) {
    try {
      assertRenameIdsArePaired(events);
    } catch (assertionError) {
      failure = assertionError;
    }
  }

  const next = queue[0];
  if (failure) {
    if (next) {
      queue.shift();
      next.reject(failure);
    } else {
      queue.error = failure;
    }
    return;
  }
  if (next) {
    next.events.push(...events);
    if (next.predicate(next.events)) {
      queue.shift();
      next.resolve(next.events);
    }
  }
}

function assertRenameIdsArePaired(events) {
  const pairs = new Map();
  for (const event of events) {
    if (event.renameId === undefined) continue;
    const counts = pairs.get(event.renameId) ?? {create: 0, delete: 0, other: 0};
    if (event.type === 'create') counts.create++;
    else if (event.type === 'delete') counts.delete++;
    else counts.other++;
    pairs.set(event.renameId, counts);
  }
  for (const [renameId, counts] of pairs) {
    assert.deepEqual(
      counts,
      {create: 1, delete: 1, other: 0},
      `rename ${renameId} must be complete within one callback batch`,
    );
  }
}

function containsEvent(events, type, eventPath) {
  return events.some((event) => event.type === type && event.path === eventPath);
}

function containsRename(events, oldPath, newPath) {
  return (
    containsEvent(events, 'delete', oldPath) &&
    containsEvent(events, 'create', newPath)
  );
}

function assertRename(events, oldPath, newPath) {
  const removed = events.find(
    (event) => event.type === 'delete' && event.path === oldPath,
  );
  const created = events.find(
    (event) => event.type === 'create' && event.path === newPath,
  );

  assert.ok(removed, `missing delete event for ${oldPath}`);
  assert.ok(created, `missing create event for ${newPath}`);
  assert.equal(typeof removed.renameId, 'string');
  assert.equal(created.renameId, removed.renameId);
}

function assertKind(events, type, eventPath, kind) {
  const event = events.find(
    (candidate) => candidate.type === type && candidate.path === eventPath,
  );
  assert.ok(event, `missing ${type} event for ${eventPath}`);
  assert.equal(event.kind, kind);
  return event;
}

test('pairs every descendant of a renamed directory with its entry kind',
  {skip: !exactRenamePlatform}, async (t) => {
    const directory = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-tree-rename-',
    ));
    const oldRoot = path.join(directory, 'old');
    const newRoot = path.join(directory, 'new');
    const oldNested = path.join(oldRoot, 'nested');
    const oldFile = path.join(oldNested, 'file.txt');
    await fs.mkdir(oldNested, {recursive: true});
    await fs.writeFile(oldFile, 'content');

    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });
    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const barrier = path.join(directory, 'barrier.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', barrier),
    );
    await fs.writeFile(barrier, 'ready');
    await eventsPromise;

    const newNested = path.join(newRoot, 'nested');
    const newFile = path.join(newNested, 'file.txt');
    eventsPromise = waitForEvents(pending, (events) =>
      [
        [oldRoot, newRoot],
        [oldNested, newNested],
        [oldFile, newFile],
      ].every(([oldPath, newPath]) => containsRename(events, oldPath, newPath)),
    );
    await fs.rename(oldRoot, newRoot);
    const events = await eventsPromise;
    const ids = new Set();
    for (const [oldPath, newPath, kind] of [
      [oldRoot, newRoot, 'directory'],
      [oldNested, newNested, 'directory'],
      [oldFile, newFile, 'file'],
    ]) {
      assertRename(events, oldPath, newPath);
      const removed = assertKind(events, 'delete', oldPath, kind);
      assertKind(events, 'create', newPath, kind);
      ids.add(removed.renameId);
    }
    assert.equal(ids.size, 3, 'each entry needs its own rename id');
  });

test('reports typed descendant deletes when a directory leaves the root',
  {skip: !exactRenamePlatform}, async (t) => {
    const parent = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-tree-move-out-',
    ));
    const directory = path.join(parent, 'watched');
    const oldRoot = path.join(directory, 'old');
    const oldNested = path.join(oldRoot, 'nested');
    const oldFile = path.join(oldNested, 'file.txt');
    const outside = path.join(parent, 'outside');
    await fs.mkdir(oldNested, {recursive: true});
    await fs.writeFile(oldFile, 'content');

    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });
    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(parent, {recursive: true, force: true});
    });

    const barrier = path.join(directory, 'barrier.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', barrier),
    );
    await fs.writeFile(barrier, 'ready');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      [oldRoot, oldNested, oldFile].every((entry) =>
        containsEvent(events, 'delete', entry)),
    );
    await fs.rename(oldRoot, outside);
    const events = await eventsPromise;
    assertKind(events, 'delete', oldRoot, 'directory');
    assertKind(events, 'delete', oldNested, 'directory');
    assertKind(events, 'delete', oldFile, 'file');
    assert.ok(events.every((event) => event.renameId === undefined));
  });

test('applies ignores to each descendant across a directory rename',
  {skip: !exactRenamePlatform}, async (t) => {
    const directory = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-tree-ignore-',
    ));
    const oldRoot = path.join(directory, 'old');
    const newRoot = path.join(directory, 'new');
    const oldHidden = path.join(oldRoot, 'hidden.txt');
    const oldVisible = path.join(oldRoot, 'visible.txt');
    const newHidden = path.join(newRoot, 'hidden.txt');
    const newVisible = path.join(newRoot, 'visible.txt');
    await fs.mkdir(oldRoot);
    await fs.writeFile(oldHidden, 'hidden');
    await fs.writeFile(oldVisible, 'visible');

    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    }, {ignore: ['old/hidden.txt', 'new/visible.txt']});
    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const barrier = path.join(directory, 'barrier.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', barrier),
    );
    await fs.writeFile(barrier, 'ready');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, oldRoot, newRoot) &&
      containsEvent(events, 'delete', oldVisible) &&
      containsEvent(events, 'create', newHidden),
    );
    await fs.rename(oldRoot, newRoot);
    const events = await eventsPromise;
    assertRename(events, oldRoot, newRoot);
    assert.equal(assertKind(events, 'delete', oldVisible, 'file').renameId, undefined);
    assert.equal(assertKind(events, 'create', newHidden, 'file').renameId, undefined);
    assert.ok(events.every((event) =>
      event.path !== oldHidden && event.path !== newVisible));
  });


test(
  'correlates file and directory renames from the native backend',
  {skip: !exactRenamePlatform},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const oldFile = path.join(directory, 'old.txt');
    const newFile = path.join(directory, 'new.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', oldFile),
    );
    await fs.writeFile(oldFile, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, oldFile, newFile),
    );
    await fs.rename(oldFile, newFile);
    assertRename(await eventsPromise, oldFile, newFile);

    const oldDirectory = path.join(directory, 'old-directory');
    const newDirectory = path.join(directory, 'new-directory');
    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', oldDirectory),
    );
    await fs.mkdir(oldDirectory);
    await eventsPromise;

    const oldChild = path.join(oldDirectory, 'child.txt');
    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', oldChild),
    );
    await fs.writeFile(oldChild, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, oldDirectory, newDirectory),
    );
    await fs.rename(oldDirectory, newDirectory);
    assertRename(await eventsPromise, oldDirectory, newDirectory);

    const movedChild = path.join(newDirectory, 'child.txt');
    const renamedChild = path.join(newDirectory, 'renamed-child.txt');
    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, movedChild, renamedChild),
    );
    await fs.rename(movedChild, renamedChild);
    assertRename(await eventsPromise, movedChild, renamedChild);
  },
);

test(
  'correlates a file that existed before subscription',
  {skip: !exactRenamePlatform},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const oldPath = path.join(directory, 'existing.txt');
    const newPath = path.join(directory, 'renamed.txt');
    await fs.writeFile(oldPath, 'content');

    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    // Flush FSEvents records queued while the startup index was built. Without
    // this boundary, the pre-subscription create can legitimately coalesce
    // with the rename into one final create event.
    const barrier = path.join(directory, 'barrier.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', barrier),
    );
    await fs.writeFile(barrier, 'ready');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, oldPath, newPath),
    );
    await fs.rename(oldPath, newPath);
    assertRename(await eventsPromise, oldPath, newPath);
  },
);

test(
  'correlates a case-only rename on macOS',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const oldPath = path.join(directory, 'case-name.txt');
    const newPath = path.join(directory, 'CASE-name.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', oldPath),
    );
    await fs.writeFile(oldPath, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, oldPath, newPath),
    );
    await fs.rename(oldPath, newPath);
    assertRename(await eventsPromise, oldPath, newPath);
  },
);

test(
  'does not infer a macOS rename from ambiguous hard-link identity',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const original = path.join(directory, 'original.txt');
    const linked = path.join(directory, 'linked.txt');
    const renamed = path.join(directory, 'renamed.txt');

    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', original),
    );
    await fs.writeFile(original, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', linked),
    );
    await fs.link(original, linked);
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      events.some((event) => event.path === original) &&
      events.some((event) => event.path === renamed),
    );
    await fs.rename(original, renamed);
    const events = await eventsPromise;
    assert.ok(events.some((event) => event.path === original));
    assert.ok(events.some((event) => event.path === renamed));
    assert.ok(events.every((event) => event.renameId === undefined));
  },
);

test(
  'treats macOS target replacement as ambiguous and refreshes its identity',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const source = path.join(directory, 'source.txt');
    const target = path.join(directory, 'target.txt');
    const finalPath = path.join(directory, 'final.txt');

    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', source),
    );
    await fs.writeFile(source, 'source');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', target),
    );
    await fs.writeFile(target, 'target');
    await eventsPromise;

    const timestamp = new Date(1_700_000_000_000);
    eventsPromise = waitForEvents(pending, (events) =>
      events.some(
        (event) =>
          event.type === 'update' &&
          (event.path === source || event.path === target),
      ),
    );
    await Promise.all([
      fs.utimes(source, timestamp, timestamp),
      fs.utimes(target, timestamp, timestamp),
    ]);
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'delete', source) &&
      events.some((event) => event.path === target),
    );
    await fs.rename(source, target);
    const replacementEvents = await eventsPromise;
    assert.ok(replacementEvents.every((event) => event.renameId === undefined));

    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, target, finalPath),
    );
    await fs.rename(target, finalPath);
    assertRename(await eventsPromise, target, finalPath);
  },
);

test(
  'does not correlate a rename that replaces an existing target',
  {skip: !['linux', 'win32'].includes(process.platform)},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const source = path.join(directory, 'source.txt');
    const target = path.join(directory, 'target.txt');
    await fs.writeFile(source, 'source');
    await fs.writeFile(target, 'target');

    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });
    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'delete', source) &&
      events.some((event) => event.path === target),
    );
    await fs.rename(source, target);
    const events = await eventsPromise;
    assert.ok(events.every((event) => event.renameId === undefined));
  },
);

test(
  'preserves Windows descendant indexes across queued directory renames',
  {skip: process.platform !== 'win32'},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const oldRoot = path.join(directory, 'old');
    const middleRoot = path.join(directory, 'middle');
    const newRoot = path.join(directory, 'new');
    const target = path.join(newRoot, 'nested', 'target.txt');
    const source = path.join(newRoot, 'nested', 'source.txt');
    const marker = path.join(directory, 'delivery-marker');
    await fs.mkdir(path.join(oldRoot, 'nested'), {recursive: true});
    await fs.writeFile(path.join(oldRoot, 'nested', 'target.txt'), 'target');

    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });
    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', marker),
    );
    await fs.rename(oldRoot, middleRoot);
    await fs.rename(middleRoot, newRoot);
    await fs.writeFile(marker, 'marker');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', source),
    );
    await fs.writeFile(source, 'source');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'delete', source) &&
      events.some((event) => event.path === target),
    );
    await fs.rename(source, target);
    const replacementEvents = await eventsPromise;
    assert.ok(replacementEvents.every((event) => event.renameId === undefined));
  },
);

test(
  'does not assign a rename id when only one side is watched',
  {skip: !exactRenamePlatform},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const parent = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const directory = path.join(parent, 'watched');
    await fs.mkdir(directory);
    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(parent, {recursive: true, force: true});
    });

    const inside = path.join(directory, 'inside.txt');
    const outside = path.join(parent, 'outside.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', inside),
    );
    await fs.writeFile(inside, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'delete', inside),
    );
    await fs.rename(inside, outside);
    const events = await eventsPromise;
    const removed = events.find(
      (event) => event.type === 'delete' && event.path === inside,
    );
    assert.ok(removed);
    assert.equal(removed.renameId, undefined);

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', inside),
    );
    await fs.rename(outside, inside);
    const returnedEvents = await eventsPromise;
    const created = returnedEvents.find(
      (event) => event.type === 'create' && event.path === inside,
    );
    assert.ok(created);
    assert.equal(created.renameId, undefined);
  },
);

test(
  'removes macOS identities for every descendant of a deleted directory',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    const tempDirectory = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-'));
    const pending = [];
    const subscription = await watcher.subscribe(directory, (error, events) => {
      dispatchEvents(pending, error, events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const subtree = path.join(directory, 'subtree');
    const nestedDirectory = path.join(subtree, 'nested');
    const nestedFile = path.join(nestedDirectory, 'file.txt');
    let eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', nestedFile),
    );
    await fs.mkdir(nestedDirectory, {recursive: true});
    await fs.writeFile(nestedFile, 'first');
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'delete', subtree),
    );
    await fs.rm(subtree, {recursive: true});
    await eventsPromise;

    eventsPromise = waitForEvents(pending, (events) =>
      containsEvent(events, 'create', nestedFile),
    );
    await fs.mkdir(nestedDirectory, {recursive: true});
    await fs.writeFile(nestedFile, 'second');
    await eventsPromise;

    const renamedFile = path.join(nestedDirectory, 'renamed.txt');
    eventsPromise = waitForEvents(pending, (events) =>
      containsRename(events, nestedFile, renamedFile),
    );
    await fs.rename(nestedFile, renamedFile);
    assertRename(await eventsPromise, nestedFile, renamedFile);
  },
);
