'use strict';

const assert = require('node:assert/strict');
const fsSync = require('node:fs');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {execFile} = require('node:child_process');
const test = require('node:test');
const {promisify} = require('node:util');
const watcher = require('..');
const {
  EventCollector,
  assertEvent,
  assertNoPath,
  createFixture,
} = require('./helpers');

const settle = () => new Promise((resolve) => setTimeout(resolve, 150));
const execFileAsync = promisify(execFile);

test('reports create, update, and delete for a file', async (t) => {
  const {directory, collector} = await createFixture(t);
  const file = path.join(directory, 'file.txt');

  let mark = collector.mark();
  const created = collector.waitFor('create', file, mark);
  await fs.writeFile(file, 'one');
  assertEvent(await created, 'create', file);

  mark = collector.mark();
  const updated = collector.waitFor('update', file, mark);
  await fs.writeFile(file, 'two');
  assertEvent(await updated, 'update', file);

  mark = collector.mark();
  const deleted = collector.waitFor('delete', file, mark);
  await fs.unlink(file);
  assertEvent(await deleted, 'delete', file);
});

test('rapid file changes do not leave a stale final event state', async (t) => {
  const {directory, collector} = await createFixture(t);
  const ephemeral = path.join(directory, 'ephemeral.txt');
  const replaced = path.join(directory, 'replaced.txt');

  let mark = collector.mark();
  let marker = path.join(directory, 'first-marker');
  let observed = collector.waitFor('create', marker, mark);
  await fs.writeFile(ephemeral, 'temporary');
  await fs.unlink(ephemeral);
  await fs.writeFile(marker, 'marker');
  await observed;
  let ephemeralEvents = collector.events
    .slice(mark)
    .filter((event) => event.path === ephemeral);
  if (ephemeralEvents.at(-1)?.type === 'create') {
    await collector.waitFor('delete', ephemeral, mark);
    ephemeralEvents = collector.events
      .slice(mark)
      .filter((event) => event.path === ephemeral);
  }
  assert.ok(
    ephemeralEvents.length === 0 ||
      ephemeralEvents.at(-1).type === 'delete',
    `short-lived path ended as present: ${JSON.stringify(ephemeralEvents)}`,
  );

  observed = collector.waitFor('create', replaced);
  await fs.writeFile(replaced, 'before');
  await observed;

  mark = collector.mark();
  marker = path.join(directory, 'second-marker');
  observed = collector.waitFor('create', marker, mark);
  await fs.unlink(replaced);
  await fs.writeFile(replaced, 'after');
  await fs.writeFile(marker, 'marker');
  await observed;
  let replacementEvents = collector.events
    .slice(mark)
    .filter((event) => event.path === replaced);
  if (replacementEvents.at(-1)?.type === 'delete') {
    await collector.waitFrom(mark, (events) =>
      events.some(
        (event) => event.path === replaced && event.type !== 'delete',
      ),
    );
    replacementEvents = collector.events
      .slice(mark)
      .filter((event) => event.path === replaced);
  }
  assert.ok(
    replacementEvents.length === 0 ||
      replacementEvents.at(-1).type !== 'delete',
    `recreated path ended as deleted: ${JSON.stringify(replacementEvents)}`,
  );
});

test('recursively reports nested file and directory changes', async (t) => {
  const {directory, collector} = await createFixture(t);
  const parent = path.join(directory, 'parent');
  const childDirectory = path.join(parent, 'child');
  const file = path.join(childDirectory, 'nested.txt');

  let mark = collector.mark();
  let waiting = collector.waitFor('create', childDirectory, mark);
  await fs.mkdir(childDirectory, {recursive: true});
  let events = await waiting;
  assertEvent(events, 'create', parent);
  assertEvent(events, 'create', childDirectory);

  mark = collector.mark();
  waiting = collector.waitFor('create', file, mark);
  await fs.writeFile(file, 'one');
  assertEvent(await waiting, 'create', file);

  mark = collector.mark();
  waiting = collector.waitFor('update', file, mark);
  await fs.appendFile(file, 'two');
  assertEvent(await waiting, 'update', file);

  mark = collector.mark();
  waiting = collector.waitFrom(
    mark,
    (batch) =>
      batch.some((event) => event.type === 'delete' && event.path === file) &&
      batch.some(
        (event) =>
          event.type === 'delete' && event.path === childDirectory,
      ),
  );
  await fs.rm(childDirectory, {recursive: true});
  events = await waiting;
  assertEvent(events, 'delete', file);
  assertEvent(events, 'delete', childDirectory);
});

test('watches a nested directory tree that existed before subscription', async (t) => {
  let existingFile;
  const {directory, collector} = await createFixture(t, undefined, async (directory) => {
    existingFile = path.join(directory, 'existing', 'nested', 'file.txt');
    await fs.mkdir(path.dirname(existingFile), {recursive: true});
    await fs.writeFile(existingFile, 'one');
  });

  // Flush any startup events FSEvents queued while the initial tree was built.
  const barrier = path.join(directory, 'barrier.txt');
  let mark = collector.mark();
  let waiting = collector.waitFor('create', barrier, mark);
  await fs.writeFile(barrier, 'ready');
  await waiting;

  mark = collector.mark();
  waiting = collector.waitFor('update', existingFile, mark);
  await fs.writeFile(existingFile, 'two');
  assertEvent(await waiting, 'update', existingFile);
});

