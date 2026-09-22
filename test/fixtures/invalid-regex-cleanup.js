'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');

async function main() {
  await assert.rejects(
    watcher.subscribe(await fs.realpath(os.tmpdir()), () => {}, {
      ignore: [/(?<=x)y/],
    }),
  );
  if (process.platform !== 'darwin') return;
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-regex-'),
  );
  const failures = [];
  const subscription = await watcher.subscribe(
    directory,
    (error) => {
      if (error) failures.push(error);
    },
    {ignore: [/^(a+)+$/]},
  );
  try {
    const pathological = `${'a'.repeat(32)}!`;
    await fs.writeFile(path.join(directory, pathological), 'trigger');
    const deadline = Date.now() + 3000;
    while (failures.length === 0 && Date.now() < deadline) {
      await new Promise((resolve) => setImmediate(resolve));
    }
    assert.equal(failures.length, 1, 'expected one runtime regex error');
    await subscription.unsubscribe();

    const nextDirectory = await fs.mkdtemp(
      path.join(await fs.realpath(os.tmpdir()), 'native-watcher-regex-next-'),
    );
    try {
      let resolveEvent;
      const received = new Promise((resolve) => {
        resolveEvent = resolve;
      });
      const next = await watcher.subscribe(nextDirectory, (error, events) => {
        if (error) throw error;
        if (events.some((event) => event.path === path.join(nextDirectory, 'ok')))
          resolveEvent();
      });
      try {
        await fs.writeFile(path.join(nextDirectory, 'ok'), 'ok');
        let timer;
        try {
          await Promise.race([
            received,
            new Promise((_, reject) => {
              timer = setTimeout(
                () => reject(new Error('subsequent subscription stalled')),
                3000,
              );
            }),
          ]);
        } finally {
          clearTimeout(timer);
        }
      } finally {
        await next.unsubscribe();
      }
    } finally {
      await fs.rm(nextDirectory, {recursive: true, force: true});
    }
  } finally {
    await subscription.unsubscribe();
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('runtime regex failure cleanup ok'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);
