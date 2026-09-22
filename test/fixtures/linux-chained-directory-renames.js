'use strict';

const assert = require('node:assert/strict');
const fsSync = require('node:fs');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {fork} = require('node:child_process');
const watcher = require('../..');

const delay = (milliseconds) =>
  new Promise((resolve) => setTimeout(resolve, milliseconds));

async function waitFor(predicate, describe, timeout = 5000) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started >= timeout) {
      throw new Error(`timed out waiting for ${describe}`);
    }
    await delay(10);
  }
}

async function runChild(root, options) {
  const subscription = await watcher.subscribe(root, (error, events) => {
    process.send({error: error?.message, events});
  }, options);
  process.send({ready: true});
  process.on('message', async (message) => {
    if (message === 'close') {
      await subscription.unsubscribe();
      process.disconnect();
    }
  });
}

async function runParent(mode) {
  const parent = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-chain-'),
  );
  const root = path.join(parent, 'root');
  const oldRoot = path.join(root, 'old');
  const middleRoot = path.join(root, 'middle');
  const newRoot = path.join(root, 'new');
  const oldChild = path.join(oldRoot, 'nested', 'existing');
  const newChild = path.join(newRoot, 'nested', 'existing');
  const marker = path.join(root, 'delivery-marker');
  await fs.mkdir(path.dirname(oldChild), {recursive: true});
  await fs.writeFile(oldChild, 'before');

  const options = mode === 'ignore' ? {ignore: ['old/nested']} : {};
  const child = fork(__filename, ['child', root, JSON.stringify(options)], {
    stdio: ['ignore', 'inherit', 'inherit', 'ipc'],
  });
  const messages = [];
  child.on('message', (message) => messages.push(message));

  try {
    await waitFor(() => messages.some((message) => message.ready), 'subscriber');
    child.kill('SIGSTOP');
    await waitFor(
      () => /State:\s+T/.test(
        fsSync.readFileSync(`/proc/${child.pid}/status`, 'utf8'),
      ),
      'subscriber to stop',
    );

    const operationMark = messages.length;
    if (mode === 'reuse') {
      await fs.rename(oldRoot, path.join(parent, 'outside'));
      await fs.mkdir(path.dirname(oldChild), {recursive: true});
      await fs.writeFile(oldChild, 'replacement');
    } else if (mode === 'chain') {
      await fs.rename(oldRoot, middleRoot);
      await fs.rename(middleRoot, newRoot);
    } else {
      await fs.rename(oldRoot, newRoot);
    }
    child.kill('SIGCONT');
    await delay(200);

    const mark = messages.length;
    const expectedChild = mode === 'reuse' ? oldChild : newChild;
    await fs.appendFile(expectedChild, 'after');
    await fs.writeFile(marker, 'marker');
    await waitFor(
      () => messages.slice(mark).some((message) =>
        message.events?.some((event) => event.path === marker),
      ),
      'delivery marker',
    );

    const events = messages
      .slice(mark)
      .flatMap((message) => message.events ?? []);
    assert.ok(
      events.some(
        (event) => event.type === 'update' && event.path === expectedChild,
      ),
      `missing update for ${expectedChild}: ${JSON.stringify(events)}`,
    );
    if (mode === 'reuse') {
      const operationEvents = messages
        .slice(operationMark)
        .flatMap((message) => message.events ?? []);
      assert.ok(
        operationEvents.some(
          (event) => event.type === 'create' && event.path === oldRoot,
        ),
        `missing create for reused path ${oldRoot}: ${JSON.stringify(operationEvents)}`,
      );
      assert.ok(
        operationEvents.every(
          (event) => event.type !== 'delete' || event.path !== oldRoot,
        ),
        `received stale delete for reused path ${oldRoot}: ${JSON.stringify(operationEvents)}`,
      );
    }
    if (mode !== 'reuse') {
      assert.ok(
        events.every((event) => event.path !== oldChild),
        `received stale path ${oldChild}: ${JSON.stringify(events)}`,
      );
    }
    assert.ok(
      messages.every((message) => !message.error),
      `watcher error: ${JSON.stringify(messages)}`,
    );
  } finally {
    child.kill('SIGCONT');
    if (child.connected) child.send('close');
    await waitFor(
      () => child.exitCode !== null || child.signalCode !== null,
      'subscriber to exit',
    ).catch(() => child.kill('SIGKILL'));
    await fs.rm(parent, {recursive: true, force: true});
  }
}

if (process.argv[2] === 'child') {
  runChild(process.argv[3], JSON.parse(process.argv[4])).catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
} else {
  const mode = process.argv[2] ?? 'chain';
  runParent(mode).then(
    () => console.log(
      mode === 'reuse'
        ? 'reused directory path remains watched'
        : mode === 'ignore'
          ? 'renamed directory installs newly visible watches'
          : 'chained directory renames preserve descendant paths',
    ),
    (error) => {
      console.error(error);
      process.exitCode = 1;
    },
  );
}
