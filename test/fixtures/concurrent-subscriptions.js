'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const {Worker} = require('node:worker_threads');
const watcher = require('../..');

const packageRoot = path.resolve(__dirname, '..', '..');

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
    const workers = Array.from({length: 3}, () => new Worker(
      `
        const {parentPort, workerData} = require('node:worker_threads');
        const watcher = require(workerData.packageRoot);
        (async () => {
          for (let index = 0; index < 20; index++) {
            const subscription = await watcher.subscribe(
              workerData.directory,
              () => {},
            );
            await subscription.unsubscribe();
          }
          parentPort.postMessage('done');
        })().catch((error) => { throw error; });
      `,
      {eval: true, workerData: {directory, packageRoot}},
    ));
    await Promise.all([
      subscribeRepeatedly(directory),
      ...workers.map((worker) => new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
      })),
    ]);

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
