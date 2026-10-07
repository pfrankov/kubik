// Exercise ESP-IDF's unmodified NVS engine with the firmware's exact two-slot blob layout.
#include <cstddef>
#include <cstdio>
#include <vector>
#include "esp_idf_version.h"
#include "nvs_storage.hpp"
#include "nvs_flash_partition.hpp"
#include "connection_record.h"

static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 1), "Run against the firmware's ESP-IDF 5.5.1");
using Blob = std::vector<unsigned char>;
static const char *const RECORD_KEYS[] = {CONNECTION_RECORD_KEY_A, CONNECTION_RECORD_KEY_B};

static uint8_t open_storage(nvs::Storage &storage) {
    assert(storage.init(0, TestFlash::SECTORS) == ESP_OK);
    uint8_t ns = 0;
    assert(storage.createOrOpenNamespace("kubik", true, ns) == ESP_OK);
    return ns;
}

static esp_err_t put(nvs::Storage &storage, uint8_t ns, const char *key, const Blob &value) {
    return storage.writeItem(ns, nvs::ItemType::BLOB, key, value.data(), value.size());
}

static void expect_blob(nvs::Storage &storage, uint8_t ns, const char *key, const Blob &expected) {
    Blob actual(expected.size());
    assert(storage.readItem(ns, nvs::ItemType::BLOB, key, actual.data(), actual.size()) == ESP_OK);
    assert(actual == expected);
}

static void expect_legacy(nvs::Storage &storage, uint8_t ns) {
    expect_blob(storage, ns, "networks", Blob(920, 0x31));
    expect_blob(storage, ns, "pin", Blob(32, 0x45));
    expect_blob(storage, ns, "muse_v1", Blob(4204, 0x31));
}

static void expect_other_state(nvs::Storage &storage, uint8_t ns) {
    expect_blob(storage, ns, "devkey", Blob(32, 0x70));
    for (int i = 0; i < 12; ++i) {
        char key[16]; snprintf(key, sizeof key, "pref%d", i);
        int value = -1;
        assert(storage.readItem(ns, key, value) == ESP_OK && value == i);
    }
    uint8_t phy;
    assert(storage.createOrOpenNamespace("phy", false, phy) == ESP_OK);
    expect_blob(storage, phy, "cal_data", Blob(1904, 0x87));
    expect_blob(storage, phy, "cal_mac", Blob(6, 0x64));
    uint32_t version = 0;
    assert(storage.readItem(phy, "cal_version", version) == ESP_OK && version == 1);
}

static TestFlash legacy_flash() {
    TestFlash flash;
    nvs::Storage storage(&flash);
    uint8_t ns = open_storage(storage);
    assert(put(storage, ns, "networks", Blob(920, 0x31)) == ESP_OK);
    assert(put(storage, ns, "pin", Blob(32, 0x45)) == ESP_OK);
    assert(put(storage, ns, "muse_v1", Blob(4204, 0x31)) == ESP_OK);
    assert(put(storage, ns, "devkey", Blob(32, 0x70)) == ESP_OK);
    for (int i = 0; i < 12; ++i) {
        char key[16]; snprintf(key, sizeof key, "pref%d", i);
        assert(storage.writeItem(ns, key, i) == ESP_OK);
    }
    // ESP32-C6 shares this partition with 1904 B of PHY calibration and its metadata.
    uint8_t phy;
    assert(storage.createOrOpenNamespace("phy", true, phy) == ESP_OK);
    assert(put(storage, phy, "cal_data", Blob(1904, 0x87)) == ESP_OK);
    assert(put(storage, phy, "cal_mac", Blob(6, 0x64)) == ESP_OK);
    assert(storage.writeItem(phy, "cal_version", uint32_t{1}) == ESP_OK);
    return flash;
}

// Deliberately distinct opaque payloads test every byte, not only a header. The settings
// harness separately exercises the application's schema, migration and slot selection.
static Blob record(uint64_t revision) {
    Blob value(sizeof(connection_record_t), static_cast<unsigned char>(revision % 239 + 1));
    memcpy(value.data() + offsetof(connection_record_t, revision), &revision, sizeof revision);
    return value;
}

// 0 = absent; positive values identify a complete record; a negative value is a read error.
static int64_t revision(nvs::Storage &storage, uint8_t ns, unsigned slot) {
    Blob value(sizeof(connection_record_t));
    esp_err_t err = storage.readItem(ns, nvs::ItemType::BLOB, RECORD_KEYS[slot], value.data(), value.size());
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (err != ESP_OK) return -err;
    uint64_t result;
    memcpy(&result, value.data() + offsetof(connection_record_t, revision), sizeof result);
    assert(result > 0 && result < INT64_MAX);
    assert(value == record(result) && "NVS exposed a torn connection record");
    return static_cast<int64_t>(result);
}

static int64_t newest(nvs::Storage &storage, uint8_t ns) {
    int64_t a = revision(storage, ns, 0), b = revision(storage, ns, 1);
    return a < 0 || b < 0 ? -1 : std::max(a, b);
}

struct FaultResults { int cases = 0, old = 0, replaced = 0, stale_reads = 0, old_then_new = 0; };

