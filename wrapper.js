'use strict';

const path = require('path');
const isGlob = require('is-glob');
const picomatch = require('picomatch');

function normalizeOptions(directory, options = {}) {
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
      (nativeOptions.ignorePaths ??= []).push(path.resolve(directory, value));
    }
  }

  return nativeOptions;
}

exports.createWrapper = (binding) => ({
  async subscribe(directory, callback, options) {
    const absoluteDirectory = path.resolve(directory);
    const nativeOptions = normalizeOptions(absoluteDirectory, options);
    await binding.subscribe(absoluteDirectory, callback, nativeOptions);

    let active = true;
    return {
      async unsubscribe() {
        if (!active) return;
        active = false;
        await binding.unsubscribe(
          absoluteDirectory,
          callback,
          nativeOptions,
        );
      },
    };
  },
});
