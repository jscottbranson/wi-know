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

    char root[] = "/tmp/wiknow-storage-XXXXXX";
    CHECK(mkdtemp(root));
    const auto path = std::filesystem::path(root);
    auto room = [] { return 1; };
    auto put = [&](const char *name, const char *data) { std::ofstream(path / name) << data; };
    put("keep.txt", "unrelated");
    put("WK00000001.LOG", "oldest");
    put("WK00000002.LOG", "previous boot");
    FILE *file = open_next_log(root, 2, room);
    CHECK(file);
    CHECK(std::fputs("new boot\n", file) >= 0);
    CHECK(std::fclose(file) == 0);
    CHECK(!std::filesystem::exists(path / "WK00000001.LOG"));
    CHECK(std::filesystem::exists(path / "WK00000002.LOG"));
    CHECK(std::filesystem::exists(path / "WK00000003.LOG"));
    CHECK(std::filesystem::exists(path / "keep.txt"));
    std::string previous;
    std::ifstream(path / "WK00000002.LOG") >> previous;
    CHECK(previous == "previous"); // Previous log was not truncated.

    // Low space prunes only owned logs, keeping the next ID even if all are deleted.
    file = open_next_log(root, 8, [&] {
        return !std::filesystem::exists(path / "WK00000002.LOG") &&
                       !std::filesystem::exists(path / "WK00000003.LOG")
                   ? 1
                   : 0;
    });
    CHECK(file && std::fclose(file) == 0);
    CHECK(std::filesystem::exists(path / "WK00000004.LOG"));
    CHECK(!open_next_log(root, 8, [] { return -1; }));
    CHECK(std::filesystem::exists(path / "WK00000004.LOG"));
    CHECK(!open_next_log(root, 8, [] { return 0; }));
    CHECK(std::filesystem::exists(path / "keep.txt"));
    CHECK(!open_next_log("/nonexistent/wiknow", 8, room));

    std::filesystem::create_directory(path / "WK00000007.LOG");
    file = open_next_log(root, 1, room);
    CHECK(file && std::fclose(file) == 0);
    CHECK(std::filesystem::exists(path / "WK00000008.LOG"));
    CHECK(std::filesystem::is_directory(path / "WK00000007.LOG"));
    put("WK99999999.LOG", "last id");
    CHECK(!open_next_log(root, 1, room));
    CHECK(std::filesystem::exists(path / "WK00000008.LOG"));
    std::filesystem::remove_all(path);
    std::puts("Storage selection, first-use formatting, and retention tests passed");
}
