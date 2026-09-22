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

async function runChild(root) {
  const subscription = await watcher.subscribe(root, (error, events) => {
    process.send({error: error?.message, events});
  });
  process.send({ready: true});
  process.on('message', async (message) => {
    if (message === 'close') {
      await subscription.unsubscribe();
      process.disconnect();
    }
  });
}

async function runParent() {
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

  const child = fork(__filename, ['child', root], {
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

    await fs.rename(oldRoot, middleRoot);
    await fs.rename(middleRoot, newRoot);
    child.kill('SIGCONT');
    await delay(200);

    const mark = messages.length;
    await fs.appendFile(newChild, 'after');
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
      events.some((event) => event.type === 'update' && event.path === newChild),
      `missing update for ${newChild}: ${JSON.stringify(events)}`,
    );
    assert.ok(
      events.every((event) => event.path !== oldChild),
      `received stale path ${oldChild}: ${JSON.stringify(events)}`,
    );
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
  runChild(process.argv[3]).catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
} else {
  runParent().then(
    () => console.log('chained directory renames preserve descendant paths'),
    (error) => {
      console.error(error);
      process.exitCode = 1;
    },
  );
}