test('remains usable when a subtree changes during initial scanning', async () => {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-scan-race-'),
  );
  const first = path.join(directory, 'moving-a');
  const second = path.join(directory, 'moving-b');
  await fs.mkdir(first);
  await fs.writeFile(path.join(first, 'child.txt'), 'before');
  for (let index = 0; index < 64; index++) {
    const nested = path.join(directory, `existing-${index}`, 'nested');
    await fs.mkdir(nested, {recursive: true});
    await fs.writeFile(path.join(nested, 'file.txt'), 'existing');
  }

  const collector = new EventCollector();
  let current = first;
  let next = second;
  let subscription;
  try {
    const subscribing = watcher.subscribe(directory, collector.callback);
    for (let index = 0; index < 8; index++) {
      await fs.rename(current, next);
      [current, next] = [next, current];
    }
    subscription = await subscribing;

    const child = path.join(current, 'child.txt');
    const observed = collector.waitFor('update', child);
    await fs.appendFile(child, 'after');
    assertEvent(await observed, 'update', child);
  } finally {
    if (subscription) await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
});

test('indexes and reports a populated directory moved into the root', async (t) => {
  const tempDirectory = await fs.realpath(os.tmpdir());
  const parent = await fs.mkdtemp(path.join(tempDirectory, 'native-watcher-move-'));
  const directory = path.join(parent, 'watched');
  const outside = path.join(parent, 'outside');
  const outsideChild = path.join(outside, 'nested', 'existing.txt');
  await fs.mkdir(directory);
  await fs.mkdir(path.dirname(outsideChild), {recursive: true});
  await fs.writeFile(outsideChild, 'existing');

  const collector = new EventCollector();
  const subscription = await watcher.subscribe(directory, collector.callback);
  t.after(async () => {
    await subscription.unsubscribe();
    await fs.rm(parent, {recursive: true, force: true});
  });

  const moved = path.join(directory, 'moved');
  const movedDirectory = path.join(moved, 'nested');
  const movedChild = path.join(movedDirectory, 'existing.txt');
  let mark = collector.mark();
  let waiting = collector.waitFrom(
    mark,
    (events) =>
      events.some((event) => event.type === 'create' && event.path === moved) &&
      events.some(
        (event) => event.type === 'create' && event.path === movedDirectory,
      ) &&
      events.some(
        (event) => event.type === 'create' && event.path === movedChild,
      ),
  );
  await fs.rename(outside, moved);
  let events = await waiting;
  assertEvent(events, 'create', moved);
  assertEvent(events, 'create', movedDirectory);
  assertEvent(events, 'create', movedChild);

  const renamedChild = path.join(movedDirectory, 'renamed.txt');
  mark = collector.mark();
  waiting = collector.waitFrom(
    mark,
    (batch) =>
      batch.some(
        (event) => event.type === 'delete' && event.path === movedChild,
      ) &&
      batch.some(
        (event) => event.type === 'create' && event.path === renamedChild,
      ),
  );
  await fs.rename(movedChild, renamedChild);
  events = await waiting;
  const removed = events.find(
    (event) => event.type === 'delete' && event.path === movedChild,
  );
  const created = events.find(
    (event) => event.type === 'create' && event.path === renamedChild,
  );
  assert.equal(typeof removed.renameId, 'string');
  assert.equal(created.renameId, removed.renameId);

  const newChild = path.join(movedDirectory, 'new.txt');
  mark = collector.mark();
  waiting = collector.waitFor('create', newChild, mark);
  await fs.writeFile(newChild, 'new');
  events = await waiting;
  assertEvent(events, 'create', newChild);
});

test('reports descendant changes below a renamed directory at its new path', async (t) => {
  let oldDirectory;
  let oldChild;
  const {directory, collector} = await createFixture(
    t,
    undefined,
    async (root) => {
      oldDirectory = path.join(root, 'old', 'nested');
      oldChild = path.join(oldDirectory, 'existing');
      await fs.mkdir(oldDirectory, {recursive: true});
      await fs.writeFile(oldChild, 'before');
    },
  );
  const newRoot = path.join(directory, 'new');
  const newChild = path.join(newRoot, 'nested', 'existing');

  let mark = collector.mark();
  let observed = collector.waitFrom(mark, (events) =>
    events.some((event) => event.path === newRoot),
  );
  await fs.rename(path.join(directory, 'old'), newRoot);
  await observed;

  mark = collector.mark();
  observed = collector.waitFrom(mark, (events) =>
    events.some((event) => event.path === oldChild || event.path === newChild),
  );
  await fs.appendFile(newChild, 'after');
  await fs.writeFile(path.join(directory, 'delivery-marker'), 'marker');
  const events = await observed;
  assertEvent(events, 'update', newChild);
  assert.ok(events.every((event) => event.path !== oldChild));
});

