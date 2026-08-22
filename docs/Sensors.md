# Sensor Data

Test

## Sensor Test

Test

### Sensors

Why UEFI Cannot Run Background Tasks Post-Boot

#### 1. The ExitBootServices() Lifecycle Boundary • Boot Services (gBS):
All EDK2 timers (gBS->SetTimer), event notifications, task priority levels (TPLs), memory allocation routines, and device drivers only exist prior to kernel handoff. • When the kernel starts booting, it invokes gBS->ExitBootServices(). This call destroys all event queues and timers, and the OS reclaims all memory marked as EfiBootServicesCode and EfiBootServicesData for general OS page allocations. #### 2. Runtime Services (gRT) Are Strictly Passive • Code placed in EfiRuntimeServicesCode remains in memory after ExitBootServices(). • However, UEFI Runtime Services are passive, synchronous function calls (such as GetVariable(), SetVariable(), and ResetSystem()). • Runtime drivers cannot: • Create background threads or schedulers. • Register hardware timers or interrupts. • Allocate dynamic memory. • Initiate asynchronous I/O autonomously. • A runtime service only executes when an OS kernel thread explicitly calls into the runtime dispatch table.

#### 3. Hardware Ownership Handoff

• When Linux boots at EL1/EL2 (on ARM64 / Cortex-A76), the kernel assumes exclusive control of: • The MMU and virtual memory translation. • The Generic Interrupt Controller (GIC). • The ARM generic architected timers. • All I/O bus controllers (I2C, USB, PCIe, etc.). • Any background firmware attempting to access hardware peripherals simultaneously would cause bus contention and kernel panics. ──────

How Sensor Streaming to a BMC is Implemented
To send telemetry across all boot phases, standard server and embedded BMC architectures use one of the following models:

│ ARM Application Cores (A76) │ │ Pre-OS Boot Phase │ OS Runtime Phase │ │ (EDK2 UEFI) │ (Linux Kernel/User) │ │ ┌────────────────────────┐ │ ┌─────────────────────────┐ │ │ │ RpiRedfishPkg / DXE │ │ │ bmc-agent / telegraf │ │ │ └───────────┬────────────┘ │ └────────────┬────────────┘ │ │ (CDC-NCM / Redfish HTTP) │ (CDC-NCM / Redfish HTTP) ▼ ▼ │ BMC (OpenBMC / USB Host Interface) │ ▲ ▲ │ │ ┌─────────────┴────────────┐ ┌──────────────┴────────────┐ │ Out-of-Band (I2C/ADC) │ │ Aux Co-processor (VPU) │ └──────────────────────────┘ └───────────────────────────┘

#### 1. Tiered Hand-Off (Standard Redfish / BMC Model — Recommended)

Because your setup uses the Redfish Host Interface over USB CDC-NCM (RpiRedfishPkg / usbnet):

• Boot Phase (UEFI): Your EDK2 DXE drivers (RpiRedfishPkg, ActiveCoolerDxe) communicate with the BMC over the USB CDC-NCM gadget during pre-boot. • OS Phase (Linux): Upon booting, the Linux cdc_ncm driver binds to the USB-C / USB network interface. A lightweight Linux service (e.g., a systemd unit running a script, ipmid, or a telemetry daemon) takes over and periodically POSTs sensor readings to the BMC over the same Redfish HTTP endpoint.

#### 2. Out-of-Band (OOB) Telemetry via BMC Direct Polling

• The BMC monitors sensors independently over dedicated hardware buses without involving the host CPU: • The BMC reads voltage/temperature sensors over I2C/SMBus/PMBus. • The BMC samples analog rails directly via ADC lines. • This operates continuously regardless of whether the Pi 5 is powered off, in reset, in UEFI Setup, or running Linux.

#### 3. Co-Processor / Auxiliary Microcontroller Offload

• On the Raspberry Pi 5 / BCM2712 platform, the VideoCore VPU and the RP1 southbridge (Cortex-M3 cores) run firmware independently of the ARM Cortex-A76 application cores. • Telemetry gathered by the VPU or RP1 can be forwarded directly to a BMC over hardware I2C/UART/SPI lines across all boot phases without ARM CPU intervention.

#### 4. ACPI Thermal Methods (_TMP / _OST)

• In ACPI-based UEFI systems, you can define ACPI Thermal Zones and sensors in an SSDT (e.g., SsdtThermal.asl in the RPi5 AcpiTables overlay). • The OS ACPI thermal subsystem periodically evaluates these AML methods at runtime. ──────

