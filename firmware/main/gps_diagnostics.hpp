#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace platform {
// Observe receiver time without changing the parser's validity decisions. Retain
// the checksum so serial captures can diagnose corrupt as well as invalid TIM.
class GpsDiagnostics {
  public:
    struct Sample {
        char raw[160]{};
        uint64_t at{}, count{};
        bool checksum_ok{};
    };
    enum Kind { rmc, gga, zda, txt, software, other, kinds };
    static constexpr const char *names[kinds] = {"RMC", "GGA", "ZDA", "TXT", "SW", "other"};
    std::array<Sample, kinds> samples{};
    uint64_t sentences{}, bad_checksums{}, overflows{}, resyncs{}, nonprintable{};
    uint64_t tx_batches{}, tx_stalls{}, tx_errors{}, rx_errors{};
    void feed(char byte, uint64_t now) {
        ++received_bytes;
        if (byte == '$') {
            if (collecting_)
                ++resyncs;
            length_ = 0;
            sanitized_ = false;
            collecting_ = true;
        }
        if (!collecting_ || byte == '\r')
            return;
        if (byte == '\n') {
            collecting_ = false;
            line_[length_] = 0;
            ++sentences;
            const bool valid = !sanitized_ && checksum(line_);
            if (!valid)
                ++bad_checksums;
            Kind kind = other;
            if (length_ >= 7 && line_[6] == ',') {
                if (!std::strncmp(line_ + 3, "RMC", 3))
                    kind = rmc;
                else if (!std::strncmp(line_ + 3, "GGA", 3))
                    kind = gga;
                else if (!std::strncmp(line_ + 3, "ZDA", 3))
                    kind = zda;
                else if (!std::strncmp(line_ + 3, "TXT", 3))
                    kind = txt;
            }
            // Keep software identity even when later TXT antenna/status messages arrive.
            if (kind == txt && std::strstr(line_, ",SW="))
                save(samples[software], now, valid);
            if (std::strncmp(line_, "$PCAS60,", 8))
                save(samples[kind], now, valid);
            if (!std::strncmp(line_, "$PCAS60,", 8)) {
                std::memcpy(last_tim, line_, length_ + 1);
                last_tim_at = now;
                last_tim_checksum_ok = valid;
                ++tim_sentences;
            }
        } else if (length_ + 1 < sizeof(line_)) {
            const auto value = static_cast<unsigned char>(byte);
            if (value < 32 || value > 126) {
                ++nonprintable;
                sanitized_ = true;
                line_[length_++] = '?';
            } else {
                line_[length_++] = byte;
            }
        } else {
            ++overflows;
            collecting_ = false;
        }
    }
    uint64_t received_bytes{}, transmitted_bytes{}, tim_sentences{}, last_tim_at{};
    char last_tim[160]{};
    bool last_tim_checksum_ok{};

  private:
    static int hex(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    }
    static bool checksum(const char *line) {
        const char *star = std::strchr(line, '*');
        if (!star || std::strlen(star + 1) != 2 || hex(star[1]) < 0 || hex(star[2]) < 0)
            return false;
        unsigned sum = 0;
        for (const char *p = line + 1; p < star; ++p)
            sum ^= static_cast<unsigned char>(*p);
        return sum == unsigned(hex(star[1]) * 16 + hex(star[2]));
    }
    void save(Sample &sample, uint64_t now, bool valid) {
        std::memcpy(sample.raw, line_, length_ + 1);
        sample.at = now;
        sample.checksum_ok = valid;
        ++sample.count;
    }
    char line_[160]{};
    size_t length_{};
    bool collecting_{};
    bool sanitized_{};
};
} // namespace platform
