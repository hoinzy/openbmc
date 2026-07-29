/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_inventory.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

void appendLe16(std::vector<uint8_t>& output, uint16_t value)
{
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
}

void putString(std::vector<uint8_t>& output, size_t offset,
               const std::string& value)
{
    assert(offset + value.size() < output.size());
    std::memcpy(output.data() + offset, value.data(), value.size());
}

void appendRecord(std::vector<uint8_t>& output, uint16_t id, uint8_t type,
                  const std::string& path, const std::vector<uint8_t>& body)
{
    appendLe16(output, id);
    appendLe16(output, static_cast<uint16_t>(body.size()));
    output.push_back(type);
    output.insert(output.end(), 3, 0);
    output.push_back(static_cast<uint8_t>(path.size()));
    output.insert(output.end(), path.begin(), path.end());
    output.insert(output.end(), body.begin(), body.end());
}

std::vector<uint8_t> sampleInventory()
{
    std::vector<uint8_t> inventory(0x40, 0);
    inventory[0x3d] = 3;

    std::vector<uint8_t> cpu(0x96, 0);
    cpu[0] = 0;
    cpu[1] = 32;
    cpu[2] = 64;
    cpu[0x0b] = 0xb0;
    cpu[0x0c] = 0x0d;
    putString(cpu, 0x0f, "AuthenticAMD");
    putString(cpu, 0x1f, "AMD Zen Processor Family");
    putString(cpu, 0x3d, "3970X");
    putString(cpu, 0x5b, "AMD Ryzen Threadripper 3970X");
    appendRecord(inventory, 0x100, 1, "CPU0", cpu);

    std::vector<uint8_t> dimm(0x5f, 0);
    dimm[0] = 0;
    dimm[1] = 0x00;
    dimm[2] = 0x40;
    dimm[3] = 0x10;
    dimm[4] = 0x0e;
    putString(dimm, 0x05, "G.Skill");
    putString(dimm, 0x23, "UDIMM");
    putString(dimm, 0x2d, "DDR4");
    putString(dimm, 0x37, "1.2V");
    dimm[0x41] = 0x11;
    dimm[0x42] = 0x22;
    dimm[0x43] = 0x33;
    dimm[0x44] = 0x44;
    putString(dimm, 0x45, "F4-3600C16-16GTRGC");
    dimm[0x5b] = 0;
    dimm[0x5c] = 1;
    dimm[0x5d] = 0;
    dimm[0x5e] = 0;
    appendRecord(inventory, 0x200, 8, "A1", dimm);

    std::vector<uint8_t> pci(0x16, 0);
    pci[0] = 0;
    pci[2] = 1;
    pci[4] = 0;
    pci[5] = 1;
    pci[6] = 0;
    pci[7] = 0xde;
    pci[8] = 0x10;
    pci[9] = 0xb0;
    pci[10] = 0x1b;
    pci[15] = 0xa1;
    pci[16] = 0;
    pci[17] = 0;
    pci[18] = 3;
    appendRecord(inventory, 0x300, 0x20, "PCIE1", pci);

    return inventory;
}

} // namespace

int main()
{
    const std::vector<uint8_t> sample = sampleInventory();
    ami::inventory::Inventory decoded;
    std::string error;
    assert(ami::inventory::parse(sample, decoded, error));
    assert(decoded.cpus.size() == 1);
    assert(decoded.cpus[0].coreCount == 32);
    assert(decoded.cpus[0].threadCount == 64);
    assert(decoded.cpus[0].maxFrequencyMhz == 3504);
    assert(decoded.cpus[0].brand == "AMD Ryzen Threadripper 3970X");
    assert(decoded.dimms.size() == 1);
    assert(decoded.dimms[0].sizeMiB == 16384);
    assert(decoded.dimms[0].speedMhz == 3600);
    assert(decoded.dimms[0].manufacturer == "G.Skill");
    assert(decoded.dimms[0].serialNumber == "11223344");
    assert(decoded.pciDevices.size() == 1);
    assert(decoded.pciDevices[0].vendorId == 0x10de);
    assert(decoded.pciDevices[0].deviceId == 0x1bb0);

    auto truncated = sample;
    truncated.pop_back();
    assert(!ami::inventory::parse(truncated, decoded, error));

    auto tooManyRecords = sample;
    tooManyRecords[0x3d] = 65;
    assert(!ami::inventory::parse(tooManyRecords, decoded, error));
    return 0;
}
