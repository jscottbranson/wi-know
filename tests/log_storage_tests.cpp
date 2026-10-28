#include "log_storage.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition);                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

int main() {
    using namespace platform;
    int sd_calls = 0, flash_calls = 0;
    auto sd_ok = [&] {
        ++sd_calls;
        return true;
    };
    auto flash_ok = [&] {
        ++flash_calls;
        return true;
    };
    CHECK(select_storage(true, sd_ok, flash_ok) == Storage::sd);
    CHECK(sd_calls == 1 && flash_calls == 0);
    CHECK(select_storage(false, sd_ok, flash_ok) == Storage::flash);
    CHECK(sd_calls == 1 && flash_calls == 1);
    CHECK(select_storage(true, [] { return false; }, flash_ok) == Storage::flash);
    CHECK(select_storage(true, [] { return false; }, [] { return false; }) == Storage::off);
    CHECK(log_limits(Storage::flash, 10485760, 100).rotate_bytes == 32768);
    CHECK(log_limits(Storage::flash, 10485760, 100).max_files == 8);
    CHECK(log_limits(Storage::flash, 4096, 2).rotate_bytes == 4096);
    CHECK(log_limits(Storage::flash, 4096, 2).max_files == 2);
    CHECK(log_limits(Storage::sd, 10485760, 100).rotate_bytes == 10485760);

    std::vector<uint8_t> partition(1025, 0xff);
    auto read = [&](size_t offset, void *data, size_t size) {
        std::memcpy(data, partition.data() + offset, size);
        return true;
    };
    CHECK(erased_partition(partition.size(), read));
    partition.back() = 0;
    CHECK(!erased_partition(partition.size(), read));
    partition.back() = 0xff;
    CHECK(!erased_partition(partition.size(), [](size_t, void *, size_t) { return false; }));
    CHECK(!erased_partition(0, read));

    struct Directory {
        char name[40] = "/tmp/wiknow-storage-XXXXXX";
        Directory() {
            CHECK(mkdtemp(name));
        }
        ~Directory() {
            std::filesystem::remove_all(name);
        }
        auto path(const char *file) {
            return std::filesystem::path(name) / file;
        }
        void put(const char *file, const char *data = "evidence\n") {
            std::ofstream(path(file)) << data;
        }
        bool has(const char *file) {
            return std::filesystem::exists(path(file));
        }
        std::string read(const char *file) {
            std::ifstream input(path(file));
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
    };
    const LogId inactive[2]{};
    auto room = [] { return 1; };
    auto close = [](OpenLog log) {
        CHECK(log.file && log.result == LogResult::stored);
        CHECK(std::fclose(log.file) == 0);
    };
    // Count pressure removes unmatched before older matched/legacy data; within
    // each priority it removes the oldest ID, independent of readdir order.
    for (auto classification : {LogClass::matched, LogClass::unmatched}) {
        Directory d;
        d.put("WM00000001.LOG");
        d.put("WK00000002.LOG");
        d.put("WU00000004.LOG");
        d.put("WU00000003.LOG");
        d.put("keep.txt");
        close(open_next_log(d.name, 4, classification, inactive, room));
        CHECK(!d.has("WU00000003.LOG") && d.has("WU00000004.LOG"));
        CHECK(d.has("WM00000001.LOG") && d.has("WK00000002.LOG") && d.has("keep.txt"));
        close(open_next_log(d.name, 4, classification, inactive, room));
        CHECK(!d.has("WU00000004.LOG"));
    }
    // With only protected files, an unmatched arrival cannot make room even when
    // the highest ID is a legacy mixed file from a previous firmware/boot.
    {
        Directory d;
        d.put("WM00000001.LOG");
        d.put("WK00000002.LOG");
        CHECK(open_next_log(d.name, 2, LogClass::unmatched, inactive, room).result ==
              LogResult::blocked);
        CHECK(d.has("WM00000001.LOG") && d.has("WK00000002.LOG"));
        close(open_next_log(d.name, 2, LogClass::matched, inactive, room));
        CHECK(!d.has("WM00000001.LOG") && d.has("WK00000002.LOG") && d.has("WM00000003.LOG"));
        CHECK(d.read("WK00000002.LOG") == "evidence\n");
    }
    // Low space applies the same order, preserving the originally chosen next ID.
    {
        Directory d;
        d.put("WM00000001.LOG");
        d.put("WU00000003.LOG");
        d.put("WU00000002.LOG");
        std::vector<std::string> removed;
        auto remove = [&](const char *file) {
            removed.push_back(std::filesystem::path(file).filename().string());
            return std::remove(file);
        };
        close(open_next_log(
            d.name, 8, LogClass::matched, inactive, [&] { return removed.size() >= 3 ? 1 : 0; },
            remove));
        CHECK((removed ==
               std::vector<std::string>{"WU00000002.LOG", "WU00000003.LOG", "WM00000001.LOG"}));
        CHECK(d.has("WM00000004.LOG"));
        d.put("WU00000005.LOG");
        CHECK(open_next_log(d.name, 8, LogClass::unmatched, inactive, [] { return 0; }).result ==
              LogResult::blocked);
        CHECK(d.has("WM00000004.LOG") && !d.has("WU00000005.LOG"));
    }
    // Active files must be closed by their owner before deletion; skipping an
    // active unmatched candidate must never allow deleting a matched file.
    {
        Directory d;
        d.put("WM00000001.LOG");
        d.put("WU00000002.LOG");
        d.put("WU00000003.LOG");
        const LogId active[2] = {{2, 'U'}, {1, 'M'}};
        CHECK(open_next_log(d.name, 3, LogClass::matched, active, room).result ==
              LogResult::blocked);
        CHECK(d.has("WM00000001.LOG") && d.has("WU00000002.LOG") && d.has("WU00000003.LOG"));
        const LogId only_matched[2] = {{}, {1, 'M'}};
        close(open_next_log(d.name, 3, LogClass::matched, only_matched, room));
        CHECK(!d.has("WU00000002.LOG") && d.has("WM00000001.LOG"));
    }
    // I/O failures never fall through to lower-priority evidence or unrelated paths.
    {
        Directory d;
        d.put("keep.txt");
        d.put("WU00000001.LOG");
        d.put("WM00000002.LOG");
        CHECK(open_next_log(d.name, 2, LogClass::matched, inactive, room, [](const char *) {
                  return -1;
              }).result == LogResult::error);
        CHECK(open_next_log(d.name, 2, LogClass::matched, inactive, [] { return -1; }).result ==
              LogResult::error);
        CHECK(d.has("WU00000001.LOG") && d.has("WM00000002.LOG"));
        CHECK(open_next_log("/nonexistent/wiknow", 8, LogClass::matched, inactive, room).result ==
              LogResult::error);
        std::filesystem::create_directory(d.path("WU00000005.LOG"));
        std::filesystem::create_symlink(d.path("keep.txt"), d.path("WM00000006.LOG"));
        close(open_next_log(d.name, 2, LogClass::matched, inactive, room));
        CHECK(d.has("WM00000007.LOG") && d.read("keep.txt") == "evidence\n");
        CHECK(std::filesystem::is_directory(d.path("WU00000005.LOG")));
        CHECK(std::filesystem::is_symlink(d.path("WM00000006.LOG")));
        d.put("WK99999999.LOG");
        CHECK(open_next_log(d.name, 1, LogClass::matched, inactive, room).result ==
              LogResult::error);
        CHECK(d.has("WM00000007.LOG"));
    }
    auto ample = [](size_t) { return 1; };
    auto sync = [](FILE *) { return true; };
    // Both streams remain open, share one overall count budget, and retain all
    // classification/drone records passed with the observation's matched class.
    {
        Directory d;
        LogWriter writer;
        CHECK(writer.append(d.name, {64, 2}, LogClass::matched, "match one\n", ample) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {64, 2}, LogClass::unmatched, "ordinary\n", ample) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {64, 2}, LogClass::matched, "match two\nRID\n", ample) ==
              LogResult::stored);
        CHECK(writer.flush(sync));
        CHECK(d.read("WM00000001.LOG") == "match one\nmatch two\nRID\n");
        CHECK(d.read("WU00000002.LOG") == "ordinary\n");
        LogFiles index;
        CHECK(scan_logs(d.name, index) && index.count == 2);
        // Failed durable flush is surfaced, not marked clean or retried as success.
        CHECK(writer.append(d.name, {64, 2}, LogClass::matched, "third\n", ample) ==
              LogResult::stored);
        CHECK(!writer.flush([](FILE *) { return false; }));
        CHECK(writer.close_all());
        LogWriter restarted;
        CHECK(restarted.append(d.name, {64, 2}, LogClass::unmatched, "new boot\n", ample) ==
              LogResult::stored);
        CHECK(restarted.flush(sync));
        CHECK(d.has("WM00000001.LOG") && !d.has("WU00000002.LOG") && d.has("WU00000003.LOG"));
    }
    // A full matched stream rotates only after the open unmatched stream is
    // finalized/reclaimed. The old matched file survives even though it is older.
    {
        Directory d;
        LogWriter writer;
        CHECK(writer.append(d.name, {8, 2}, LogClass::matched, "match 1\n", ample) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {8, 2}, LogClass::unmatched, "noise\n", ample) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {8, 2}, LogClass::matched, "match 2\n", ample) ==
              LogResult::stored);
        CHECK(writer.flush(sync));
        CHECK(d.has("WM00000001.LOG") && d.has("WM00000003.LOG") && !d.has("WU00000002.LOG"));
        CHECK(writer.append(d.name, {8, 2}, LogClass::unmatched, "drop\n", ample) ==
              LogResult::blocked);
        CHECK(writer.append(d.name, {8, 2}, LogClass::matched, "match 3\n", ample) ==
              LogResult::stored);
        CHECK(!d.has("WM00000001.LOG") && d.has("WM00000003.LOG") && d.has("WM00000004.LOG"));
    }
    // With a single-file budget, matched input can reclaim the active unmatched
    // stream, but unmatched input cannot evict the active matched stream.
    {
        Directory d;
        LogWriter writer;
        CHECK(writer.append(d.name, {64, 1}, LogClass::unmatched, "noise\n", ample) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {64, 1}, LogClass::matched, "match\n", ample) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {64, 1}, LogClass::unmatched, "drop\n", ample) ==
              LogResult::blocked);
        CHECK(writer.flush(sync));
        CHECK(!d.has("WU00000001.LOG") && d.read("WM00000002.LOG") == "match\n");
    }
    // Reserve capacity for both streams and outstanding buffers plus headroom.
    {
        Directory d;
        LogWriter writer;
        std::vector<size_t> reserves;
        auto space = [&](size_t reserve) {
            reserves.push_back(reserve);
            return 1;
        };
        CHECK(writer.append(d.name, {64, 8}, LogClass::matched, "match\n", space) ==
              LogResult::stored);
        CHECK(writer.append(d.name, {64, 8}, LogClass::unmatched, "noise\n", space) ==
              LogResult::stored);
        CHECK((reserves == std::vector<size_t>{8192 + 64, 8192 + 128}));
        CHECK(writer.append(d.name, {1, 8}, LogClass::unmatched, "oversize\n", space) ==
              LogResult::blocked);
        CHECK(writer.close_all());
        CHECK(writer.append(d.name, {64, 8}, LogClass::matched, "match\n",
                            [](size_t) { return -1; }) == LogResult::error);
    }
    {
        Directory d;
        LogWriter writer;
        size_t reserved = 0;
        CHECK(writer.append(d.name, {65, 8, 64}, LogClass::matched, "match\n", [&](size_t bytes) {
            reserved = bytes;
            return 1;
        }) == LogResult::stored);
        CHECK(reserved == 8192 + 128); // Include the partially used final allocation cluster.
        CHECK(writer.append(d.name, {65, 8, 0}, LogClass::unmatched, "drop\n", ample) ==
              LogResult::error);
    }
    std::puts(
        "Storage selection, migration, priority retention, and two-stream writer tests passed");
}