test(
  'preserves descendant paths across queued Linux directory renames',
  {skip: process.platform !== 'linux'},
  async () => {
    const fixture = path.join(
      __dirname,
      'fixtures',
      'linux-chained-directory-renames.js',
    );
    const {stdout} = await execFileAsync(process.execPath, [fixture], {
      timeout: 15000,
    });
    assert.match(stdout, /chained directory renames preserve descendant paths/);
  },
);

test(
  'keeps a reused Linux directory path watched after move expiry',
  {skip: process.platform !== 'linux'},
  async () => {
    const fixture = path.join(
      __dirname,
      'fixtures',
      'linux-chained-directory-renames.js',
    );
    const {stdout} = await execFileAsync(process.execPath, [fixture, 'reuse'], {
      timeout: 15000,
    });
    assert.match(stdout, /reused directory path remains watched/);
  },
);

test(
  'watches a Linux subtree that becomes visible after its parent moves',
  {skip: process.platform !== 'linux'},
  async () => {
    const fixture = path.join(
      __dirname,
      'fixtures',
      'linux-chained-directory-renames.js',
    );
    const {stdout} = await execFileAsync(process.execPath, [fixture, 'ignore'], {
      timeout: 15000,
    });
    assert.match(stdout, /renamed directory installs newly visible watches/);
  },
);

test(
  'rewatches a replaced Linux subtree after it becomes visible again',
  {skip: process.platform !== 'linux'},
  async () => {
    const fixture = path.join(
      __dirname,
      'fixtures',
      'linux-chained-directory-renames.js',
    );
    const {stdout} = await execFileAsync(
      process.execPath,
      [fixture, 'ignore-replace'],
      {timeout: 15000},
    );
    assert.match(stdout, /replaced directory installs newly visible watches/);
  },
);

test('stops watching every descendant of a directory moved out of the root', async () => {
  const tempRoot = await fs.realpath(os.tmpdir());
  const parent = await fs.mkdtemp(path.join(tempRoot, 'native-watcher-move-out-'));
  const directory = path.join(parent, 'watched');
  const oldRoot = path.join(directory, 'old');
  const oldChild = path.join(oldRoot, 'nested', 'existing');
  const outsideRoot = path.join(parent, 'outside');
  const outsideChild = path.join(outsideRoot, 'nested', 'existing');
  await fs.mkdir(path.dirname(oldChild), {recursive: true});
  await fs.writeFile(oldChild, 'before');
  const collector = new EventCollector();
  const subscription = await watcher.subscribe(directory, collector.callback);

  try {
    let mark = collector.mark();
    let observed = collector.waitFor('delete', oldRoot, mark);
    await fs.rename(oldRoot, outsideRoot);
    await observed;

    mark = collector.mark();
    const marker = path.join(directory, 'delivery-marker');
    observed = collector.waitFor('create', marker, mark);
    await fs.appendFile(outsideChild, 'after');
    await fs.writeFile(marker, 'marker');
    await observed;
    await settle();

    const events = collector.events.slice(mark);
    assert.ok(events.every((event) => event.path !== oldChild));
    assert.ok(events.every((event) => event.path !== outsideChild));
  } finally {
    await subscription.unsubscribe();
    await fs.rm(parent, {recursive: true, force: true});
  }
});

test('reports symlink creation and deletion without following its target', async (t) => {
  const {directory, collector} = await createFixture(t);
  const target = path.join(directory, 'target.txt');
  const link = path.join(directory, 'link.txt');

  let mark = collector.mark();
  let waiting = collector.waitFor('create', target, mark);
  await fs.writeFile(target, 'content');
  await waiting;

  mark = collector.mark();
  waiting = collector.waitFor('create', link, mark);
  await fs.symlink(target, link, process.platform === 'win32' ? 'file' : undefined);
  assertEvent(await waiting, 'create', link);

  mark = collector.mark();
  waiting = collector.waitFor('delete', link, mark);
  await fs.unlink(link);
  assertEvent(await waiting, 'delete', link);
  assert.equal(await fs.readFile(target, 'utf8'), 'content');
});

test('does not follow a directory link created after subscription', async () => {
  const tempRoot = await fs.realpath(os.tmpdir());
  const parent = await fs.mkdtemp(
    path.join(tempRoot, 'native-watcher-runtime-link-'),
  );
  const directory = path.join(parent, 'watched');
  const outside = path.join(parent, 'outside');
  const outsideFile = path.join(outside, 'external.txt');
  const link = path.join(directory, 'external-link');
  const linkedFile = path.join(link, 'external.txt');
  await fs.mkdir(directory);
  await fs.mkdir(outside);
  await fs.writeFile(outsideFile, 'before');

  const collector = new EventCollector();
  const subscription = await watcher.subscribe(directory, collector.callback);
  try {
    let mark = collector.mark();
    let observed = collector.waitFor('create', link, mark);
    await fs.symlink(
      outside,
      link,
      process.platform === 'win32' ? 'junction' : 'dir',
    );
    await observed;

    mark = collector.mark();
    const marker = path.join(directory, 'delivery-marker');
    observed = collector.waitFor('create', marker, mark);
    await fs.appendFile(outsideFile, 'after');
    await fs.writeFile(marker, 'marker');
    const events = await observed;
    assertNoPath(events, linkedFile);
    assert.ok(events.every((event) => event.path !== outsideFile));
  } finally {
    await subscription.unsubscribe();
    await fs.rm(parent, {recursive: true, force: true});
  }
});

