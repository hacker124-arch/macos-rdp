// QEMU-compatible firmware configuration device (fw_cfg), ports 0x510 (selector) and 0x511 (data).
// OVMF reads the ACPI tables and the memory map through it. The tables are built here and linked by
// OVMF through the "etc/table-loader" script, the same way QEMU provides them.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

class FwCfg {
public:
    explicit FwCfg(uint32_t ramMB);
    // Returns false when the port is not the fw_cfg selector or data port.
    bool out(uint16_t port, uint32_t value);
    bool in(uint16_t port, uint32_t& value);

private:
    std::map<uint16_t, std::vector<uint8_t>> blobs;  // by selector key
    uint16_t selector = 0;
    uint32_t offset = 0;
};
