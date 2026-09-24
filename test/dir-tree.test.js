'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const execFileAsync = promisify(execFile);

test('round-trips DirTree entries whose paths start with digits',
  {skip: process.platform === 'win32'}, async () => {
    const directory = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-dir-tree-',
    ));
    try {
      const sourceRoot = path.join(__dirname, '..', 'src');
      const binary = path.join(directory, 'dir-tree-serialization');
      await execFileAsync('c++', [
        '-std=c++17', '-ffunction-sections', '-fdata-sections',
        process.platform === 'darwin' ? '-Wl,-dead_strip' : '-Wl,--gc-sections',
        `-I${require('node-addon-api').include.replace(/^"|"$/g, '')}`,
        `-I${path.resolve(process.execPath, '..', '..', 'include', 'node')}`,
        `-I${sourceRoot}`,
        path.join(__dirname, 'fixtures', 'dir-tree-serialization.cc'),
        path.join(sourceRoot, 'DirTree.cc'),
        '-o', binary,
      ]);
      await execFileAsync(binary);
    } finally {
      await fs.rm(directory, {recursive: true, force: true});
    }
  });