test(
  'keeps descendant paths after a case-only directory rename',
  {skip: !['darwin', 'win32'].includes(process.platform)},
  async () => {
    const directory = await fs.mkdtemp(
      path.join(await fs.realpath(os.tmpdir()), 'native-watcher-dir-case-'),
    );
    const oldDirectory = path.join(directory, 'folder');
    const newDirectory = path.join(directory, 'FOLDER');
    const oldChild = path.join(oldDirectory, 'child.txt');
    const newChild = path.join(newDirectory, 'child.txt');
    await fs.mkdir(oldDirectory);
    await fs.writeFile(oldChild, 'before');

    const collector = new EventCollector();
    const subscription = await watcher.subscribe(directory, collector.callback);
    try {
      let mark = collector.mark();
      let observed = collector.waitFrom(mark, (events) =>
        events.some(
          (event) => event.path === oldDirectory || event.path === newDirectory,
        ),
      );
      await fs.rename(oldDirectory, newDirectory);
      await observed;

      mark = collector.mark();
      const marker = path.join(directory, 'delivery-marker');
      observed = collector.waitFrom(mark, (events) =>
        events.some(
          (event) => event.type === 'update' && event.path === newChild,
        ) && events.some(
          (event) => event.type === 'create' && event.path === marker,
        ),
      );
      await fs.appendFile(newChild, 'after');
      await fs.writeFile(marker, 'marker');
      const events = await observed;
      assertEvent(events, 'update', newChild);
      assert.ok(events.every((event) => event.path !== oldChild));
    } finally {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    }
  },
);

test('supports multiple subscriptions for the same directory', async (t) => {
  const {directory, collector: first, subscription: firstSubscription} =
    await createFixture(t);
  const second = new EventCollector();
  const secondSubscription = await watcher.subscribe(directory, second.callback);
  t.after(() => secondSubscription.unsubscribe());

  const firstFile = path.join(directory, 'first.txt');
  const firstMark = first.mark();
  const secondMark = second.mark();
  const firstWaiting = first.waitFor('create', firstFile, firstMark);
  const secondWaiting = second.waitFor('create', firstFile, secondMark);
  await fs.writeFile(firstFile, 'one');
  await Promise.all([firstWaiting, secondWaiting]);

  await firstSubscription.unsubscribe();
  await firstSubscription.unsubscribe();

  const secondFile = path.join(directory, 'second.txt');
  const afterUnsubscribe = first.mark();
  const stillWaiting = second.waitFor('create', secondFile);
  await fs.writeFile(secondFile, 'two');
  await stillWaiting;
  await settle();
  assert.equal(first.events.slice(afterUnsubscribe).length, 0);
});

test('an immediately unsubscribed subscription stays inactive', async () => {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-immediate-stop-'),
  );
  const retired = new EventCollector();
  const first = await watcher.subscribe(directory, retired.callback);

  try {
    await first.unsubscribe();
    const retainedCount = retired.events.length;
    const active = new EventCollector();
    const second = await watcher.subscribe(directory, active.callback);
    try {
      const file = path.join(directory, 'after-unsubscribe');
      const observed = active.waitFor('create', file);
      await fs.writeFile(file, 'event');
      await observed;
      assert.equal(retired.events.length, retainedCount);
    } finally {
      await second.unsubscribe();
    }
  } finally {
    await first.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
});

test('unsubscribe drains in-flight delivery before resolving', async () => {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-delivery-stop-'),
  );
  const retired = new EventCollector();
  const first = await watcher.subscribe(directory, retired.callback);

  try {
    await fs.writeFile(path.join(directory, 'possibly-in-flight'), 'event');
    await first.unsubscribe();
    const retainedCount = retired.events.length;

    const active = new EventCollector();
    const second = await watcher.subscribe(directory, active.callback);
    try {
      const marker = path.join(directory, 'post-unsubscribe-marker');
      const observed = active.waitFor('create', marker);
      await fs.writeFile(marker, 'marker');
      await observed;
      assert.equal(retired.events.length, retainedCount);
    } finally {
      await second.unsubscribe();
    }
  } finally {
    await first.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
});

test('keeps parent and child root subscriptions independent', async () => {
  const parent = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-parent-root-'),
  );
  const child = path.join(parent, 'child');
  await fs.mkdir(child);
  const parentEvents = new EventCollector();
  const childEvents = new EventCollector();
  const parentSubscription = await watcher.subscribe(
    parent,
    parentEvents.callback,
  );
  const childSubscription = await watcher.subscribe(
    child,
    childEvents.callback,
  );

  try {
    const sharedFile = path.join(child, 'shared.txt');
    const parentObserved = parentEvents.waitFor('create', sharedFile);
    const childObserved = childEvents.waitFor('create', sharedFile);
    await fs.writeFile(sharedFile, 'one');
    await Promise.all([parentObserved, childObserved]);

    await parentSubscription.unsubscribe();
    const parentMark = parentEvents.mark();
    const childOnlyFile = path.join(child, 'child-only.txt');
    const childOnlyObserved = childEvents.waitFor('create', childOnlyFile);
    await fs.writeFile(childOnlyFile, 'two');
    await childOnlyObserved;
    assert.equal(parentEvents.events.length, parentMark);
  } finally {
    await parentSubscription.unsubscribe();
    await childSubscription.unsubscribe();
    await fs.rm(parent, {recursive: true, force: true});
  }
});

