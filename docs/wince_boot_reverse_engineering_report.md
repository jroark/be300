# WinCE Kernel, Driver, and Early Startup Reverse-Engineering Report

## Summary

This directory contains extracted XIP modules from a Windows CE 3.00 ROM image for a CASIO handheld. The binaries are enough to reconstruct:

- the ROM/XIP module ordering
- the first-wave OS processes and their likely startup dependencies
- the driver activation model used by `device.exe`
- the board and SoC identity
- the major hardware blocks present in the image
- a meaningful amount of interrupt, timer, and DMA behavior

The strongest platform identifiers are in `nk.exe`, `NANDAccess.dll`, and `CVersion.exe`:

- `Windows CE Kernel for MIPS Built on Apr 11 2001 at 15:23:09`
- `PF2001`
- `Pocket Manager J740`
- `Palm PC2`
- `VR4131`
- `JX-740 Cassiopeia WIL Version 1.01`
- `Version: 3.00`

Taken together, this is a CASIO Cassiopeia J-740 / JX-740-class Windows CE 3.00 image for a MIPS VR4131-family platform.

## Evidence Basis

Direct evidence used here:

- `modules/index.txt` for XIP order and virtual base addresses
- `modules/*_slot*.txt` for process-slot mapping
- ASCII and UTF-16LE strings from the extracted binaries
- raw MIPS disassembly using the provided container toolchain

Important limitation:

- this repo does **not** contain the full ROM, bootloader, or a dumped registry hive
- exact `HKLM\\init` launch values, exact OEMAddressTable contents, and exact boot ROM handoff code are therefore not directly recoverable here

Throughout this report:

- `Confirmed` means directly visible in the artifacts
- `Inferred` means a strong conclusion based on WinCE 3.00 behavior plus the visible evidence
- `Unknown` means the artifacts here are not sufficient

## 1. ROM/XIP Load Order

### 1.1 Confirmed ROM order

The XIP order in `modules/index.txt` starts:

| XIP idx | Module | Meaning |
| --- | --- | --- |
| 0 | `nk.exe` | kernel / OAL-adjacent code |
| 1 | `coredll.dll` | core user-mode API DLL |
| 2 | `filesys.exe` | file system / registry / object store process |
| 3 | `gwes.exe` | graphics, windowing, events, input |
| 4 | `device.exe` | device manager |
| 5 | `fatfs.dll` | FAT file system |
| 6 | `shell.exe` | debug command shell |
| 15 | `atadisk.dll` | ATA / storage driver |
| 30 | `ne2000.dll` | PCMCIA NE2000 Ethernet miniport |
| 63 | `touch.dll` | touch panel driver |
| 64 | `keybddr.dll` | keyboard driver |
| 65 | `eeprom.dll` | EEPROM access |
| 66 | `PowerOn.dll` | power / restart / high-current handling |
| 73 | `nanddisk.dll` | NAND-backed disk driver |
| 74 | `pcmcia.dll` | PCMCIA subsystem |
| 77 | `digcam.dll` | digital camera driver |
| 78 | `serial.dll` | serial / modem driver |
| 79 | `socket.dll` | socket / modem-socket helper |
| 80 | `usb.dll` | USB connection driver |
| 81 | `wavedev.dll` | audio device driver |
| 82 | `buzzer.dll` | buzzer API/helper |
| 83 | `bltinbuz.dll` | built-in buzzer stream driver |
| 90 | `GetDisk.dll` | CASIO disk naming / disk selection helper |
| 91 | `NANDAccess.dll` | raw NAND access helper |
| 92 | `Boot.exe` | CASIO boot/patch/init helper app |
| 94 | `BootSafeShell.exe` | launches `SafeShell.exe` |

### 1.2 What ROM order means

`Confirmed`:

- these modules are present in the ROM/XIP image
- the early OS code is intended to run entirely from XIP, not from a filesystem loaded later
- the core kernel/userspace trio is `nk.exe`, `filesys.exe`, `gwes.exe`, `device.exe`

`Inferred`:

- the XIP index is a reliable statement about ROM inclusion order
- it is **not** by itself a guaranteed statement about exact process execution order
- actual process launch ordering is typically controlled by `HKLM\\init` in WinCE 3.0, which is not dumped here

## 2. Early Runtime Order

### 2.1 What the kernel clearly knows about startup

`Confirmed` from `nk.exe` strings:

- `Launch`
- `init`
- `filesys.exe`
- `SystemPath`
- `Loader`
- `JITDebugger`
- `InitDebugEther`

This is strong evidence that the kernel contains the normal CE init/launch machinery and knows about `filesys.exe` as an early system process.

### 2.2 Filesys comes up very early

`Confirmed` from `filesys.exe`:

- it references `\\Windows\\initobj.dat`
- it references `\\windows\\initdb.ini`
- it references the major registry roots:
  - `HKEY_LOCAL_MACHINE`
  - `HKEY_CURRENT_USER`
  - `HKEY_USERS`
  - `HKEY_CLASSES_ROOT`
- it references the standard CE hives/areas:
  - `system`
  - `drivers`
  - `hardware`
  - `init`
  - `wdmdrivers`

`Inferred`:

- `filesys.exe` is responsible for making the initial object store / registry view available
- the missing `initdb.ini` contents almost certainly define `HKLM\\init`, `Drivers\\BuiltIn`, and other early boot keys
- because later components depend heavily on registry data, `filesys.exe` must be one of the first user-mode processes to run

### 2.3 Separate process slots confirm early user processes

`Confirmed` from sidecars:

- `filesys_slot1.txt` maps `filesys.exe` at `0x02010000`
- `gwes_slot2.txt` maps `gwes.exe` at `0x04010000`

This is characteristic of WinCE 3.0 process-slot mapping. It proves these are real early processes, not just libraries sitting in ROM.

### 2.4 Device manager is another first-wave process

`Confirmed` from `device.exe` strings:

- `Drivers\\Active`
- `Drivers\\BuiltIn`
- `Drivers\\PCMCIA`
- `Install_Driver`
- `GetDriverName`
- `Prefix`
- `Index`
- `Order`
- `Entry`
- `ClientInfo`
- `PnpId`
- `CardSystemInit`
- `CardRegisterClient`
- `CardSystemPower`
- `SendNotifyMessageW`
- `TapiDeviceChange`

This is direct evidence that `device.exe`:

- enumerates built-in drivers
- maintains `Drivers\\Active`
- also manages PCMCIA-backed devices
- assigns stream-driver names using `Prefix` and `Index`
- sends add/remove notifications

### 2.5 Probable startup sequence

`Inferred`, with high confidence:

1. Boot ROM / bootloader transfers control to `nk.exe`.
2. `nk.exe` initializes the kernel, memory, MMU/TLB/cache, scheduler, and OEM-dependent hardware support.
3. `nk.exe` optionally initializes debug Ethernet / KITL-like services (`InitDebugEther`, EDBG strings).
4. `filesys.exe` starts very early and materializes registry/object-store state from `initobj.dat` / `initdb.ini`.
5. `gwes.exe` and `device.exe` are launched as first-wave system processes.
6. `device.exe` activates built-in and PCMCIA drivers and populates `Drivers\\Active`.
7. `gwes.exe` binds to keyboard/touch drivers through `HARDWARE\\DEVICEMAP`.
8. Higher-level CASIO shell and maintenance apps come later: `coshell.exe`, `Boot.exe`, `SafeShell.exe`, `PCLASRV.exe`, `StartingUSB.exe`, restore tools, and version utilities.

### 2.6 What is not provable here

`Unknown`:

- the exact numeric `LaunchXX` and `DependXX` order under `HKLM\\init`
- whether `gwes.exe` starts before `device.exe` or very shortly after it
- exact pre-kernel boot ROM sequencing

## 3. Kernel and Platform Bring-Up

### 3.1 Platform identity

`Confirmed` from `nk.exe` and CASIO helpers:

- `VR4131`
- `PF2001`
- `Pocket Manager J740`
- `Palm PC2`
- `JX-740 Cassiopeia WIL Version 1.01`

`Inferred`:

- the CPU family is NEC VR4131-class MIPS
- `PF2001` is the internal platform / board family name
- CASIO layered its own board-support helpers on top of standard CE 3.0 driver models

### 3.2 Kernel startup services visible in `nk.exe`

`Confirmed`:

- the kernel is MIPS-specific
- it contains JIT/debugger loader references:
  - `kInitializeJit`
  - `JITDebugger`
  - `mscoree.dll`
- it contains launch/init references
- it contains extensive exception-dump formatting strings
- it contains full debug Ethernet logic, including DHCP, timeouts, and interrupt/timer worker threads

Representative strings:

- `Windows CE Kernel for MIPS Built on Apr 11 2001 at 15:23:09`
- `InitDebugEther`
- `using Debug Ethernet card`
- `Debug messages successflly transported to Ether!`
- `EDBG InterruptInitialize failed`
- `Error creating timer thread`
- `TimerEnt: slot %u, expire: %u`

### 3.3 What this implies for interrupts and timers

`Confirmed`:

- the kernel has an EDBG interrupt thread
- the kernel has an EDBG timer thread
- the kernel can fall back between interrupt-driven and polling-like behavior (`EDBG: Leaving polling mode...`)

`Inferred`:

- the OAL/HAL layer below `nk.exe` provides the actual low-level timer tick and interrupt controller glue
- the stripped raw image does not expose OEM function names clearly enough to recover the full OEM interrupt address table

## 4. Driver Activation Model

### 4.1 Standard stream-driver contract

Many board drivers export the normal WinCE stream-driver entrypoints.

`Confirmed` examples:

- `DSK_Init`, `DSK_Deinit`, `DSK_IOControl`, `DSK_PowerUp`, `DSK_PowerDown`
- `COM_Init`, `COM_Deinit`, `COM_IOControl`, `COM_PowerUp`, `COM_PowerDown`
- `WAV_Init`, `WAV_Deinit`, `WAV_IOControl`, `WAV_PowerUp`, `WAV_PowerDown`
- `BUZ_Init`, `BUZ_Deinit`, `BUZ_IOControl`, `BUZ_PowerUp`, `BUZ_PowerDown`
- `PWO_Init`, `PWO_Deinit`, `PWO_IOControl`, `PWO_PowerUp`, `PWO_PowerDown`

`Inferred`:

- `device.exe` reads registry keys, loads the DLL named by each key, calls the `Init` entrypoint, then creates an active instance under `Drivers\\Active`

### 4.2 CASIO hardware helper layer: `cdm.dll`

`Confirmed` exports / strings from `cdm.dll`:

- `GetPhysMem`
- `GetVMem`
- `ReadPortDataEx`
- `WritePortDataEx`
- `CreateDelayedIntObj`
- `DeleteDelayedIntObj`
- `SetDelayedInt`
- `IsDelayedInt`
- `SetCritIntTime`
- `VRB_GetResDma`
- `VRB_RelResDma`
- `MutexDilayedInterruptServer`
- `VRB_EVENT0`
- `VRB_EVENT1`
- `VRB_MUTEX`

This is one of the most important findings in the image.

`Confirmed` meaning:

- CASIO factored common low-level services into `cdm.dll`
- drivers use it for:
  - memory-mapped I/O mapping
  - port I/O helpers
  - delayed interrupt handling
  - DMA resource reservation / release

`Inferred`:

- `cdm.dll` is acting as a mini board-support runtime shared by multiple drivers
- it likely wraps OEM-specific address mapping and interrupt policies so the individual drivers remain simpler

## 5. Hardware Discovery and Initialization by Subsystem

### 5.1 PCMCIA core

`Confirmed` from `pcmcia.dll`:

- registry roots:
  - `\\Drivers\\PCMCIA`
  - `Drivers\\Pcmcia`
  - `Drivers\\CASIO\\UTIL\\%s\\SOCK%d`
- events/UI:
  - `JacketNotifyEvent`
  - `PC card detected`
  - battery warning dialog strings
- card-management API use:
  - `CardRegisterClient`
  - `CardRequestWindow`
  - `CardMapWindow`
  - `CardRequestConfiguration`
  - `CardRequestIRQ`
  - `CardSystemInit`
  - `CardSystemPower`

