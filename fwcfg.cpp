#include "fwcfg.h"

#include <cstring>
#include <string>

namespace {

using Bytes = std::vector<uint8_t>;

void putLe(Bytes& v, size_t off, uint64_t val, int n) {
    for (int i = 0; i < n; i++) v[off + i] = uint8_t(val >> (8 * i));
}

void putBe(Bytes& v, size_t off, uint64_t val, int n) {
    for (int i = 0; i < n; i++) v[off + i] = uint8_t(val >> (8 * (n - 1 - i)));
}

void putStr(Bytes& v, size_t off, const char* s, size_t n) {
    const size_t len = std::strlen(s);
    for (size_t i = 0; i < n; i++) v[off + i] = i < len ? uint8_t(s[i]) : 0;
}

// Standard ACPI table header (36 bytes). The checksum is filled in by OVMF through the loader.
Bytes tableHeader(const char* sig, uint32_t length, uint8_t revision) {
    Bytes t(length, 0);
    putStr(t, 0, sig, 4);
    putLe(t, 4, length, 4);
    t[8] = revision;
    putStr(t, 10, "QVMSFT", 6);
    putStr(t, 16, "QVMSOFT", 8);
    putLe(t, 24, 1, 4);
    putStr(t, 28, "QVM", 4);
    putLe(t, 32, 1, 4);
    return t;
}

// One 128-byte entry of the table-loader script: a 4-byte command and a 124-byte payload.
Bytes loaderEntry(uint32_t cmd, const Bytes& payload) {
    Bytes e(128, 0);
    putLe(e, 0, cmd, 4);
    std::memcpy(e.data() + 4, payload.data(), payload.size());
    return e;
}

Bytes allocate(const char* file, uint32_t align, uint8_t zone) {
    Bytes p(124, 0);
    putStr(p, 0, file, 56);
    putLe(p, 56, align, 4);
    p[60] = zone;
    return loaderEntry(1, p);
}

Bytes addPointer(const char* dest, const char* src, uint32_t offset, uint8_t size) {
    Bytes p(124, 0);
    putStr(p, 0, dest, 56);
    putStr(p, 56, src, 56);
    putLe(p, 112, offset, 4);
    p[116] = size;
    return loaderEntry(2, p);
}

Bytes addChecksum(const char* file, uint32_t offset, uint32_t start, uint32_t length) {
    Bytes p(124, 0);
    putStr(p, 0, file, 56);
    putLe(p, 56, offset, 4);
    putLe(p, 60, start, 4);
    putLe(p, 64, length, 4);
    return loaderEntry(3, p);
}

void append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }

}  // namespace

FwCfg::FwCfg(uint32_t ramMB) {
    // Signature and features. Feature bit 0 is clear: this device only uses the selector/data ports.
    blobs[0x0000] = Bytes{'Q', 'E', 'M', 'U'};
    blobs[0x0001] = Bytes{0x01, 0x00, 0x00, 0x00};  // bit 0: traditional selector/data interface
    // RAM size (u64) and CPU count (u16), as QEMU provides them. OVMF needs the CPU count to start.
    {
        Bytes ramSize(8, 0);
        putLe(ramSize, 0, uint64_t(ramMB) << 20, 8);
        blobs[0x0003] = ramSize;
        Bytes cpus(2, 0);
        putLe(cpus, 0, 1, 2);
        blobs[0x0005] = cpus;
    }

    // ACPI tables. Each one is its own file so the loader can point at it.
    Bytes rsdp(20, 0);
    putStr(rsdp, 0, "RSD PTR ", 8);
    putStr(rsdp, 9, "QVMSFT", 6);
    rsdp[15] = 0;  // ACPI 1.0

    Bytes rsdt = tableHeader("RSDT", 40, 1);  // one entry: the FADT, filled in by the loader
    Bytes fadt = tableHeader("FACP", 116, 1);
    putLe(fadt, 76, 0x608, 4);  // PM_TMR_BLK: the ACPI power timer port (board serves it)
    fadt[91] = 4;               // PM_TMR_LEN: 4 bytes
    Bytes facs(64, 0);
    putStr(facs, 0, "FACS", 4);
    putLe(facs, 4, 64, 4);
    Bytes dsdt = tableHeader("DSDT", 36, 1);

    // Memory map: 20-byte entries of address, size and type (1 = usable RAM).
    Bytes e820;
    auto addRange = [&](uint64_t addr, uint64_t size, uint32_t type) {
        Bytes e(20, 0);
        putLe(e, 0, addr, 8);
        putLe(e, 8, size, 8);
        putLe(e, 16, type, 4);
        append(e820, e);
    };
    addRange(0, 0x9FC00, 1);
    const uint64_t ramBytes = uint64_t(ramMB) << 20;
    if (ramBytes > 0x100000) addRange(0x100000, ramBytes - 0x100000, 1);

    // The table-loader script: allocate each table, link the pointers, and fill in the checksums.
    Bytes loader;
    append(loader, allocate("etc/acpi/rsdp", 16, 0));
    append(loader, allocate("etc/acpi/rsdt", 64, 1));
    append(loader, allocate("etc/acpi/fadt", 64, 1));
    append(loader, allocate("etc/acpi/facs", 64, 1));
    append(loader, allocate("etc/acpi/dsdt", 64, 1));
    append(loader, addPointer("etc/acpi/rsdp", "etc/acpi/rsdt", 16, 4));
    append(loader, addPointer("etc/acpi/rsdt", "etc/acpi/fadt", 36, 4));
    append(loader, addPointer("etc/acpi/fadt", "etc/acpi/facs", 36, 4));
    append(loader, addPointer("etc/acpi/fadt", "etc/acpi/dsdt", 40, 4));
    append(loader, addChecksum("etc/acpi/rsdp", 8, 0, 20));
    append(loader, addChecksum("etc/acpi/rsdt", 9, 0, 40));
    append(loader, addChecksum("etc/acpi/fadt", 9, 0, 116));
    append(loader, addChecksum("etc/acpi/dsdt", 9, 0, 36));

    // Files get selector keys from 0x20 upward, in the order of the directory below.
    struct File { const char* name; Bytes data; };
    std::vector<File> files = {
        {"etc/table-loader", loader},
        {"etc/acpi/rsdp", rsdp},
        {"etc/acpi/rsdt", rsdt},
        {"etc/acpi/fadt", fadt},
        {"etc/acpi/facs", facs},
        {"etc/acpi/dsdt", dsdt},
        {"etc/e820", e820},
    };

    Bytes dir;
    Bytes count(4, 0);
    putBe(count, 0, files.size(), 4);
    append(dir, count);
    uint16_t key = 0x20;
    for (const File& f : files) {
        Bytes entry(64, 0);
        putBe(entry, 0, f.data.size(), 4);
        putBe(entry, 4, key, 2);
        putStr(entry, 8, f.name, 56);
        append(dir, entry);
        blobs[key] = f.data;
        key++;
    }
    blobs[0x0019] = dir;  // FW_CFG_FILE_DIR
}

bool FwCfg::out(uint16_t port, uint32_t value) {
    if (port == 0x510) {
        selector = uint16_t(value);
        offset = 0;
        return true;
    }
    return false;
}

bool FwCfg::in(uint16_t port, uint32_t& value) {
    if (port != 0x511) return false;
    value = 0;
    auto it = blobs.find(selector);
    if (it != blobs.end() && offset < it->second.size()) value = it->second[offset];
    offset++;
    return true;
}