Summary
Approach | Runs Pre-OS? | Runs Post-OS? | Complexity | Notes ---------------------------|--------------|---------------|------------|------------------------------------------- EDK2 Timer / DXE | Yes | No | Low | Dies at ExitBootServices() EDK2 Runtime Service | Yes | Passive only | Medium | Requires OS to explicitly poll it UEFI + OS Agent Hand-off | Yes | Yes | Low | Standard approach via Redfish/CDC-NCM Out-of-Band (OOB) BMC I2C | Yes | Yes | Low | Independent of host CPU state VPU / RP1 Co-processor | Yes | Yes | High | Requires VideoCore/RP1 firmware customizat

1. EDK2 (UEFI) — Not for Background Tasks
• Why it doesn't work: When an OS kernel (like Linux) takes over, it calls gBS->ExitBootServices(). This tears down the UEFI scheduler, timers, and event loops, and releases all BootServicesCode and BootServicesData memory back to the kernel. • Runtime Services limitation: EDK2 does support Runtime Services (which persist in memory after boot), but these are strictly passive and synchronous. They only execute when the OS kernel explicitly makes an EFI call (e.g., GetVariable(), ResetSystem()). EDK2 cannot schedule background threads, handle OS-independent timer interrupts, or run autonomously in the Rich OS (Normal World). • (Note: On x86, SMM can run behind the OS via periodic SMIs, and on ARM, the equivalent is Standalone MM in Secure World, but this is handled by TrustZone, not standard UEFI execution). ──────

2. TF-A (Trusted Firmware-A @ EL3) — Possible, but Strongly Discouraged
• TF-A's Purpose: TF-A runs at the highest privilege level (EL3 / Secure Monitor). Its job is minimal: boot handoff, power management (PSCI), and context-switching between Normal World (Linux/EDK2) and Secure World (OP-TEE). • Why TF-A has I2C/USB: The I2C and USB drivers in TF-A exist almost exclusively for boot-phase tasks (e.g., configuring a PMIC over I2C before releasing CPU cores, or basic USB DFU recovery during early board bring-up). These are simple, blocking, polling drivers. • Why background services in TF-A are bad practice: • Interrupt Latency & Starvation: Running continuous or periodic I2C/USB I/O in EL3 blocks or severely degrades Linux kernel interrupt latency (jitter). • No Preemption/Scheduler: TF-A does not have a multitasking OS scheduler. Handling transactions requires busy- waiting or routing secure timer interrupts (FIQ / Group 0) directly into EL3. • Security Surface: Putting complex protocol stacks (especially USB) in EL3 expands the most privileged attack surface on the SoC.

──────

3. OP-TEE (Secure World @ S-EL1 / S-EL0) — The Architecturally Correct Firmware Approach
If you need the service to run independently of the host OS and survive even if Linux crashes or is compromised, OP-TEE is designed for this.

#### How it works

Peripheral Partitioning: You configure the SoC firewall / TrustZone Controller (TZPC/TZASC) so that the specific
communication bus (e.g., I2C or UART) is configured as Secure-only. The Linux kernel will not have access to it, preventing bus contention.

Interrupt Handling: A hardware timer or sensor interrupt is configured as a Secure Interrupt (FIQ / Group 1
Secure). When triggered, the core traps from Linux to EL3, which forwards it to S-EL1 (OP-TEE OS).

Execution: OP-TEE receives the interrupt, dispatches it to a Core Driver / Early TA (Trusted Application) or
Pseudo-TA, reads the telemetry, and transmits it to the BMC.

#### Communication Bus Considerations:

• I2C / SPI / UART (Recommended): These are lightweight, register-mapped peripherals with minimal state machines. Writing an OP-TEE driver to send periodic sensor packets (e.g., IPMB/MCTP or custom framing) over I2C/UART is standard practice. • USB (Not Recommended): An xHCI/EHCI USB host controller stack is massive, DMA-heavy, and complex to run inside OP-TEE. If your interface to the BMC must be USB, it is significantly more complex to isolate in Secure World. ──────

Summary & Recommended Architectural Choices
Approach | Runs Post-B… | OS Independ… | Feasibility / Recommendation ------------------------------|--------------|--------------|------------------------------------------------------ EDK2 Standard Driver | ❌ No | ❌ No | Terminated at ExitBootServices(). TF-A (EL3) | ⚠️ Hacky | ⚠️ Yes | Violates EL3 design principles; causes high CPU | | | jitter; complex drivers risky in EL3. OP-TEE (S-EL1 / S-EL0) | ✅ Yes | ✅ Yes | Best firmware approach. Use Secure Interrupts + | | | Secure I2C/UART driver in an Early TA. Normal World Linux Service | ✅ Yes | ❌ No | Easiest approach. A standard Linux systemd daemon / | | | kernel module talking to the BMC (unless isolation | | | from the OS is a strict requirement). Dedicated Co-processor (SCP) | ✅ Yes | ✅ Yes | If the SoC has an auxiliary Cortex-M / System | | | Control Processor (like an RP2040 on Pi-BMC designs | | | or an integrated SCP), it is the industry standard | | | for out-of-band telemetry.
