'use strict';

const fs = require('node:fs/promises');
const path = require('path');

function normalizeOptions(directory, options = {}, inputDirectory = directory) {
  const {ignore, ...nativeOptions} = options;

  if (!Array.isArray(ignore)) return nativeOptions;

  for (const value of ignore) {
    if (typeof value !== 'string') {
      throw new TypeError('Expected ignore patterns to be strings');
    }
    if (value.includes('\0')) {
      throw new TypeError('ignore patterns cannot contain NUL characters');
    }
    if (value.includes('*')) {
      (nativeOptions.ignoreGlobs ??= []).push(value);
      continue;
    }
    const absolutePath = path.resolve(inputDirectory, value);
    const relativePath = path.relative(inputDirectory, absolutePath);
    const isInsideRoot = relativePath === '' || (
      relativePath !== '..' &&
      !relativePath.startsWith(`..${path.sep}`) &&
      !path.isAbsolute(relativePath)
    );
    if (isInsideRoot && relativePath !== '') {
      (nativeOptions.ignoreGlobs ??= []).push(relativePath);
    } else {
      (nativeOptions.ignorePaths ??= []).push(
        isInsideRoot ? directory : absolutePath,
      );
    }
  }

  return nativeOptions;
}

exports.createWrapper = (binding) => ({
  async subscribe(directory, callback, options) {
    if (typeof callback !== 'function') {
      throw new TypeError('Expected a function');
    }
    const absoluteDirectory = path.resolve(directory);
    const watchedDirectory = process.platform === 'darwin'
      ? await fs.realpath(absoluteDirectory)
      : absoluteDirectory;
    const nativeOptions = normalizeOptions(
      watchedDirectory,
      options,
      absoluteDirectory,
    );
    const nativeCallback = (error, events) => callback(error, events);
    await binding.subscribe(watchedDirectory, nativeCallback, nativeOptions);

    let unsubscribePromise;
    return {
      unsubscribe() {
        if (!unsubscribePromise) {
          try {
            unsubscribePromise = Promise.resolve(binding.unsubscribe(
              watchedDirectory,
              nativeCallback,
              nativeOptions,
            ));
          } catch (error) {
            unsubscribePromise = Promise.reject(error);
          }
        }
        return unsubscribePromise;
      },
    };
  },
});
