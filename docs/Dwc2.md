# Synopsys DWC2 USB Controller (DwUsbHostDxe)

In the EDK2 Raspberry Pi platform architecture, the Synopsys DWC2 USB controller is a memory-mapped peripheral integrated directly into the Broadcom SoC, rather than existing behind a standard PCI/PCIe bridge.

Because EDK2 is entirely single-threaded and lacks a native asynchronous runtime or hardware interrupt service routines (ISRs) for general payloads, asynchronous behavior—mandated by the `EFI_USB2_HC_PROTOCOL`—must be simulated. For non-blocking endpoints (e.g., interrupt or bulk async endpoints), `DwUsbHostDxe` schedules transactions using an internal state machine driven by UEFI Timer Events (`gBS->CreateEvent`).

The architecture requires mapping EDK2's non-blocking callbacks to the Synopsys DWC2 Host Channels (`HCCHAR`, `HCTSIZ`, `HCINT`, `HCDMA`).

## EDK2 Driver Architectural Mapping

1. **Usb2HcAsyncTransfer API:** The entry points executed by EDK2's generic `UsbBusDxe`. If requested asynchronously, `DwUsbHostDxe` flushes the data cache to RAM, maps the CPU virtual address to a bus physical address (if necessary), attaches a tracking structure to an internal linked list, and returns `EFI_SUCCESS` immediately.
2. **Periodic Timer Callback:** A hardware polling routine registered via `gBS->SetTimer()` running at 1ms intervals. This acts as the async executor, scanning the DWC2's host channel interrupt registers (`HCINTn`) via `MmioRead32` to update transfer states.
3. **Completion & Coherency:** When a channel signals `XFER_COMP` or an error, the timer routine invalidates the CPU data cache (ensuring memory coherency for inbound DMA), removes the node from the active queue, frees the channel slot, and executes the caller-provided `EFI_ASYNC_USB_TRANSFER_CALLBACK`.

## Low-Level C Implementation Structure

### 1. Register Macros and Context Trackers

Instead of magic numbers, `DwUsbHostDxe` relies on offset macros relative to the controller's MMIO base address, which is typically retrieved via `PcdGet32 (PcdDwc2BaseAddress)`.

```c
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Protocol/Usb2HostController.h>

#define DWC2_MAX_CHANNELS     8

/* DWC2 Channel Register Offsets */
#define DWC2_HC_BASE(Ch)      (0x500 + ((Ch) * 0x20))
#define DWC2_HCCHAR(Ch)       (DWC2_HC_BASE(Ch) + 0x00)
#define DWC2_HCINT(Ch)        (DWC2_HC_BASE(Ch) + 0x08)
#define DWC2_HCTSIZ(Ch)       (DWC2_HC_BASE(Ch) + 0x10)
#define DWC2_HCDMA(Ch)        (DWC2_HC_BASE(Ch) + 0x14)

/* HCINT Bit Masks */
#define DWC2_HCINT_XFERCOMP   BIT0
#define DWC2_HCINT_STALL      BIT3
#define DWC2_HCINT_AHBERR     BIT4
#define DWC2_HCINT_ACK        BIT5
#define DWC2_HCINT_NAK        BIT4
#define DWC2_HCINT_NYET       BIT6

// Structure tracking an ongoing transaction
typedef struct {
  LIST_ENTRY                        Link;
  UINT8                             ChannelIndex;
  UINT8                             DeviceAddress;
  UINT8                             EndPointAddress;
  UINT8                             DataToggle;
  VOID                              *Buffer;       // CPU Virtual Address
  EFI_PHYSICAL_ADDRESS              DmaBuffer;     // Bus Physical Address
  UINTN                             DataLength;
  BOOLEAN                           IsDirectionIn; // TRUE for IN endpoints
  EFI_ASYNC_USB_TRANSFER_CALLBACK   Callback;
  VOID                              *Context;
  UINT32                            ResultStatus;
} DWUSB_ASYNC_REQUEST;

// Global driver state
typedef struct {
  UINTN                             Signature;
  EFI_USB2_HC_PROTOCOL              Usb2Hc;
  EFI_EVENT                         TimerEvent;
  LIST_ENTRY                        AsyncQueue;
  UINT32                            AllocatedChannels;
  EFI_PHYSICAL_ADDRESS              Dwc2BaseAddress;
} DWUSB_HC_DEVICE;

```

### 2. Submitting an Asynchronous Transfer

When a non-blocking transfer is initiated, the driver must explicitly handle ARM cache coherency before configuring the DWC2 DMA engine.

