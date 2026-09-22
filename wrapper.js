'use strict';

const fs = require('node:fs/promises');
const path = require('path');
const isGlob = require('is-glob');
const picomatch = require('picomatch');

function normalizeOptions(directory, options = {}, inputDirectory = directory) {
  const {ignore, ...nativeOptions} = options;

  if (!Array.isArray(ignore)) return nativeOptions;

  for (const value of ignore) {
    if (value instanceof RegExp) {
      if (value.flags !== '') {
        throw new Error('RegExp ignore patterns cannot use flags');
      }
      (nativeOptions.ignoreGlobs ??= []).push(
        `^[\\s\\S]*(?:${value.source})[\\s\\S]*$`,
      );
    } else if (isGlob(value)) {
      const regex = picomatch.makeRe(value, {
        dot: true,
        windows: process.platform === 'win32',
      });
      (nativeOptions.ignoreGlobs ??= []).push(regex.source);
    } else {
      const absolutePath = path.resolve(inputDirectory, value);
      const relativePath = path.relative(inputDirectory, absolutePath);
      const isInsideRoot = relativePath === '' || (
        relativePath !== '..' &&
        !relativePath.startsWith(`..${path.sep}`) &&
        !path.isAbsolute(relativePath)
      );
      (nativeOptions.ignorePaths ??= []).push(
        isInsideRoot
          ? path.resolve(directory, relativePath)
          : absolutePath,
      );
    }
  }

  return nativeOptions;
}

exports.createWrapper = (binding) => ({
  async subscribe(directory, callback, options) {
    const absoluteDirectory = path.resolve(directory);
    const watchedDirectory = process.platform === 'darwin'
      ? await fs.realpath(absoluteDirectory)
      : absoluteDirectory;
    const nativeOptions = normalizeOptions(
      watchedDirectory,
      options,
      absoluteDirectory,
    );
    await binding.subscribe(watchedDirectory, callback, nativeOptions);

    let active = true;
    return {
      async unsubscribe() {
        if (!active) return;
        active = false;
        await binding.unsubscribe(
          watchedDirectory,
          callback,
          nativeOptions,
        );
      },
    };
  },
});