`Confirmed` from `device.exe`:

- `device.exe` imports and references `CardSystemInit`, `CardRegisterClient`, tuple functions, and `Drivers\\PCMCIA`

`Inferred`:

- `pcmcia.dll` is initialized very early because several other drivers depend on it
- it owns socket detection, tuple parsing, attribute/common memory windows, config-register access, and IRQ arbitration for card-based devices

### 5.2 ATA / storage-card path

`Confirmed` from `atadisk.dll`:

- it is PCMCIA-backed, not memory-disk-only:
  - `CardRegisterClient`
  - `CardRequestWindow`
  - `CardRequestConfiguration`
  - `CardRequestIRQ`
  - `PCMCIA.DLL`
- expected disk parameters:
  - `CHSMode`
  - `Sectors`
  - `Heads`
  - `Cylinders`
  - `Folder`
- activity signal:
  - `EVENT_DISK_ACCESS_INDICATE`

`Inferred`:

- this driver handles ATA-compatible storage cards in the PCMCIA slot
- it likely supports both tuple-derived config and a registry override path for geometry/folder naming

### 5.3 NE2000 network card

`Confirmed` from `ne2000.dll`:

- it is an NDIS miniport:
  - `NdisInitializeWrapper`
  - `NdisMRegisterMiniport`
  - `NdisMRegisterIoPortRange`
  - `NdisMRegisterInterrupt`
  - `NdisMSynchronizeWithInterrupt`
- it performs hardware discovery/configuration using:
  - `HalGetBusDataByOffset`
  - `PCMCIA.DLL`
  - `CardRegisterClient`
  - `CardRequestWindow`
  - `CardRequestConfiguration`
  - `CardResetFunction`
- its registry expectations are visible:
  - `Drivers\\PCMCIA\\Detect\\`
  - `Drivers\\PCMCIA\\NE2000`
  - `Comm\\NE2000`
  - `Comm\\NE20001`
  - `Comm\\NE20001\\Parms`
  - `Comm\\NE2000\\Linkage`
  - `BusNumber`
  - `BusType`
  - `InterruptNumber`
  - `IoBaseAddress`
  - `Transceiver`
  - `Route`

Expected values from hardware / config:

- an I/O port base
- an interrupt number
- bus type / bus number
- PCMCIA tuple/configuration data sufficient to map the card and reset it

### 5.4 Serial / modem path

`Confirmed` from `serial.dll`:

- it is PCMCIA-backed:
  - `CardRequestIRQ`
  - `CardRequestConfiguration`
  - `CardMapWindow`
  - `CardResetFunction`
- it depends on `socket.dll`
- expected configuration strings include:
  - `NoScratchPad`
  - `ResetDelay`
  - `Sckt`
  - `Modem`
  - `Priority256`
  - `DeviceArrayIndex`

`Confirmed` from `socket.dll`:

- it tracks modem socket information under:
  - `Drivers\\CASIO\\Socket\\ComInf`
  - `Count`
  - `Type`
  - `Index`
  - `Info`
  - `ExtMdmCount`
  - `ModemSocketEvent`
- it exports:
  - `ModemSockOpenDevice`
  - `ModemSockCloseDevice`
  - `ModemSockReset`
  - `ModemSockGetSocketStatus`
  - `ModemSockEventOpen`
  - `ModemSockEventClose`

`Inferred`:

- the serial stack is not just a generic UART driver; it is tied to a removable modem/socket subsystem
- `socket.dll` is a CASIO helper that tracks physical modem/card socket state and serial attach/detach behavior

### 5.5 USB path

`Confirmed` from `usb.dll`:

- registry root:
  - `Drivers\\BuiltIn\\usb`
- runtime/event strings:
  - `UsbConnectEvent`
  - `FriendlyName`
  - `DeviceArrayIndex`
  - `Priority256`
  - `ControlPanel\\Comm`
- control-transfer field strings:
  - `bmRequestType`
  - `bRequest`
  - `wValue`
  - `wIndex`
  - `wLength`
