#pragma once
#include "log_helpers.hpp"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>

namespace platform {
enum class Storage { starting, off, sd, flash };
inline const char *storage_label(Storage value) {
    switch (value) {
    case Storage::starting:
        return "WAIT";
    case Storage::sd:
        return "SD";
    case Storage::flash:
        return "FLASH";
    default:
        return "OFF";
    }
}
// Callbacks succeed only after opening a writable log, not merely mounting.
template <class Sd, class Flash> Storage select_storage(bool sd_enabled, Sd sd, Flash flash) {
    if (sd_enabled && sd())
        return Storage::sd;
    return flash() ? Storage::flash : Storage::off;
}
struct LogLimits {
    size_t rotate_bytes, max_files;
};
inline LogLimits log_limits(Storage storage, size_t rotate_bytes, size_t max_files) {
    return storage == Storage::flash
               ? LogLimits{std::min(rotate_bytes, size_t(32768)), std::min(max_files, size_t(8))}
               : LogLimits{rotate_bytes, max_files};
}
// Read all bytes before allowing first-use formatting. A read failure or any
// existing content must never be interpreted as an empty partition.
template <class Read> bool erased_partition(size_t size, Read read) {
    uint8_t buffer[256];
    for (size_t offset = 0; offset < size; offset += sizeof(buffer)) {
        const size_t n = std::min(sizeof(buffer), size - offset);
        if (!read(offset, buffer, n))
            return false;
        for (size_t i = 0; i < n; ++i)
            if (buffer[i] != 0xff)
                return false;
    }
    return size != 0;
}
struct LogFiles {
    uint32_t newest{}, oldest{UINT32_MAX};
    size_t count{};
};
inline bool log_path(char *out, size_t capacity, const char *root, uint32_t id) {
    const int n = std::snprintf(out, capacity, "%s/WK%08u.LOG", root, unsigned(id));
    return n > 0 && size_t(n) < capacity;
}
inline bool scan_logs(const char *root, LogFiles &result) {
    result = {};
    DIR *dir = opendir(root);
    if (!dir)
        return false;
    bool ok = true;
    for (;;) {
        errno = 0;
        auto *entry = readdir(dir);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        uint32_t id{};
        if (!log_file_id(entry->d_name, id))
            continue;
        char path[192];
        struct stat info{};
        if (!log_path(path, sizeof(path), root, id) || stat(path, &info) != 0) {
            ok = false;
            break;
        }
        // A matching directory must never be removed or used as a new log.
        result.newest = std::max(result.newest, id);
        if (!S_ISREG(info.st_mode))
            continue;
        result.oldest = std::min(result.oldest, id);
        ++result.count;
    }
    closedir(dir);
    return ok;
}
// Called only with the previous log closed. Space returns -1 on error, 0 when
// more space is needed, 1 when sufficient. Retention owns only WK########.LOG.
template <class Space> FILE *open_next_log(const char *root, size_t max_files, Space space) {
    LogFiles index;
    if (!max_files || !scan_logs(root, index) || index.newest >= 99999999)
        return nullptr;
    const uint32_t next = index.newest + 1;
    for (;;) {
        const int room = space();
        if (room < 0)
            return nullptr;
        if (index.count < max_files && room > 0)
            break;
        char oldest[192];
        if (!index.count || !log_path(oldest, sizeof(oldest), root, index.oldest) ||
            std::remove(oldest) != 0 || !scan_logs(root, index))
            return nullptr;
    }
    char path[192];
    if (!log_path(path, sizeof(path), root, next))
        return nullptr;
    return std::fopen(path, "w");
}
} // namespace platform
