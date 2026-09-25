'use strict';

const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {execFile} = require('node:child_process');
const {promisify} = require('node:util');
const execFileAsync = promisify(execFile);

test('reindexes macOS replacements and preserves metadata updates during rescans',
  {skip: process.platform !== 'darwin', timeout: 30_000}, async () => {
    const directory = await fs.mkdtemp(path.join(
      await fs.realpath(os.tmpdir()), 'native-watcher-type-replacement-',
    ));
    try {
      const sourceRoot = path.join(__dirname, '..', 'src');
      const binary = path.join(directory, 'type-replacement');
      await execFileAsync('c++', [
        '-std=c++17', '-DFS_EVENTS', '-ffunction-sections',
        '-fdata-sections', '-Wl,-dead_strip',
        '-framework', 'CoreServices',
        `-I${require('node-addon-api').include.replace(/^"|"$/g, '')}`,
        `-I${path.resolve(process.execPath, '..', '..', 'include', 'node')}`,
        `-I${sourceRoot}`,
        path.join(__dirname, 'fixtures', 'macos-type-replacement.cc'),
        path.join(sourceRoot, 'DirTree.cc'),
        path.join(sourceRoot, 'macos', 'IdentityIndex.cc'),
        '-o', binary,
      ]);
      await execFileAsync(binary, [directory], {timeout: 5_000});
    } finally {
      await fs.rm(directory, {recursive: true, force: true});
    }
  });
