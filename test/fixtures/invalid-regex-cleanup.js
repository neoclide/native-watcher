'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const watcher = require('../..');

async function main() {
  const directory = await fs.mkdtemp(
    path.join(await fs.realpath(os.tmpdir()), 'native-watcher-regex-'),
  );
  try {
    await assert.rejects(
      watcher.subscribe(directory, () => {}, {ignore: [/(?<=x)y/]}),
    );
  } finally {
    await fs.rm(directory, {recursive: true, force: true});
  }
}

main().then(
  () => console.log('invalid regex rejected cleanly'),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  },
);
