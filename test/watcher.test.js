'use strict';

const assert = require('node:assert/strict');
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

  await assert.rejects(
    watcher.subscribe(path.join(directory, 'missing'), () => {}),
  );

  const file = path.join(directory, 'file.txt');
  await fs.writeFile(file, 'content');
  await assert.rejects(watcher.subscribe(file, () => {}));
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
