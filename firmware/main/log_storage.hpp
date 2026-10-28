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
// Callbacks verify the mounted filesystem is readable; files open lazily on arrival.
template <class Sd, class Flash> Storage select_storage(bool sd_enabled, Sd sd, Flash flash) {
    if (sd_enabled && sd())
        return Storage::sd;
    return flash() ? Storage::flash : Storage::off;
}
struct LogLimits {
    size_t rotate_bytes, max_files;
    size_t cluster_bytes{1}; // Set from the mounted FAT volume; host fixtures use byte units.
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
// Legacy WK files may contain matching evidence: retain them at matched priority.
// IDs are global across all three prefixes, so restart never appends old files.
struct LogId {
    uint32_t sequence{};
    char kind{};
    explicit operator bool() const {
        return kind != 0;
    }
};
inline bool operator==(LogId a, LogId b) {
    return a.sequence == b.sequence && a.kind == b.kind;
}
inline bool log_path(char *out, size_t capacity, const char *root, LogId id) {
    const int n =
        std::snprintf(out, capacity, "%s/W%c%08u.LOG", root, id.kind, unsigned(id.sequence));
    return n > 0 && size_t(n) < capacity;
}
struct LogFiles {
    uint32_t newest{};
    size_t count{};
    LogId unmatched{}, matched{};
};
inline void older(LogId &oldest, LogId candidate) {
    if (!oldest || candidate.sequence < oldest.sequence ||
        (candidate.sequence == oldest.sequence && candidate.kind < oldest.kind))
        oldest = candidate;
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
        const LogId key{id, entry->d_name[1]};
        char path[192];
        struct stat info{};
        if (!log_path(path, sizeof(path), root, key)) {
            ok = false;
            break;
        }
#ifdef ESP_PLATFORM
        // FAT has no symbolic links. Host tests use lstat to avoid following links.
        const int status = stat(path, &info);
#else
        const int status = lstat(path, &info);
#endif
        if (status != 0) {
            ok = false;
            break;
        }
        // Reserve IDs occupied by directories/symlinks, but never delete them.
        result.newest = std::max(result.newest, id);
        if (!S_ISREG(info.st_mode))
            continue;
        older(key.kind == 'U' ? result.unmatched : result.matched, key);
        ++result.count;
    }
    if (closedir(dir) != 0)
        ok = false;
    return ok;
}
enum class LogResult { stored, blocked, error };
struct OpenLog {
    FILE *file{};
    LogId id{};
    LogResult result{LogResult::error};
};
// Never unlink an open stream. Return blocked so its owner can explicitly close
// an unmatched stream before a matched arrival reclaims it. Do not skip the oldest
// candidate: that would invert age order or delete matched data ahead of unmatched.
// Space returns -1 on failure, 0 when more space is needed, 1 when sufficient.
template <class Space, class Remove>
OpenLog open_next_log(const char *root, size_t max_files, LogClass classification,
                      const LogId (&active)[2], Space space, Remove remove) {
    LogFiles index;
    if (!max_files || !scan_logs(root, index) || index.newest >= 99999999)
        return {};
    const LogId next{index.newest + 1, classification == LogClass::matched ? 'M' : 'U'};
    for (;;) {
        const int room = space();
        if (room < 0)
            return {};
        if (index.count < max_files && room > 0)
            break;
        const LogId victim = index.unmatched                       ? index.unmatched
                             : classification == LogClass::matched ? index.matched
                                                                   : LogId{};
        if (!victim || victim == active[0] || victim == active[1])
            return {nullptr, {}, LogResult::blocked};
        char path[192];
        if (!log_path(path, sizeof(path), root, victim) || remove(path) != 0 ||
            !scan_logs(root, index))
            return {};
    }
    char path[192];
    if (!log_path(path, sizeof(path), root, next))
        return {};
    FILE *file = std::fopen(path, "w");
    return {file, next, file ? LogResult::stored : LogResult::error};
}
template <class Space>
OpenLog open_next_log(const char *root, size_t max_files, LogClass classification,
                      const LogId (&active)[2], Space space) {
    return open_next_log(root, max_files, classification, active, space,
                         [](const char *path) { return std::remove(path); });
}
// One owner/task; at most two files and two libc buffers, no dynamic index.
// The caller supplies FAT free-space inspection and durable-flush callbacks.
class LogWriter {
    struct Stream {
        FILE *file{};
        LogId id{};
        size_t bytes{};
        bool dirty{};
    } streams_[2];
    bool close(size_t index) {
        auto &stream = streams_[index];
        const bool ok = !stream.file || std::fclose(stream.file) == 0;
        stream = {};
        return ok;
    }

  public:
    LogWriter() = default;
    LogWriter(const LogWriter &) = delete;
    LogWriter &operator=(const LogWriter &) = delete;
    ~LogWriter() {
        close_all();
    }
    bool close_all() {
        const bool first = close(0);
        return close(1) && first;
    }
    template <class Space>
    LogResult append(const char *root, LogLimits limits, LogClass classification, const char *line,
                     Space space) {
        if (!limits.cluster_bytes || !limits.rotate_bytes)
            return LogResult::error;
        const size_t size = std::strlen(line);
        if (size > limits.rotate_bytes)
            return LogResult::blocked;
        const size_t index = classification == LogClass::matched ? 1 : 0;
        auto &stream = streams_[index];
        if (stream.file && size > limits.rotate_bytes - stream.bytes && !close(index))
            return LogResult::error;
        if (!stream.file) {
            auto open = [&] {
                const LogId active[] = {streams_[0].id, streams_[1].id};
                // Reserve a full file for each open stream, including any buffered
                // bytes not yet reflected by FAT's free-space count, plus metadata.
                return open_next_log(root, limits.max_files, classification, active, [&] {
                    const size_t allocation =
                        ((limits.rotate_bytes + limits.cluster_bytes - 1) / limits.cluster_bytes) *
                        limits.cluster_bytes;
                    const size_t reserve = allocation * (streams_[1 - index].file ? 2 : 1) + 8192;
                    return space(reserve);
                });
            };
            auto next = open();
            if (next.result == LogResult::blocked && classification == LogClass::matched &&
                streams_[0].file) {
                // Finalize the unmatched stream before retention may delete it.
                if (!close(0))
                    return LogResult::error;
                next = open();
            }
            if (next.result != LogResult::stored)
                return next.result;
            stream = {next.file, next.id, 0, false};
        }
        if (std::fwrite(line, 1, size, stream.file) != size)
            return LogResult::error;
        stream.bytes += size;
        stream.dirty = true;
        return LogResult::stored;
    }
    template <class Sync> bool flush(Sync sync) {
        for (auto &stream : streams_) {
            if (stream.file && stream.dirty) {
                if (std::fflush(stream.file) != 0 || !sync(stream.file))
                    return false;
                stream.dirty = false;
            }
        }
        return true;
    }
};
} // namespace platform
