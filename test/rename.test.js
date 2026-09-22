'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const watcher = require('..');

const exactRenamePlatform =
  process.platform === 'linux' || process.platform === 'win32';

function waitForEvents(queue, timeout = 5000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timed out waiting for events')), timeout);
    queue.push({
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
    const next = pending.shift();
    if (!next) return;
    if (error) next.reject(error);
    else next.resolve(events);
  });

  t.after(async () => {
    await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  });

  const file = path.join(directory, 'created.txt');
  const eventsPromise = waitForEvents(pending);
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
      const next = pending.shift();
      if (!next) return;
      if (error) next.reject(error);
      else next.resolve(events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    });

    const oldFile = path.join(directory, 'old.txt');
    const newFile = path.join(directory, 'new.txt');
    let eventsPromise = waitForEvents(pending);
    await fs.writeFile(oldFile, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending);
    await fs.rename(oldFile, newFile);
    assertRename(await eventsPromise, oldFile, newFile);

    const oldDirectory = path.join(directory, 'old-directory');
    const newDirectory = path.join(directory, 'new-directory');
    eventsPromise = waitForEvents(pending);
    await fs.mkdir(oldDirectory);
    await eventsPromise;

    eventsPromise = waitForEvents(pending);
    await fs.rename(oldDirectory, newDirectory);
    assertRename(await eventsPromise, oldDirectory, newDirectory);
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
      const next = pending.shift();
      if (!next) return;
      if (error) next.reject(error);
      else next.resolve(events);
    });

    t.after(async () => {
      await subscription.unsubscribe();
      await fs.rm(parent, {recursive: true, force: true});
    });

    const inside = path.join(directory, 'inside.txt');
    const outside = path.join(parent, 'outside.txt');
    let eventsPromise = waitForEvents(pending);
    await fs.writeFile(inside, 'content');
    await eventsPromise;

    eventsPromise = waitForEvents(pending);
    await fs.rename(inside, outside);
    const events = await eventsPromise;
    const removed = events.find(
      (event) => event.type === 'delete' && event.path === inside,
    );
    assert.ok(removed);
    assert.equal(removed.renameId, undefined);
  },
);