```c
EFI_STATUS
EFIAPI
DwUsb2HcAsyncBulkTransfer (
  IN     EFI_USB2_HC_PROTOCOL               *This,
  IN     UINT8                              DeviceAddress,
  IN     UINT8                              EndPointAddress,
  IN     UINT8                              DeviceSpeed,
  IN     UINTN                              MaximumPacketLength,
  IN OUT VOID                               *Data,
  IN OUT UINTN                              *DataLength,
  IN OUT UINT8                              *DataToggle,
  IN     UINTN                              TimeOut,
  IN     EFI_USB2_HC_TRANSLATOR             *Translator,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK    Callback,
  IN     VOID                               *Context OPTIONAL
  )
{
  DWUSB_HC_DEVICE      *HcDev;
  DWUSB_ASYNC_REQUEST  *Request;
  UINT8                ChannelIdx;

  HcDev = (DWUSB_HC_DEVICE *)This;

  // 1. Allocate a hardware host channel slot
  for (ChannelIdx = 0; ChannelIdx < DWC2_MAX_CHANNELS; ChannelIdx++) {
    if ((HcDev->AllocatedChannels & (1 << ChannelIdx)) == 0) {
      break;
    }
  }
  if (ChannelIdx == DWC2_MAX_CHANNELS) {
    return EFI_OUT_OF_RESOURCES;
  }
  HcDev->AllocatedChannels |= (1 << ChannelIdx);

  // 2. Allocate tracking node
  Request = AllocateZeroPool (sizeof (DWUSB_ASYNC_REQUEST));
  if (Request == NULL) {
    HcDev->AllocatedChannels &= ~(1 << ChannelIdx);
    return EFI_OUT_OF_RESOURCES;
  }

  // 3. Cache Maintenance for ARM DMA
  // DWC2 performs DMA; EDK2 must flush CPU caches to main memory
  if (Data != NULL && *DataLength > 0) {
    WriteBackInvalidateDataCacheRange (Data, *DataLength);
  }

  Request->ChannelIndex    = ChannelIdx;
  Request->Buffer          = Data;
  Request->DataLength      = *DataLength;
  Request->IsDirectionIn   = ((EndPointAddress & 0x80) != 0);
  Request->Callback        = Callback;
  Request->Context         = Context;

  // 4. Hardware Configuration
  MmioWrite32 (HcDev->Dwc2BaseAddress + DWC2_HCDMA (ChannelIdx), (UINT32)(UINTN)Data);
  // (Additional setup for HCCHAR and HCTSIZ omitted for brevity)

  // Set CHEN to fire the transaction
  MmioOr32 (HcDev->Dwc2BaseAddress + DWC2_HCCHAR (ChannelIdx), BIT31);

  // 5. Append to queue
  InsertTailList (&HcDev->AsyncQueue, &Request->Link);

  return EFI_SUCCESS;
}

```

### 3. The Polling "Executor" via UEFI Timer Event

Because the DWC2 on the Raspberry Pi does not fire PCI-style MSIs that EDK2 can easily intercept, the polling function strictly reads the memory-mapped `HCINT` registers.

```c
VOID
EFIAPI
DwUsb2HcTimerCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DWUSB_HC_DEVICE      *HcDev;
  LIST_ENTRY           *Link;
  DWUSB_ASYNC_REQUEST  *Request;
  UINT32               HcIntReg;

  HcDev = (DWUSB_HC_DEVICE *)Context;
  if (IsListEmpty (&HcDev->AsyncQueue)) {
    return;
  }

  Link = GetFirstNode (&HcDev->AsyncQueue);
  while (!IsNull (&HcDev->AsyncQueue, Link)) {
    Request = (DWUSB_ASYNC_REQUEST *)Link;
    Link    = GetNextNode (&HcDev->AsyncQueue, Link);

    HcIntReg = MmioRead32 (HcDev->Dwc2BaseAddress + DWC2_HCINT (Request->ChannelIndex));

    // Case A: Transfer Finished Successfully
    if (HcIntReg & DWC2_HCINT_XFERCOMP) {
      Request->ResultStatus = EFI_USB_NOERROR;

      // Maintain Cache Coherency for IN transfers (Device -> RAM)
      if (Request->IsDirectionIn && Request->Buffer != NULL && Request->DataLength > 0) {
        InvalidateDataCacheRange (Request->Buffer, Request->DataLength);
      }

      Request->Callback (Request->Buffer, Request->DataLength, Request->Context, Request->ResultStatus);

      HcDev->AllocatedChannels &= ~(1 << Request->ChannelIndex);
      RemoveEntryList (&Request->Link);
      FreePool (Request);
    }
    // Case B: Hardware Stall or AHB Error
    else if (HcIntReg & (DWC2_HCINT_STALL | DWC2_HCINT_AHBERR)) {
      Request->ResultStatus = EFI_USB_ERR_STALL;
      Request->Callback (Request->Buffer, 0, Request->Context, Request->ResultStatus);

      HcDev->AllocatedChannels &= ~(1 << Request->ChannelIndex);
      RemoveEntryList (&Request->Link);
      FreePool (Request);
    }
    // Case C: NAK (Keep transaction queued, retry)
    else if (HcIntReg & DWC2_HCINT_NAK) {
      // Clear NAK interrupt bit
      MmioWrite32 (HcDev->Dwc2BaseAddress + DWC2_HCINT (Request->ChannelIndex), DWC2_HCINT_NAK);
      // Re-enable the hardware channel
      MmioOr32 (HcDev->Dwc2BaseAddress + DWC2_HCCHAR (Request->ChannelIndex), BIT31);
    }
  }
}

```