- exported interface is stream-driver-like and COM-like:
  - `COM_Init`
  - `COM_Deinit`
  - `COM_IOControl`
  - `COM_PowerUp`
  - `COM_PowerDown`

`Inferred`:

- this is probably not a generic USB host stack driver
- it looks more like a built-in USB connection / serial-style link driver used for PC connectivity
- `StartingUSB.exe` likely exists to bring this link up after the main shell or connection manager is ready

### 5.6 Touch panel

`Confirmed` from `touch.dll`:

- it is a built-in driver under:
  - `\\Drivers\\BuiltIn\\Touch`
- it publishes into:
  - `HARDWARE\\DEVICEMAP\\TOUCH`
  - `HARDWARE\\DEVICEMAP\\TOUCH\\HardIcon`
- expected config includes:
  - `MaxCalError`
  - `HighPriority256`
  - `Priority256`

`Confirmed` from `gwes.exe`:

- `gwes.exe` expects:
  - `HARDWARE\\DEVICEMAP\\TOUCH`
  - `DriverName`
  - `CalibrationData`
  - `TouchCalibrate`
  - `TouchPanelEnable`
  - `TouchPanelReadCalibrationPoint`
  - `TouchPanelSetCalibration`
  - `TouchPanelGetDeviceCaps`

`Inferred`:

- `device.exe` activates `touch.dll`
- the driver populates `HARDWARE\\DEVICEMAP\\TOUCH`
- `gwes.exe` then binds the touch driver through that devicemap entry and performs calibration/UI work

### 5.7 Keyboard and special keys

`Confirmed` from `keybddr.dll`:

- built-in driver path:
  - `\\Drivers\\BuiltIn\\KeyBd`
- per-key mappings/config:
  - `Drivers\\CASIO\\keybd`
  - `Drivers\\CASIO\\keybd\\keys\\ButtonA`
  - `Drivers\\CASIO\\keybd\\keys\\ButtonB`
  - `Drivers\\CASIO\\keybd\\keys\\ButtonS`
  - `Drivers\\CASIO\\keybd\\keys\\ButtonV`
- timing/config values:
  - `KeyRepeatDelayTime`
  - `KeyRepeatOffTime`
  - `Priority256`
- board-specific events:
  - `OEMBacklightContrastIncEvent`
  - `OEMBacklightContrastDecEvent`
  - `OEMBacklightBrightIncBatteryEvent`
  - `OEMBacklightBrightDecBatteryEvent`
  - `OEMBacklightBrightIncEXPowerEvent`
  - `OEMBacklightBrightDecEXPowerEvent`
  - `OEMBacklightAutoDimONEvent`
  - `OEMBacklightAutoDimOFFEvent`

`Confirmed` from `gwes.exe`:

- it references:
  - `HARDWARE\\DEVICEMAP\\KEYBD`
  - `KeybdDriverInitialize`
  - `KeybdDriverInitializeEx`
  - `KeybdDriverSetMode`
  - `KeybdDriverVKeyToUnicode`
  - `ControlPanel\\Keybd`
  - `InitialDelay`
  - `RepeatRate`

`Inferred`:

- the keyboard driver also owns several board hotkeys and backlight-control keys
- it may provide a diagnostic-boot shortcut, given:
  - `\\Windows\\DiagBoot`
  - `\\Windows\\DiagBoot.exe`
  - `\\Storage Card\\diag\\diagboot.exe`

### 5.8 Audio and buzzer

`Confirmed` from `wavedev.dll`:

- stream-driver exports:
  - `WAV_Init`
  - `WAV_Deinit`
  - `WAV_IOControl`
  - `WAV_PowerUp`
  - `WAV_PowerDown`
- DMA-related imports:
  - `VRB_GetResDma`
  - `VRB_RelResDma`

`Confirmed` from disassembly at `0x019111dc`:

- the driver uses a mapped register block stored in a global pointer
- it accesses at least these offsets from the device base:
  - `+0x3c0`
  - `+0x3c4`
  - `+0x3f4`
  - `+0x880`
  - `+0x884`
  - `+0x888`
- it repeatedly busy-waits on a status register before issuing the next write
- it programs control words including values derived from the requested sample rate / mode

