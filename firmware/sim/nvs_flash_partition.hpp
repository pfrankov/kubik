// NOR flash for the real ESP-IDF NVS storage engine: six 4 KiB sectors, no 0 -> 1 writes.
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include "partition.hpp"
#include "spi_flash_mmap.h"

class TestFlash final : public nvs::Partition {
public:
    enum class Failure { Before, Partial, After };
    static constexpr size_t SECTOR_BYTES = 4096, SECTORS = 6;
    std::array<unsigned char, SECTOR_BYTES * SECTORS> bytes;
    int operations = 0;

    TestFlash() { bytes.fill(0xff); }
    const char *get_partition_name() override { return "nvs"; }
    uint32_t get_address() override { return 0; }
    uint32_t get_size() override { return bytes.size(); }
    bool get_readonly() override { return false; }

    esp_err_t read_raw(size_t offset, void *out, size_t size) override {
        assert(offset + size <= bytes.size());
        memcpy(out, bytes.data() + offset, size);
        return ESP_OK;
    }
    esp_err_t read(size_t offset, void *out, size_t size) override { return read_raw(offset, out, size); }
    esp_err_t write_raw(size_t offset, const void *in, size_t size) override { return write(offset, in, size); }

    esp_err_t write(size_t offset, const void *in, size_t size) override {
        assert(offset + size <= bytes.size());
        if (failed) return ESP_ERR_FLASH_OP_FAIL;
        const bool fault = operations++ == fail_at;
        const size_t written = fault ? prefix(size) : size;
        const auto *source = static_cast<const unsigned char *>(in);
        for (size_t i = 0; i < written; ++i) {
            assert((bytes[offset + i] & source[i]) == source[i]);
            bytes[offset + i] &= source[i];
        }
        return result(fault);
    }

    esp_err_t erase_range(size_t offset, size_t size) override {
        assert(offset % SECTOR_BYTES == 0 && size % SECTOR_BYTES == 0);
        assert(offset + size <= bytes.size());
        if (failed) return ESP_ERR_FLASH_OP_FAIL;
        const bool fault = operations++ == fail_at;
        const size_t erased = fault ? prefix(size) : size;
        std::fill(bytes.begin() + offset, bytes.begin() + offset + erased, 0xff);
        return result(fault);
    }

    // Once a fault fires, further mutations fail until power/storage is restored.
    void inject(int at = -1, Failure mode = Failure::Before) {
        operations = 0; fail_at = at; failure = mode; failed = false;
    }

private:
    int fail_at = -1;
    Failure failure = Failure::Before;
    bool failed = false;

    size_t prefix(size_t size) const {
        if (failure == Failure::Before) return 0;
        if (failure == Failure::After) return size;
        return size / 8 * 4; // Interrupt a write/erase midway, at a word boundary.
    }
    esp_err_t result(bool fault) {
        if (!fault) return ESP_OK;
        failed = true;
        return ESP_ERR_FLASH_OP_FAIL;
    }
};
