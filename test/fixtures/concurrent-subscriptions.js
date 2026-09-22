'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');

async function subscribeRepeatedly(directory) {
  for (let index = 0; index < 20; index++) {
    const subscription = await watcher.subscribe(directory, () => {});
    await subscription.unsubscribe();
  }
}

async function main() {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-concurrent-'),
  );
  try {
    await Promise.all(Array.from(
      {length: 4},
      () => subscribeRepeatedly(directory),
    ));
    const eventPath = path.join(directory, 'after-concurrency');
    let resolveEvent;
    const observed = new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error('final subscription received no event')),
        3000,
      );
      resolveEvent = () => {
        clearTimeout(timer);
        resolve();
      };
    });
    const subscription = await watcher.subscribe(directory, (error, events) => {
      if (error) throw error;
      if (events.some((event) => event.path === eventPath)) resolveEvent();
    });
    try {
      await fs.writeFile(eventPath, 'event');
      await observed;
    } finally {
      await subscription.unsubscribe();
    }
  } finally {
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('concurrent subscriptions ok'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);