This is strong evidence of real hardware programming rather than a stub.

`Confirmed` from `buzzer.dll` and `bltinbuz.dll`:

- buzzer categories:
  - `BuzzWarning`
  - `BuzzAlarm`
  - `BuzzApplication`
  - `BuzzKey`
  - `BuzzScreen`
- file-backed patterns:
  - `\\Windows\\button.buz`
  - `\\Windows\\tap.buz`
- registry path:
  - `Drivers\\CASIO\\buzzer\\path`

`Inferred`:

- `wavedev.dll` is the main audio hardware driver
- the buzzer drivers are lighter-weight notification/audio devices layered above or beside it

### 5.9 NAND flash, object store, and disk naming

`Confirmed` from `nanddisk.dll`:

- built-in NAND path exists via `NANDAccess.dll`:
  - `Drivers\\BuiltIn\\nanddisk`
- filesystem dependency:
  - `fatfs.dll`
- per-driver registry reads:
  - `Folder`
  - `RegOpenKeyEx`
  - `RegQueryValueEx`
- media/event strings:
  - `EVENT_DISK_ACCESS_INDICATE`
  - `NAND_RESERVE_WARNING_0`
  - `NAND_RESERVE_WARNING_1`

Most importantly, explicit failure strings prove the driver has its own flash controller logic:

- `Cannot allocate memory for NAND_FLASH_REG`
- `Cannot allocate memory for NAND_FLASH_OBJ`
- `Wait Busy Time Out in ERASE`
- `Wait Busy Time Out in Write`
- `DMA transfer Time out`
- `NAND CORE: read Timeout error %d`
- `NAND CORE: read ECC error at %d sector`
- `DMA Error history %x`
- `CheckDMAEnd: Error timeout`
- `Last BlockReplace`
- `Error: Cannot correct`

`Confirmed` from source-path strings:

- `o:\\wince300\\platform\\pf2001\\drivers\\nanddisk\\nandfunc\\.\\entryfunc.c`

That path directly ties the NAND implementation to the PF2001 platform tree.

`Confirmed` from `NANDAccess.dll`:

- raw NAND helpers:
  - `NANDOpen`
  - `NANDClose`
  - `NANDGetInfo`
  - `NANDReadSector`
  - `NANDWriteSector`
  - `NANDGetAccessFlag`
  - `NANDClearAccessFlag`

`Confirmed` from `GetDisk.dll`:

- CASIO disk naming and storage layout:
  - `InnerProgramDisk`
  - `InnerDisk`
  - `ProgramFolder`
  - `UserFolder`
  - `BackupFolder`
  - `TempFolder`
  - `GetSystemDisk`
  - `GetInnerProgramDisk`
  - `GetInnerUserDisk`
  - `GetProgramDiskName`
  - `GetUserDiskName`
  - `Drivers\\CASIO\\DiskName`
  - `Drivers\\PCMCIA\\ATADisK`

`Confirmed` from `Boot.exe`:

- it manipulates:
  - `\\Windows\\Release.txt`
  - `\\Windows\\Name.txt`
  - `\\Windows\\Type.txt`
  - `\\Windows\\Build.txt`
  - `\\Windows\\Initialized.$$$`
  - `\\Windows\\Safe.$$$`
  - `\\Windows\\Safe.reg`
  - `\\Windows\\System.$$$`
  - `\\System.reg`
  - `\\PATCH\\*.*`
  - `\\Windows\\PATCHINST.EXE`
  - `\\NAND Disk\\Program Files\\Version.txt`

`Inferred`:

- NAND is the main internal persistent store
- `Boot.exe` is a CASIO first-boot / recovery / patch / maintenance helper that runs after the core OS is up
- it is responsible for some combination of:
  - finalizing registry files
  - safe-mode bookkeeping
  - patch installation
  - release/build metadata handling

### 5.10 EEPROM and power management

`Confirmed` from `eeprom.dll`:

- it uses:
  - `GetVMem`
  - `EromMutex`

This indicates EEPROM is memory-mapped and serialized via a mutex.

`Confirmed` from `PowerOn.dll`:

