'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const watcher = require('..');

const exactRenamePlatform =
  process.platform === 'darwin' ||
  process.platform === 'linux' ||
  process.platform === 'win32';

function waitForEvents(queue, predicate = () => true, timeout = 5000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timed out waiting for events')), timeout);
    queue.push({
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
    });
  });
}

function dispatchEvents(queue, error, events) {
  const next = queue[0];
  if (!next) return;
  if (error) {
    queue.shift();
    next.reject(error);
  } else {
    next.events.push(...events);
    if (next.predicate(next.events)) {
      queue.shift();
      next.resolve(next.events);
    }
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

test('native subscription emits filesystem events', async (t) => {
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

  const file = path.join(directory, 'created.txt');
  const eventsPromise = waitForEvents(pending, (events) =>
    containsEvent(events, 'create', file),
  );
  await fs.writeFile(file, 'content');
  const events = await eventsPromise;
  assert.ok(
    events.some((event) => event.type === 'create' && event.path === file),
  );
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