test('keeps duplicate callback subscriptions independent', async () => {
  const tempRoot = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(
    path.join(tempRoot, 'native-watcher-duplicate-callback-'),
  );
  const collector = new EventCollector();
  const first = await watcher.subscribe(directory, collector.callback);
  const second = await watcher.subscribe(directory, collector.callback);

  try {
    const before = path.join(directory, 'before-first-unsubscribe');
    let observed = collector.waitFor('create', before);
    await fs.writeFile(before, 'event');
    await observed;

    await first.unsubscribe();

    const after = path.join(directory, 'second-still-active');
    const mark = collector.mark();
    observed = collector.waitFor('create', after, mark);
    await fs.writeFile(after, 'event');
    await observed;
  } finally {
    await first.unsubscribe();
    await second.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
});

test('removes subscriptions when their Worker environment exits', async () => {
  const fixture = path.join(
    __dirname,
    'fixtures',
    'worker-environment-cleanup.js',
  );
  const {stdout} = await execFileAsync(process.execPath, [fixture], {
    timeout: 15000,
  });
  assert.match(stdout, /worker cleanup ok/);
});

test('serializes concurrent subscription registry access', async () => {
  const fixture = path.join(
    __dirname,
    'fixtures',
    'concurrent-subscriptions.js',
  );
  const {stdout} = await execFileAsync(process.execPath, [fixture], {
    timeout: 20000,
  });
  assert.match(stdout, /concurrent subscriptions ok/);
});

test(
  'rejects when the inotify backend cannot initialize',
  {skip: process.platform !== 'linux'},
  async () => {
    const fixture = path.join(
      __dirname,
      'fixtures',
      'inotify-startup-failure.js',
    );
    const {stdout} = await execFileAsync(
      '/bin/sh',
      [
        '-c',
        'ulimit -n 64; exec "$1" "$2"',
        'native-watcher-inotify-test',
        process.execPath,
        fixture,
      ],
      {timeout: 5000},
    );
    assert.match(stdout, /inotify startup failure handled/);
  },
);

test(
  'rejects a Linux FIFO root without blocking other subscriptions',
  {skip: process.platform !== 'linux'},
  async () => {
    const fixture = path.join(__dirname, 'fixtures', 'linux-fifo-root.js');
    const {stdout} = await execFileAsync(process.execPath, [fixture], {
      timeout: 5000,
    });
    assert.match(stdout, /FIFO root rejected without blocking backend/);
  },
);

test(
  'scans directories whose dirent type is unknown on Linux',
  {skip: process.platform !== 'linux'},
  async () => {
    const tempDirectory = await fs.mkdtemp(
      path.join(os.tmpdir(), 'native-watcher-dirent-shim-'),
    );
    try {
      const shim = path.join(tempDirectory, 'unknown-dirent.so');
      await execFileAsync('cc', [
        '-shared',
        '-fPIC',
        '-o',
        shim,
        path.join(__dirname, 'fixtures', 'unknown-dirent.c'),
        '-ldl',
      ]);
      const fixture = path.join(
        __dirname,
        'fixtures',
        'unknown-dirent-scan.js',
      );
      const {stdout} = await execFileAsync(process.execPath, [fixture], {
        env: {...process.env, LD_PRELOAD: shim},
        timeout: 5000,
      });
      assert.match(stdout, /DT_UNKNOWN directory watched/);
    } finally {
      await fs.rm(tempDirectory, {recursive: true, force: true});
    }
  },
);

test(
  'waits for pending Windows directory reads before unsubscribe resolves',
  {skip: process.platform !== 'win32'},
  async () => {
    const directory = await fs.mkdtemp(
      path.join(os.tmpdir(), 'native-watcher-windows-stop-'),
    );
    try {
      for (let index = 0; index < 25; index++) {
        const subscription = await watcher.subscribe(directory, () => {});
        await fs.writeFile(path.join(directory, `event-${index}`), 'event');
        await subscription.unsubscribe();
      }
    } finally {
      await fs.rm(directory, {recursive: true, force: true});
    }
  },
);

test(
  'scans existing Unicode paths on Windows',
  {skip: process.platform !== 'win32'},
  async () => {
    const directory = await fs.mkdtemp(
      path.join(os.tmpdir(), '原生监听-'),
    );
    const nested = path.join(directory, '目录');
    const file = path.join(nested, '已有.txt');
    await fs.mkdir(nested);
    await fs.writeFile(file, 'before');

    const collector = new EventCollector();
    const subscription = await watcher.subscribe(
      directory,
      collector.callback,
    );
    try {
      const observed = collector.waitFor('update', file);
      await fs.writeFile(file, 'after');
      await observed;
    } finally {
      await subscription.unsubscribe();
      await fs.rm(directory, {recursive: true, force: true});
    }
  },
);

test(
  'does not recurse into a Windows junction during startup',
  {skip: process.platform !== 'win32'},
  async () => {
    const parent = await fs.mkdtemp(
      path.join(os.tmpdir(), 'native-watcher-junction-'),
    );
    const directory = path.join(parent, 'watched');
    const junction = path.join(directory, 'parent-link');
    await fs.mkdir(directory);
    await fs.symlink(parent, junction, 'junction');

    const collector = new EventCollector();
    const subscription = await watcher.subscribe(
      directory,
      collector.callback,
    );
    try {
      const visible = path.join(directory, 'visible.txt');
      const observed = collector.waitFor('create', visible);
      await fs.writeFile(visible, 'event');
      await observed;
    } finally {
      await subscription.unsubscribe();
      await fs.rm(parent, {recursive: true, force: true});
    }
  },
);

test(
  'a FIFO event does not block other macOS subscriptions',
  {skip: process.platform !== 'darwin'},
  async () => {
    const tempRoot = await fs.realpath(os.tmpdir());
    const firstDirectory = await fs.mkdtemp(
      path.join(tempRoot, 'native-watcher-fifo-'),
    );
    const secondDirectory = await fs.mkdtemp(
      path.join(tempRoot, 'native-watcher-other-'),
    );
    const first = new EventCollector();
    const second = new EventCollector();
    const firstSubscription = await watcher.subscribe(
      firstDirectory,
      first.callback,
    );
    const secondSubscription = await watcher.subscribe(
      secondDirectory,
      second.callback,
    );
    const fifo = path.join(firstDirectory, 'pipe');

    try {
      await execFileAsync('mkfifo', [fifo]);
      await new Promise((resolve) => setTimeout(resolve, 500));

      const otherFile = path.join(secondDirectory, 'still-responsive');
      const mark = second.mark();
      const observed = second.waitFrom(
        mark,
        (events) => events.some((event) => event.path === otherFile),
        1500,
      );
      await fs.writeFile(otherFile, 'event');
      await observed;
    } finally {
      try {
        const fd = fsSync.openSync(
          fifo,
          fsSync.constants.O_WRONLY | fsSync.constants.O_NONBLOCK,
        );
        fsSync.closeSync(fd);
      } catch (error) {
        if (error.code !== 'ENXIO' && error.code !== 'ENOENT') throw error;
      }
      await Promise.all([
        firstSubscription.unsubscribe(),
        secondSubscription.unsubscribe(),
      ]);
      await Promise.all([
        fs.rm(firstDirectory, {recursive: true, force: true}),
        fs.rm(secondDirectory, {recursive: true, force: true}),
      ]);
    }
  },
);

test(
  'reports updates to a write-only file on macOS',
  {skip: process.platform !== 'darwin'},
  async (t) => {
    let file;
    const {directory, collector} = await createFixture(
      t,
      undefined,
      async (root) => {
        file = path.join(root, 'write-only');
        await fs.writeFile(file, 'before', {mode: 0o200});
      },
    );

    const barrier = path.join(directory, 'barrier');
    let mark = collector.mark();
    let observed = collector.waitFor('create', barrier, mark);
    await fs.writeFile(barrier, 'ready');
    await observed;

    mark = collector.mark();
    observed = collector.waitFor('update', file, mark);
    await fs.appendFile(file, 'after');
    const events = await observed;
    assert.ok(
      events.every((event) => event.type !== 'delete' || event.path !== file),
    );
  },
);

test(
  'applies ignore globs when the macOS root is a symlink',
  {skip: process.platform !== 'darwin'},
  async () => {
    const tempRoot = await fs.realpath(os.tmpdir());
    const parent = await fs.mkdtemp(
      path.join(tempRoot, 'native-watcher-root-alias-'),
    );
    const realDirectory = path.join(parent, 'real');
    const aliasDirectory = path.join(parent, 'alias');
    await fs.mkdir(realDirectory);
    await fs.symlink(realDirectory, aliasDirectory, 'dir');

    const collector = new EventCollector();
    const subscription = await watcher.subscribe(
      aliasDirectory,
      collector.callback,
      {ignore: ['**/*.log', path.join(aliasDirectory, 'ignored.txt')]},
    );
    try {
      const ignoredGlob = path.join(realDirectory, 'ignored.log');
      const ignoredPath = path.join(realDirectory, 'ignored.txt');
      const visible = path.join(realDirectory, 'visible.txt');
      const mark = collector.mark();
      const observed = collector.waitFor('create', visible, mark);
      await fs.writeFile(ignoredGlob, 'ignored');
      await fs.writeFile(ignoredPath, 'ignored');
      await fs.writeFile(visible, 'visible');
      await observed;
      await settle();
      const events = collector.events.slice(mark);
      assertNoPath(events, ignoredGlob);
      assertNoPath(events, ignoredPath);
    } finally {
      await subscription.unsubscribe();
      await fs.rm(parent, {recursive: true, force: true});
    }
  },
);

test(
  'releases macOS stream resources across repeated subscriptions',
  {skip: process.platform !== 'darwin'},
  async () => {
    const tempRoot = await fs.realpath(os.tmpdir());
    const directory = await fs.mkdtemp(
      path.join(tempRoot, 'native-watcher-stream-lifecycle-'),
    );
    try {
      for (let index = 0; index < 25; index++) {
        const subscription = await watcher.subscribe(directory, () => {}, {
          ignore: [`ignored-${index}`],
        });
        await subscription.unsubscribe();
      }

      const collector = new EventCollector();
      const subscription = await watcher.subscribe(
        directory,
        collector.callback,
      );
      try {
        const file = path.join(directory, 'after-repeated-subscriptions');
        const observed = collector.waitFor('create', file);
        await fs.writeFile(file, 'event');
        await observed;
      } finally {
        await subscription.unsubscribe();
      }
    } finally {
      await fs.rm(directory, {recursive: true, force: true});
    }
  },
);

test('keeps different ignore options separate for the same directory', async (t) => {
  let firstExisting;
  let secondExisting;
  const {directory, collector: first} = await createFixture(
    t,
    {ignore: ['first', 'first.txt']},
    async (root) => {
      firstExisting = path.join(root, 'first', 'existing.txt');
      secondExisting = path.join(root, 'second', 'existing.txt');
      await fs.mkdir(path.dirname(firstExisting));
      await fs.mkdir(path.dirname(secondExisting));
      await fs.writeFile(firstExisting, 'one');
      await fs.writeFile(secondExisting, 'one');
    },
  );
  const second = new EventCollector();
  const secondSubscription = await watcher.subscribe(directory, second.callback, {
    ignore: ['second', 'second.txt'],
  });
  t.after(() => secondSubscription.unsubscribe());

  const firstFile = path.join(directory, 'first.txt');
  const secondFile = path.join(directory, 'second.txt');
  const firstMark = first.mark();
  const secondMark = second.mark();
  await fs.writeFile(firstFile, 'first');
  await fs.writeFile(secondFile, 'second');
  await Promise.all([
    first.waitFor('create', secondFile, firstMark),
    second.waitFor('create', firstFile, secondMark),
  ]);
  await settle();

  assertNoPath(first.events.slice(firstMark), firstFile);
  assertNoPath(second.events.slice(secondMark), secondFile);

  const firstUpdateMark = first.mark();
  const secondUpdateMark = second.mark();
  await fs.writeFile(firstExisting, 'two');
  await fs.writeFile(secondExisting, 'two');
  await Promise.all([
    first.waitFor('update', secondExisting, firstUpdateMark),
    second.waitFor('update', firstExisting, secondUpdateMark),
  ]);
  assertNoPath(first.events.slice(firstUpdateMark), path.dirname(firstExisting));
  assertNoPath(
    second.events.slice(secondUpdateMark),
    path.dirname(secondExisting),
  );
});

test('rejects a missing path and a file path', async (t) => {
  const tempDirectory = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(
    path.join(tempDirectory, 'native-watcher-errors-'),
  );
  t.after(() => fs.rm(directory, {recursive: true, force: true}));

  const missing = path.join(directory, 'missing');
  const callback = () => {};
  await assert.rejects(watcher.subscribe(missing, callback));

  await fs.mkdir(missing);
  const recovered = await watcher.subscribe(missing, callback);
  await recovered.unsubscribe();

  const file = path.join(directory, 'file.txt');
  await fs.writeFile(file, 'content');
  await assert.rejects(watcher.subscribe(file, () => {}));
});

test('rejects a non-function callback', async (t) => {
  const {directory} = await createFixture(t);
  await assert.rejects(
    watcher.subscribe(directory, null),
    {name: 'TypeError', message: 'Expected a function'},
  );
});

async function assertIgnored(t, options, ignoredRelativePath, mutateIgnored) {
  const {directory, collector} = await createFixture(t, options);
  const ignoredPath = path.join(directory, ignoredRelativePath);
  const visiblePath = path.join(directory, 'visible.txt');
  const mark = collector.mark();

  await mutateIgnored(ignoredPath);
  const visible = collector.waitFor('create', visiblePath, mark);
  await fs.writeFile(visiblePath, 'visible');
  await visible;
  await settle();

  const events = collector.events.slice(mark);
  assertEvent(events, 'create', visiblePath);
  assertNoPath(events, ignoredPath);
}

test('ignore accepts a relative file path', async (t) => {
  await assertIgnored(t, {ignore: ['ignored.txt']}, 'ignored.txt', (file) =>
    fs.writeFile(file, 'ignored'),
  );
});

test('ignore accepts an absolute file path', async (t) => {
  let absolutePath;
  const tempDirectory = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(
    path.join(tempDirectory, 'native-watcher-ignore-'),
  );
  absolutePath = path.join(directory, 'ignored.txt');
  const collector = new EventCollector();
  const subscription = await watcher.subscribe(directory, collector.callback, {
    ignore: [absolutePath],
  });
  t.after(async () => {
    await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  });

  const mark = collector.mark();
  await fs.writeFile(absolutePath, 'ignored');
  const visiblePath = path.join(directory, 'visible.txt');
  await fs.writeFile(visiblePath, 'visible');
  await collector.waitFor('create', visiblePath, mark);
  await settle();
  assertNoPath(collector.events.slice(mark), absolutePath);
});

test('ignoring a directory also ignores its descendants', async (t) => {
  await assertIgnored(t, {ignore: ['ignored']}, 'ignored', async (directory) => {
    await fs.mkdir(path.join(directory, 'nested'), {recursive: true});
    await fs.writeFile(path.join(directory, 'nested', 'file.txt'), 'ignored');
  });
});

test('an ignored directory present at startup is not watched', async (t) => {
  let ignoredDirectory;
  let ignoredFile;
  const {directory, collector} = await createFixture(
    t,
    {ignore: ['ignored']},
    async (root) => {
      ignoredDirectory = path.join(root, 'ignored');
      ignoredFile = path.join(ignoredDirectory, 'file.txt');
      await fs.mkdir(ignoredDirectory);
      await fs.writeFile(ignoredFile, 'one');
    },
  );
  const visible = path.join(directory, 'visible.txt');
  const mark = collector.mark();
  await fs.writeFile(ignoredFile, 'two');
  const waiting = collector.waitFor('create', visible, mark);
  await fs.writeFile(visible, 'visible');
  await waiting;
  await settle();
  assertNoPath(collector.events.slice(mark), ignoredDirectory);
});

test('ignore accepts glob patterns relative to the watched directory', async (t) => {
  const {directory, collector} = await createFixture(t, {
    ignore: ['**/*.ignore', 'ignored/**'],
  });
  const ignoredFile = path.join(directory, 'nested', 'file.ignore');
  const ignoredDirectory = path.join(directory, 'ignored');
  const ignoredChild = path.join(ignoredDirectory, 'file.txt');
  const visibleFile = path.join(directory, 'nested', 'file.txt');
  await fs.mkdir(path.dirname(ignoredFile), {recursive: true});
  await fs.mkdir(ignoredDirectory);

  const mark = collector.mark();
  await fs.writeFile(ignoredFile, 'ignored');
  await fs.writeFile(ignoredChild, 'ignored');
  const visible = collector.waitFor('create', visibleFile, mark);
  await fs.writeFile(visibleFile, 'visible');
  await visible;
  await settle();

  const events = collector.events.slice(mark);
  assertNoPath(events, ignoredFile);
  assertNoPath(events, ignoredDirectory);
  assertEvent(events, 'create', visibleFile);
});

test('ignore accepts RegExp patterns', async (t) => {
  const {directory, collector} = await createFixture(t, {
    ignore: [/\.generated$/, /node_modules/],
  });
  const generated = path.join(directory, 'file.generated');
  const dependency = path.join(directory, 'node_modules', 'pkg', 'index.js');
  const visible = path.join(directory, 'file.js');
  await fs.mkdir(path.dirname(dependency), {recursive: true});

  const mark = collector.mark();
  await fs.writeFile(generated, 'ignored');
  await fs.writeFile(dependency, 'ignored');
  const waiting = collector.waitFor('create', visible, mark);
  await fs.writeFile(visible, 'visible');
  await waiting;
  await settle();

  const events = collector.events.slice(mark);
  assertNoPath(events, generated);
  assertNoPath(events, path.join(directory, 'node_modules'));
  assertEvent(events, 'create', visible);
});

test('ignore rejects RegExp flags', async (t) => {
  const tempDirectory = await fs.realpath(os.tmpdir());
  const directory = await fs.mkdtemp(
    path.join(tempDirectory, 'native-watcher-options-'),
  );
  t.after(() => fs.rm(directory, {recursive: true, force: true}));

  await assert.rejects(
    watcher.subscribe(directory, () => {}, {ignore: [/ignored/i]}),
    /cannot use flags/,
  );
});

test('invalid native RegExp syntax does not leave a subscription running', async () => {
  const fixture = path.join(
    __dirname,
    'fixtures',
    'invalid-regex-cleanup.js',
  );
  const {stdout} = await execFileAsync(process.execPath, [fixture], {
    timeout: 5000,
  });
  assert.match(stdout, /invalid regex rejected cleanly/);
});

test('a move across an ignore boundary is not reported as a rename pair', async (t) => {
  const {directory, collector} = await createFixture(t, {ignore: ['ignored']});
  const ignoredDirectory = path.join(directory, 'ignored');
  const ignoredFile = path.join(ignoredDirectory, 'inside.txt');
  const visibleFile = path.join(directory, 'visible.txt');
  await fs.mkdir(ignoredDirectory);
  await fs.writeFile(ignoredFile, 'content');

  let mark = collector.mark();
  let waiting = collector.waitFor('create', visibleFile, mark);
  await fs.rename(ignoredFile, visibleFile);
  let events = await waiting;
  const created = events.find(
    (event) => event.type === 'create' && event.path === visibleFile,
  );
  assert.equal(created.renameId, undefined);
  assertNoPath(events, ignoredDirectory);

  mark = collector.mark();
  waiting = collector.waitFor('delete', visibleFile, mark);
  await fs.rename(visibleFile, ignoredFile);
  events = await waiting;
  const removed = events.find(
    (event) => event.type === 'delete' && event.path === visibleFile,
  );
  assert.equal(removed.renameId, undefined);
  assertNoPath(events, ignoredDirectory);
});