static void check_fault(const TestFlash &baseline, uint64_t before, unsigned slot, int operation,
                        TestFlash::Failure failure, FaultResults &counts) {
    TestFlash flash = baseline;
    int64_t immediate;
    esp_err_t written;
    {
        nvs::Storage storage(&flash);
        uint8_t ns = open_storage(storage);
        assert(newest(storage, ns) == static_cast<int64_t>(before));
        flash.inject(operation, failure);
        written = put(storage, ns, RECORD_KEYS[slot], record(before + 1));
        immediate = newest(storage, ns);
    }
    flash.inject();
    nvs::Storage recovered(&flash);
    uint8_t ns = open_storage(recovered);
    int64_t restored = newest(recovered, ns);
    if (restored != static_cast<int64_t>(before) && restored != static_cast<int64_t>(before + 1))
        fprintf(stderr, "NVS recovery failed: revision=%llu mode=%d op=%d write=%x immediate=%lld restored=%lld\n",
                static_cast<unsigned long long>(before), static_cast<int>(failure), operation, written,
                static_cast<long long>(immediate), static_cast<long long>(restored));
    assert(restored == static_cast<int64_t>(before) || restored == static_cast<int64_t>(before + 1));
    if (written == ESP_OK) assert(restored == static_cast<int64_t>(before + 1));
    if (!before) expect_legacy(recovered, ns);
    expect_other_state(recovered, ns);
    if (restored == static_cast<int64_t>(before + 1)) ++counts.replaced; else ++counts.old;
    if (immediate != restored) ++counts.stale_reads;
    if (before && immediate == static_cast<int64_t>(before) && restored == static_cast<int64_t>(before + 1))
        ++counts.old_then_new;
    ++counts.cases;
}

static void check_faults(const TestFlash &baseline, FaultResults &counts) {
    TestFlash measured = baseline;
    uint64_t before;
    unsigned slot;
    int operations;
    {
        nvs::Storage storage(&measured);
        uint8_t ns = open_storage(storage);
        int64_t latest = newest(storage, ns);
        assert(latest >= 0);
        before = static_cast<uint64_t>(latest);
        slot = revision(storage, ns, 0) == latest && latest ? 1 : 0;
        measured.inject();
        assert(put(storage, ns, RECORD_KEYS[slot], record(before + 1)) == ESP_OK);
        operations = measured.operations;
    }
    for (auto mode : {TestFlash::Failure::Before, TestFlash::Failure::Partial, TestFlash::Failure::After})
        for (int operation = 0; operation < operations; ++operation)
            check_fault(baseline, before, slot, operation, mode, counts);
}

static void migrate(TestFlash &flash) {
    nvs::Storage storage(&flash);
    uint8_t ns = open_storage(storage);
    assert(put(storage, ns, RECORD_KEYS[0], record(1)) == ESP_OK);
    assert(storage.eraseItem(ns, "networks") == ESP_OK);
    assert(storage.eraseItem(ns, "pin") == ESP_OK);
    assert(storage.eraseItem(ns, "muse_v1") == ESP_OK);
}

static void check_repeated_updates(TestFlash &flash, FaultResults &counts) {
    for (int round = 0; round < 24; ++round) {
        // Different page-reclamation boundaries, retaining the latest slot throughout replacement.
        check_faults(flash, counts);
        nvs::Storage storage(&flash);
        uint8_t ns = open_storage(storage);
        for (int i = 0; i < 10; ++i) {
            uint64_t latest = newest(storage, ns);
            unsigned slot = revision(storage, ns, 0) == static_cast<int64_t>(latest) ? 1 : 0;
            assert(put(storage, ns, RECORD_KEYS[slot], record(latest + 1)) == ESP_OK);
            assert(newest(storage, ns) == static_cast<int64_t>(latest + 1));
        }
    }
}

static void check_full_partition(TestFlash flash) {
    int64_t before;
    {
        nvs::Storage storage(&flash);
        uint8_t ns = open_storage(storage);
        before = newest(storage, ns);
        assert(before > 0);
        unsigned slot = revision(storage, ns, 0) == before ? 1 : 0;
        unsigned filled = 0;
        for (; filled < 100; ++filled) {
            char key[16]; snprintf(key, sizeof key, "filler%u", filled);
            esp_err_t err = put(storage, ns, key, Blob(256, 0xab));
            if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) break;
            assert(err == ESP_OK);
        }
        assert(filled < 100);
        assert(put(storage, ns, RECORD_KEYS[slot], record(before + 1)) == ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    }
    nvs::Storage recovered(&flash);
    uint8_t ns = open_storage(recovered);
    assert(newest(recovered, ns) == before);
    expect_other_state(recovered, ns);
}

int main() {
    TestFlash flash = legacy_flash();
    FaultResults counts;
    check_faults(flash, counts);
    migrate(flash);
    check_repeated_updates(flash, counts);
    check_full_partition(flash);
    assert(counts.old > 0 && counts.replaced > 0 && counts.old_then_new > 0);
    printf("NVS two %zu-byte slots / 24 KiB: migration, PHY, 240 rewrites, full partition; "
           "%d faults, %d old / %d new, %d stale immediate reads (%d old then new); complete values preserved\n",
           sizeof(connection_record_t), counts.cases, counts.old, counts.replaced, counts.stale_reads, counts.old_then_new);
}
