{
  "targets": [
    {
      "target_name": "native_watcher",
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS", "NAPI_VERSION=8"],
      "sources": [
        "src/binding.cc",
        "src/Watcher.cc",
        "src/Backend.cc",
        "src/DirTree.cc",
        "src/CaseSensitivity.cc",
        "src/Glob.cc",
        "src/Debounce.cc"
      ],
      "include_dirs": ["<!(node -p \"require('node-addon-api').include_dir\")"],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "cflags_cc": ["-std=c++17", "-fexceptions"],
      "conditions": [
        ["OS=='mac'", {
          "sources": [
            "src/macos/FSEventsBackend.cc",
            "src/macos/IdentityIndex.cc"
          ],
          "link_settings": {
            "libraries": ["CoreServices.framework"]
          },
          "defines": ["FS_EVENTS"],
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "GCC_SYMBOLS_PRIVATE_EXTERN": "YES",
            "GCC_INLINES_ARE_PRIVATE_EXTERN": "YES",
            "DEAD_CODE_STRIPPING": "YES"
          }
        }],
        ["OS=='linux'", {
          "sources": [
            "src/shared/BruteForceBackend.cc",
            "src/linux/InotifyBackend.cc",
            "src/unix/legacy.cc"
          ],
          "cflags": ["-fvisibility=hidden", "-ffunction-sections", "-fdata-sections"],
          "cflags_cc": ["-fvisibility-inlines-hidden"],
          "ldflags": [
            "-static-libstdc++",
            "-Wl,-Bsymbolic",
            "-Wl,--exclude-libs,ALL",
            "-Wl,--gc-sections"
          ],
          "defines": ["INOTIFY"]
        }],
        ["OS=='win'", {
          "sources": [
            "src/shared/BruteForceBackend.cc",
            "src/windows/WindowsBackend.cc",
            "src/windows/win_utils.cc"
          ],
          "defines": ["WINDOWS"],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions!": ["-std:c++20"],
              "AdditionalOptions": ["-std:c++17", "/W3"]
            }
          }
        }]
      ]
    }
  ]
}