- built-in driver path:
  - `Drivers\\BuiltIn\\poweron`
- callback support:
  - `RegistEventCallBack`
  - `EVTCBACK.dll`
- persistence hooks:
  - `CGDFlushRegistry`
  - `CGDFlushAlarm`
- user-visible fault handling:
  - high-current shutdown messages
  - restart-after-OS-failure messages
- related files/events:
  - `POWEROFF_WINDOW`
  - `POWERON_MESSAGE`

`Inferred`:

- `PowerOn.dll` is not just cosmetic UI
- it likely monitors board power/current fault conditions and coordinates emergency persistence and restart behavior

### 5.11 Digital camera

`Confirmed` from `digcam.dll`:

- registry path:
  - `Drivers\\CASIO\\Digtal Camera`
- PCMCIA integration:
  - `CardRequestWindow`
  - `CardMapWindow`
  - `CardRequestConfiguration`
  - `CardRequestIRQ`
- visible register names:
  - `CPORT`
  - `CSTAT`
  - `RDAT`
  - `WDAT`
  - `WADR`
  - `SADR`
  - `RSTAT`
  - `WSTAT`
  - `NUMB`
  - `AREA`
  - `FOMT`
  - `SIZE`
  - `ROWA`

`Inferred`:

- this is a real hardware camera interface driver, probably card-attached or socket-attached
- the register names imply a command/data/status interface rather than a pure file/protocol wrapper

## 6. Interrupts, Timers, and DMA

### 6.1 Interrupts

`Confirmed`:

- `nk.exe` contains an EDBG interrupt thread and interrupt initialization logic
- `ne2000.dll` registers an interrupt with NDIS via `NdisMRegisterInterrupt`
- `pcmcia.dll` owns `CardRequestIRQ` and `CardReleaseIRQ`
- `serial.dll`, `atadisk.dll`, and `digcam.dll` all use PCMCIA IRQ request/release flows
- `cdm.dll` exports delayed-interrupt helpers:
  - `CreateDelayedIntObj`
  - `SetDelayedInt`
  - `IsDelayedInt`

`Inferred`:

- removable-card devices do not wire themselves directly to the kernel; they go through the PCMCIA subsystem for IRQ ownership
- CASIO used `cdm.dll` to normalize or defer some interrupt handling into worker-thread context

### 6.2 Timers

`Confirmed`:

- the kernel debug Ethernet stack creates a timer thread:
  - `Error creating timer thread`
  - `TimerEnt: slot %u, expire: %u`
- several drivers and apps expose `Priority256`, implying explicit thread-priority tuning for time-sensitive work
- `gwes.exe` has timer-driven UI behavior:
  - `Timer`
  - notification/snooze/alarm handling

`Inferred`:

- the low-level system tick source is hidden in OEM/OAL code inside or below `nk.exe`, but not symbolized enough in this dump to name the exact timer block
- at least some device-side work is split between ISR-like capture and timer/worker processing

### 6.3 DMA

`Confirmed`:

- `cdm.dll` exports `VRB_GetResDma` and `VRB_RelResDma`
- `wavedev.dll` imports both DMA helpers
- `nanddisk.dll` logs explicit DMA transfer and DMA completion failures

`Inferred`:

- DMA is centrally brokered rather than managed ad hoc by each driver
- audio almost certainly reserves a DMA resource through `cdm.dll`
- NAND uses a controller DMA path with timeout/error recovery

What is directly provable:

- DMA exists
- at least audio and NAND depend on it
- there is a shared DMA resource abstraction in `cdm.dll`

What is not provable from this repo alone:

- exact DMA channel numbers
- exact controller register layout for the NAND DMA engine

## 7. Expected Hardware Values and Assumptions

These are the clearest parameter names exposed by the image.

### 7.1 Network

`Confirmed` expected values:

- `BusNumber`
- `BusType`
- `InterruptNumber`
- `IoBaseAddress`
- `Transceiver`
- `Route`

### 7.2 ATA / storage card

`Confirmed` expected values:

- `CHSMode`
- `Sectors`
- `Heads`
- `Cylinders`
- `Folder`

### 7.3 Touch

