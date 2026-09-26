'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {fork} = require('node:child_process');
const {once} = require('node:events');
const binding = require('../../build/Release/native_watcher.node');

async function waitFor(predicate, description) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started >= 5000) {
      throw new Error(`timed out waiting for ${description}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

async function runChild(root) {
  const callback = (error, events) => {
    process.send({error: error?.message, events});
  };
  await binding.subscribe(root, callback, {});
  process.on('message', async () => {
    await binding.unsubscribe(root, callback, {});
    process.disconnect();
  });
  process.send({ready: true});
}

async function runParent() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'native-watcher-atomic-'));
  const target = path.join(root, 'target.txt');
  const temporary = path.join(root, '.target.tmp');
  fs.writeFileSync(target, 'before');
  const child = fork(__filename, ['child', root], {
    stdio: ['ignore', 'inherit', 'inherit', 'ipc'],
  });
  const exited = once(child, 'exit');
  const messages = [];
  child.on('message', (message) => messages.push(message));
  try {
    await waitFor(() => messages.some((message) => message.ready), 'subscriber');
    child.kill('SIGSTOP');
    await waitFor(
      () => /State:\s+T/.test(fs.readFileSync(`/proc/${child.pid}/status`, 'utf8')),
      'subscriber to stop',
    );
    // Queue creation and replacement before inotify can index the temporary file.
    fs.writeFileSync(temporary, 'after');
    fs.renameSync(temporary, target);
    child.kill('SIGCONT');
    await waitFor(
      () => messages.some((message) => message.error ||
        message.events?.some((event) => event.path === target)),
      'replacement event',
    );
    assert.ok(messages.every((message) => !message.error), JSON.stringify(messages));
    const events = messages.flatMap((message) => message.events ?? []);
    assert.deepEqual(events.filter((event) => event.path === target), [
      {path: target, type: 'update', kind: 'file'},
    ]);
    assert.ok(events.every((event) => event.renameId === undefined));
  } finally {
    child.kill('SIGCONT');
    if (child.connected) child.send('close');
    try {
      await waitFor(
        () => child.exitCode !== null || child.signalCode !== null,
        'subscriber to exit',
      );
    } finally {
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
      await exited;
      fs.rmSync(root, {recursive: true, force: true});
    }
  }
}

(process.argv[2] === 'child' ? runChild(process.argv[3]) : runParent()).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