### 4. DriverBinding Registration

The timer task must be registered at `TPL_NOTIFY` during `DriverBindingStart`.

```c
EFI_STATUS
InitializeDwUsb2Driver (
  IN DWUSB_HC_DEVICE *HcDev
  )
{
  EFI_STATUS Status;

  InitializeListHead (&HcDev->AsyncQueue);
  HcDev->AllocatedChannels = 0;
  HcDev->Dwc2BaseAddress   = PcdGet32 (PcdDwc2BaseAddress);

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  DwUsb2HcTimerCallback,
                  HcDev,
                  &HcDev->TimerEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Set interval to 1ms (10,000 in 100ns units)
  Status = gBS->SetTimer (HcDev->TimerEvent, TimerPeriodic, 10000);
  return Status;
}

```

## Implementation Realities for Raspberry Pi

* **Cache Coherency:** EDK2 does not enforce a hardware coherent DMA engine for the DWC2 controller by default. You *must* use `WriteBackInvalidateDataCacheRange()` before outbound transfers and `InvalidateDataCacheRange()` after inbound transfers, or the USB stack will read stale CPU cache data instead of the newly received USB packets.
* **Absence of `EFI_PCI_IO_PROTOCOL`:** Because this is an SoC peripheral, standard PCI `Map()` and `Unmap()` paradigms do not apply. If the Raspberry Pi firmware limits DMA addresses to a specific 32-bit window (e.g., `< 3GB`), buffers must be allocated using `UncachedMemoryAllocationLib` or mapped via standard EDK2 DMA libraries rather than passing bare stack pointers.
* **Split Transactions:** If you are supporting keyboards or mice behind a high-speed USB hub, the `DwUsbHostDxe` timer callback must implement the DWC2 Split Transaction state machine (CSPLIT/SSPLIT). This requires advancing the split state on `ACK` or `NYET` interrupt flags within the timer callback, as the hardware will not handle hub scheduling automatically.

---

To bridge your `DwUsbHostDxe` driver to `SnpDxe` and utilize DMA, you must connect the two through EDK2's USB networking stack. `DwUsbHostDxe` does not interact with `SnpDxe` directly; instead, it provides the low-level transport for the CDC-NCM driver, which in turn exposes the Simple Network Protocol (SNP) for Redfish.

Here is how to map the architecture and implement safe DMA using EDK2's abstraction libraries.

## The Architectural Bridge

1. **DwUsbHostDxe (Host Controller):** Manages the DWC2 hardware, handles DMA mapping, and executes the async state machine.
2. **UsbBusDxe (Bus Driver):** Enumerates the USB devices and provides the `EFI_USB_IO_PROTOCOL`.
3. **UsbCdcNcmDxe (Network Driver):** Binds to the CDC-NCM interface, translates network packets into USB Bulk IN/OUT requests, and exposes `EFI_SIMPLE_NETWORK_PROTOCOL` (`SnpDxe` consumes this).

For `SnpDxe` to receive packets, `UsbCdcNcmDxe` continuously queues asynchronous **Bulk IN** transfers to `DwUsbHostDxe`. When a packet arrives over the wire, your `DwUsb2HcTimerCallback` completes the transaction, triggering the CDC-NCM callback, which then alerts `SnpDxe` that a frame is ready.

## Implementing DMA via EDK2 `DmaLib`

On ARM architectures like the Raspberry Pi, passing CPU virtual addresses directly to the DWC2 `HCDMA` register is unsafe due to memory fragmentation and cache coherency.

