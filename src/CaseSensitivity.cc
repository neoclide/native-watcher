#include "CaseSensitivity.hh"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifdef FS_EVENTS
#include <sys/attr.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <unistd.h>
#elif defined(WINDOWS)
#include <windows.h>
#include "windows/win_utils.hh"
#elif defined(INOTIFY)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifndef FS_CASEFOLD_FL
#define FS_CASEFOLD_FL 0x40000000
#endif
#endif

namespace {

#if defined(FS_EVENTS) || defined(INOTIFY)
bool isUnavailableError(int error) {
#ifdef FS_EVENTS
  return error == ENOENT || error == ENOTDIR || error == EOPNOTSUPP ||
    error == ENOSYS || error == EINVAL;
#elif defined(INOTIFY)
  return error == ENOENT || error == ENOTDIR || error == ELOOP ||
    error == ENOTTY || error == EOPNOTSUPP || error == EINVAL;
#endif
}
#endif

}

DirectoryCaseSensitivity directoryCaseSensitivity(const std::string &path) {
#ifdef FS_EVENTS
  struct attrlist attributes {};
  attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
  attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
  struct Result {
    uint32_t length;
    vol_capabilities_attr_t capabilities;
  } result {};
  if (getattrlist(path.c_str(), &attributes, &result, sizeof(result), FSOPT_NOFOLLOW) != 0) {
    if (isUnavailableError(errno)) return DirectoryCaseSensitivity::Unknown;
    throw std::runtime_error("Unable to determine directory case sensitivity for " +
      path + ": " + std::strerror(errno));
  }
  uint32_t valid = result.capabilities.valid[VOL_CAPABILITIES_FORMAT];
  if ((valid & VOL_CAP_FMT_CASE_SENSITIVE) == 0) {
    return DirectoryCaseSensitivity::Unknown;
  }
  return (result.capabilities.capabilities[VOL_CAPABILITIES_FORMAT] &
    VOL_CAP_FMT_CASE_SENSITIVE) != 0
    ? DirectoryCaseSensitivity::Sensitive
    : DirectoryCaseSensitivity::Insensitive;
#elif defined(WINDOWS)
  std::wstring widePath = utf8ToUtf16(path);
  HANDLE handle = CreateFileW(
    widePath.c_str(), FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr
  );
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_DIRECTORY) {
      return DirectoryCaseSensitivity::Unknown;
    }
    throw std::runtime_error("Unable to determine directory case sensitivity for " +
      path + " (Windows error " + std::to_string(error) + ")");
  }
  FILE_CASE_SENSITIVE_INFO info {};
  BOOL ok = GetFileInformationByHandleEx(
    handle, FileCaseSensitiveInfo, &info, sizeof(info)
  );
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!ok) {
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_CALL_NOT_IMPLEMENTED) {
      return DirectoryCaseSensitivity::Unknown;
    }
    throw std::runtime_error("Unable to determine directory case sensitivity for " +
      path + " (Windows error " + std::to_string(error) + ")");
  }
  return (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0
    ? DirectoryCaseSensitivity::Sensitive
    : DirectoryCaseSensitivity::Insensitive;
#elif defined(INOTIFY)
  int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    if (isUnavailableError(errno)) return DirectoryCaseSensitivity::Unknown;
    throw std::runtime_error("Unable to determine directory case sensitivity for " +
      path + ": " + std::strerror(errno));
  }
  int flags = 0;
  int result = ioctl(descriptor, FS_IOC_GETFLAGS, &flags);
  int error = result == 0 ? 0 : errno;
  close(descriptor);
  if (result != 0) {
    if (isUnavailableError(error)) return DirectoryCaseSensitivity::Unknown;
    throw std::runtime_error("Unable to determine directory case sensitivity for " +
      path + ": " + std::strerror(error));
  }
  return (flags & FS_CASEFOLD_FL) != 0
    ? DirectoryCaseSensitivity::Insensitive
    : DirectoryCaseSensitivity::Sensitive;
#else
  (void)path;
  return DirectoryCaseSensitivity::Unknown;
#endif
}
