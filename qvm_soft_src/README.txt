qvm_soft - software x86 engine (step 4)

Files
  cpu.h, cpu.cpp, cpu_ext.cpp   CPU: real mode, protected mode, paging, long mode, SSE2 subset
  devices.h, devices.cpp        8259A interrupt controller and 8253 timer
  board.h, board.cpp            COM1, CMOS/RTC, keyboard controller status, port 0x92, PCI (empty bus)
  main.cpp                      the program
  tests/*.asm                   test programs (real, pm, paging, irq, rex, long, ops)

Build (Windows, Visual Studio developer prompt)
  cl /EHsc /std:c++17 /O2 main.cpp cpu.cpp cpu_ext.cpp devices.cpp board.cpp /Fe:qvm_soft.exe

Build (Linux or MinGW)
  g++ -std=c++17 -O2 -Wall -Wextra -o qvm_soft main.cpp cpu.cpp cpu_ext.cpp devices.cpp board.cpp

Run firmware
  qvm_soft.exe --firmware OVMF_CODE.fd            (64 MB RAM, 100 million steps)
  qvm_soft.exe --firmware OVMF_CODE.fd 128 300000000

  The firmware is mapped so it ends at 4 GB and the CPU starts at the reset vector FFFF:FFF0.
  Text written to COM1 or port 0xE9 is printed to the console.
  When the engine stops it prints the reason, the CS:EIP and the number of unmapped memory accesses.
  A stop such as "unsupported opcode XX" names the next instruction to implement.

Run a test program
  qvm_soft tests/ops.bin                           (prints OPS OK, exit 0)

Limits
  No hardware virtualisation, no disk, no display, no USB, no ACPI tables.
  Not implemented yet: x87 register stack, AVX, PAE without long mode, privilege levels.
  Local APIC: a plain memory page at 0xFEE00000 (no interrupt routing).
  ACPI PM timer at port 0x608 counts emulated instructions.
  OVMF_CODE.fd runs through its early init but does not finish yet: it waits in a timer
  calibration loop that is still being diagnosed.