Instead of manually flushing caches with `WriteBackInvalidateDataCacheRange`, you must use `DmaLib` (or `IoMmuProtocol`). This abstracts the cache maintenance and provides a safe bus-physical address (or a bounce buffer if the memory region isn't DMA-capable).

### 1. Update the Request Structure

Add variables to track the DMA mapping state so it can be unmapped when the timer callback finishes.

```c
#include <Library/DmaLib.h>

typedef struct {
  // ... existing fields ...
  VOID                              *Buffer;           // Original CPU Address from SnpDxe
  EFI_PHYSICAL_ADDRESS              DmaDeviceAddress;  // Hardware address for DWC2_HCDMA
  VOID                              *DmaMapping;       // Opaque token for DmaUnmap
  UINTN                             DataLength;
  BOOLEAN                           IsDirectionIn;
  // ... existing fields ...
} DWUSB_ASYNC_REQUEST;

```

### 2. Map the Buffer in Submit Routine

When `UsbCdcNcmDxe` submits a network packet (TX) or an empty buffer for receiving (RX), map the memory for device access before configuring the hardware.

```c
EFI_STATUS
EFIAPI
DwUsb2HcAsyncBulkTransfer (
  // ... parameters ...
  )
{
  // ... allocate channel and Request node ...

  // 1. Determine DMA operation type based on USB endpoint direction
  // IN endpoints (Network RX) write to memory; OUT endpoints (Network TX) read from memory.
  DMA_MAP_OPERATION DmaOp = ((EndPointAddress & 0x80) != 0) ?
                              MapOperationBusMasterWrite :
                              MapOperationBusMasterRead;

  UINTN BytesToMap = *DataLength;

  // 2. Map the Virtual Address to a Bus Physical Address
  // This function automatically flushes/invalidates ARM caches and creates bounce
  // buffers if the SnpDxe payload is misaligned.
  Status = DmaMap (
             DmaOp,
             Data,
             &BytesToMap,
             &Request->DmaDeviceAddress,
             &Request->DmaMapping
             );

  if (EFI_ERROR (Status)) {
    FreePool (Request);
    return Status;
  }

  // 3. Program the DWC2 Hardware
  Request->Buffer = Data;

  // Write the hardware-safe DMA address to the DWC2 controller
  MmioWrite32 (HcDev->Dwc2BaseAddress + DWC2_HCDMA (ChannelIdx), (UINT32)Request->DmaDeviceAddress);

  // ... set HCCHAR, HCTSIZ, and enable CHEN ...

  InsertTailList (&HcDev->AsyncQueue, &Request->Link);
  return EFI_SUCCESS;
}

```

### 3. Unmap the Buffer in the Timer Callback

When the DWC2 controller signals `XFERCOMP` (or an error), you must unmap the buffer before executing the callback to `SnpDxe`. For RX packets, `DmaUnmap` guarantees the CPU cache is invalidated so the core reads the fresh network frame from RAM.

```c
VOID
EFIAPI
DwUsb2HcTimerCallback (
  // ... parameters ...
  )
{
  // ... traverse queue and read HcIntReg ...

  if (HcIntReg & (DWC2_HCINT_XFERCOMP | DWC2_HCINT_STALL | DWC2_HCINT_AHBERR)) {

    // Unmap the DMA buffer, restoring cache coherency
    if (Request->DmaMapping != NULL) {
      DmaUnmap (Request->DmaMapping);
      Request->DmaMapping = NULL;
    }

    if (HcIntReg & DWC2_HCINT_XFERCOMP) {
      Request->ResultStatus = EFI_USB_NOERROR;
    } else {
      Request->ResultStatus = EFI_USB_ERR_STALL; // Or hardware error
    }

    // Pass the packet up the chain to UsbCdcNcmDxe -> SnpDxe
    Request->Callback (Request->Buffer, Request->DataLength, Request->Context, Request->ResultStatus);

    // Free resources
    HcDev->AllocatedChannels &= ~(1 << Request->ChannelIndex);
    RemoveEntryList (&Request->Link);
    FreePool (Request);
  }
}

```

---

## Overcoming the 1ms Network Bottleneck

By default, UEFI timer events max out at a 1ms resolution (1,000 Hz). If `DwUsbHostDxe` only completes one `SnpDxe` bulk transfer per millisecond, your network throughput will be capped severely (often ~1-2 Mbps).

To make CDC-NCM perform adequately for an in-band BMC:

1. **Multiple Queueing:** Modify `DwUsbHostDxe` to allow multiple Bulk IN requests to be queued simultaneously on different hardware channels. `SnpDxe` can pre-allocate 4-5 RX buffers.
2. **Chained Callbacks:** Inside `DwUsb2HcTimerCallback`, if you complete an RX transfer, process all pending `HCINT` flags in a `while` loop before exiting the timer function, rather than waiting for the next 1ms tick.
3. **DWC2 Interrupt Polling (Advanced):** If throughput is still unacceptable, you can hook the actual hardware IRQ via EDK2's `gHardwareInterruptProtocol` (if exposed in your Pi platform code) to immediately execute the queue check, completely bypassing the 1ms UEFI timer limitation.
