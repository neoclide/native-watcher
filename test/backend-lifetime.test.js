'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');

const execFileAsync = promisify(execFile);

test('keeps a backend alive through terminal watcher-error cleanup',
  {skip: process.platform === 'win32', timeout: 30_000}, async () => {
    const directory = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-backend-lifetime-',
    ));
    try {
      const sourceRoot = path.join(__dirname, '..', 'src');
      const binary = path.join(directory, 'backend-lifetime');
      const linkerArgs = process.platform === 'darwin'
        ? ['-Wl,-dead_strip']
        : ['-Wl,--gc-sections'];
      await execFileAsync('c++', [
        '-std=c++17', '-pthread', '-ffunction-sections', '-fdata-sections',
        ...linkerArgs,
        `-I${require('node-addon-api').include.replace(/^"|"$/g, '')}`,
        `-I${path.resolve(process.execPath, '..', '..', 'include', 'node')}`,
        `-I${sourceRoot}`,
        path.join(__dirname, 'fixtures', 'backend-lifetime.cc'),
        '-o', binary,
      ]);
      await execFileAsync(binary, [], {timeout: 5_000});
    } finally {
      await fs.rm(directory, {recursive: true, force: true});
    }
  });