`Confirmed` expected values:

- `CalibrationData`
- `MaxCalError`
- `Priority256`
- `HighPriority256`

### 7.4 Keyboard

`Confirmed` expected values:

- `KeyRepeatDelayTime`
- `KeyRepeatOffTime`
- per-button registry mappings for `ButtonA/B/S/V`

### 7.5 Serial / modem / USB

`Confirmed` expected values:

- `NoScratchPad`
- `ResetDelay`
- `Sckt`
- `DeviceArrayIndex`
- `FriendlyName`
- descriptor/control fields for USB requests

### 7.6 NAND

`Confirmed` expected assumptions:

- valid NAND info via `NANDGetInfo`
- valid reserve/bad-block replacement area
- successful ECC on reads
- timely completion of busy / DMA phases

## 8. Probable Driver Bring-Up Order

This is the most likely activation order consistent with the evidence.

### 8.1 Very early

`Confirmed / Inferred mix`:

1. `nk.exe`
2. `filesys.exe`
3. `gwes.exe` and `device.exe` in the first wave of user processes

### 8.2 Device manager wave

`Inferred`, based on dependencies:

1. `pcmcia.dll`
2. built-in always-present devices:
   - `touch.dll`
   - `keybddr.dll`
   - `PowerOn.dll`
   - `nanddisk.dll`
   - likely `usb.dll`
   - likely `bltinbuz.dll`
3. PCMCIA-dependent clients after the PCMCIA core is live:
   - `atadisk.dll`
   - `serial.dll`
   - `ne2000.dll`
   - `digcam.dll`
4. audio path:
   - `wavedev.dll`

Rationale:

- PCMCIA client drivers cannot do anything useful before `pcmcia.dll` is initialized.
- `gwes.exe` depends on devicemap entries from touch/keyboard drivers.
- disk naming and boot helper apps depend on NAND/storage services being up.

### 8.3 Application / shell wave

`Inferred`:

- `coshell.exe` is the real end-user shell
- `shell.exe` is the debug command shell, not the normal UI shell
- `Boot.exe`, `BootSafeShell.exe`, `SafeShell.exe`, `StartingUSB.exe`, `PCLASRV.exe`, restore tools, and version tools run after the core OS and storage layers are already alive

## 9. What Can and Cannot Be Claimed

### 9.1 High-confidence findings

- Platform family: CASIO PF2001 / J-740 / JX-740 on VR4131 MIPS
- Core early processes: `filesys.exe`, `gwes.exe`, `device.exe`
- `device.exe` manages `Drivers\\BuiltIn`, `Drivers\\PCMCIA`, and `Drivers\\Active`
- PCMCIA is a central bus for multiple removable devices
- NAND is the main internal persistent store and has ECC/bad-block/DMA logic
- `cdm.dll` is the common low-level helper for mapping, delayed interrupts, and DMA resources
- audio and NAND both use DMA-backed flows
- touch and keyboard publish through `HARDWARE\\DEVICEMAP`

### 9.2 Things that remain unresolved

- exact `HKLM\\init` launch numbers and dependency numbers
- exact OEM boot ROM handoff into `nk.exe`
- exact numeric interrupt-to-SYSINTR mapping
- exact timer source block used for the system tick
- exact MMIO base addresses for most devices

## 10. Concise Bottom Line

The boot architecture is standard WinCE 3.0 with a CASIO board-support layer:

- `nk.exe` brings up the MIPS kernel and debug Ethernet
- `filesys.exe` materializes the registry/object store
- `gwes.exe` and `device.exe` form the core of early user-mode bring-up
- `device.exe` activates built-in and PCMCIA drivers using the standard stream-driver model
- `cdm.dll` provides CASIO-specific low-level services for mapped I/O, delayed interrupts, and DMA resources
- removable hardware hangs primarily off the PCMCIA subsystem
- built-in persistent storage is NAND, with explicit ECC, bad-block replacement, and DMA timeout handling
- input comes from built-in touch and keyboard drivers registered into `HARDWARE\\DEVICEMAP`
- later CASIO apps layer on shell, patching, safe mode, restore, and PC connectivity once the core OS is already running
