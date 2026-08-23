/** @file

  Simple Network Protocol driver for the Cadence GEM_GXL (r1p09) Gigabit
  Ethernet MAC inside the Raspberry Pi 5's RP1 southbridge.

  The controller is exposed by Rp1BusDxe as a vendor NON_DISCOVERABLE_DEVICE
  (Type == gRp1GemNonDiscoverableDeviceGuid) with two MMIO resources:
    [0] GEM core registers      (RP1 peripheral base + 0x100000, 0x4000)
    [1] eth_cfg wrapper         (RP1 peripheral base + 0x104000, 0x1000)

  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RP1_GEM_DXE_H__
#define RP1_GEM_DXE_H__

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DmaLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/SimpleNetwork.h>

#include "GemHw.h"

//
// Ethernet framing constants (kept local: the RPi5 platform DSC does not
// provide a NetLib mapping for standalone UEFI drivers).
//
#define GEM_ETHER_ADDR_LEN     6
#define GEM_ETHER_HEADER_SIZE  14
#define GEM_ETHER_MTU          1500
#define GEM_MAX_FRAME_SIZE     1536     // matches NET_CFG 1536RXEN limit

//
// Ring geometry: modest polled-mode rings, 2048-byte buffers.
//
#define GEM_RX_DESC_COUNT   16
#define GEM_TX_DESC_COUNT   8
#define GEM_RX_BUFFER_SIZE  2048
#define GEM_TX_BUFFER_SIZE  2048

#define RP1_GEM_SIGNATURE  SIGNATURE_32 ('R', 'G', 'e', 'm')

typedef struct {
  UINT32                         Signature;

  //
  // Handles and parent device
  //
  EFI_HANDLE                     ControllerHandle;   // NonDiscoverable handle
  EFI_HANDLE                     ChildHandle;        // handle carrying SNP
  EFI_HANDLE                     DriverBindingHandle;
  NON_DISCOVERABLE_DEVICE        *Dev;

  //
  // MMIO bases parsed from the NON_DISCOVERABLE_DEVICE resources
  //
  EFI_PHYSICAL_ADDRESS           GemBase;
  EFI_PHYSICAL_ADDRESS           EthCfgBase;

  //
  // UEFI protocol instances
  //
  EFI_SIMPLE_NETWORK_PROTOCOL    Snp;
  EFI_SIMPLE_NETWORK_MODE        SnpMode;
  EFI_DEVICE_PATH_PROTOCOL       *DevicePath;
  EFI_EVENT                      ExitBootServicesEvent;
  EFI_LOCK                       Lock;

  //
  // PHY state (BCM54213PE on the GEM internal MDIO, Clause 22)
  //
  UINT8                          PhyAddr;
  BOOLEAN                        LinkUp;

  //
  // DMA rings and buffers: uncached DmaAllocateBuffer memory, identity
  // mapped (CPU address == bus address) and below 3 GiB by platform policy
  // (PcdDmaDeviceOffset = 0, PcdDmaDeviceLimit = 0xBFFFFFFF).
  //
  VOID                           *DescBuffer;        // one page
  GEM_DMA_DESC                   *RxRing;            // GEM_RX_DESC_COUNT
  GEM_DMA_DESC                   *TxRing;            // GEM_TX_DESC_COUNT
  GEM_DMA_DESC                   *NullRxDesc;        // priority queue stub
  GEM_DMA_DESC                   *NullTxDesc;        // priority queue stub
  UINT8                          *RxBuffers;         // COUNT * SIZE
  UINT8                          *TxBuffers;         // COUNT * SIZE

  //
  // Ring cursors. TxProd/TxCons are free-running; descriptor index is
  // (value % GEM_TX_DESC_COUNT).
  //
  UINT16                         RxHead;
  UINT16                         TxProd;
  UINT16                         TxCons;

  //
  // Caller buffers pending recycle via GetStatus(), per TX descriptor.
  //
  VOID                           *TxUserBuffer[GEM_TX_DESC_COUNT];
} RP1_GEM_PRIVATE_DATA;

#define RP1_GEM_PRIVATE_DATA_FROM_SNP_THIS(a) \
  CR ((a), RP1_GEM_PRIVATE_DATA, Snp, RP1_GEM_SIGNATURE)

//
// GemHw.c
//
EFI_STATUS
GemDmaAlloc (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemDmaFree (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemMacReset (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemMacConfigure (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemMacEnableTxRx (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemMacDisableTxRx (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemSetMacAddress (
  IN RP1_GEM_PRIVATE_DATA   *Gem,
  IN CONST EFI_MAC_ADDRESS  *MacAddress
  );

VOID
GemSetReceiveFilters (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINT32                Filters
  );

VOID
GemUpdateLinkSpeed (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINTN                 SpeedMbps,
  IN BOOLEAN               FullDuplex
  );

EFI_STATUS
GemMdioRead (
  IN  RP1_GEM_PRIVATE_DATA  *Gem,
  IN  UINT8                 PhyAddr,
  IN  UINT8                 Reg,
  OUT UINT16                *Data
  );

EFI_STATUS
GemMdioWrite (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN UINT8                 PhyAddr,
  IN UINT8                 Reg,
  IN UINT16                Data
  );

BOOLEAN
GemRxPending (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

EFI_STATUS
GemReceiveFrame (
  IN     RP1_GEM_PRIVATE_DATA  *Gem,
  OUT    VOID                  *Buffer,
  IN OUT UINTN                 *BufferSize
  );

EFI_STATUS
GemTransmitFrame (
  IN RP1_GEM_PRIVATE_DATA  *Gem,
  IN VOID                  *UserBuffer,
  IN UINTN                 Length
  );

BOOLEAN
GemTxPendingCompletion (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

VOID
GemGetRecycledTxBuffer (
  IN  RP1_GEM_PRIVATE_DATA  *Gem,
  OUT VOID                  **TxBuf
  );

//
// Phy.c
//
EFI_STATUS
Rp1GemPhyInit (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

EFI_STATUS
Rp1GemPhyReset (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

EFI_STATUS
Rp1GemPhyUpdateConfig (
  IN RP1_GEM_PRIVATE_DATA  *Gem
  );

//
// Snp.c
//
extern CONST EFI_SIMPLE_NETWORK_PROTOCOL  gRp1GemSimpleNetworkTemplate;

VOID
EFIAPI
Rp1GemWaitForPacketNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  );

//
// DriverBinding.c / ComponentName.c
//
extern EFI_DRIVER_BINDING_PROTOCOL   gRp1GemDriverBinding;
extern EFI_COMPONENT_NAME_PROTOCOL   gRp1GemComponentName;
extern EFI_COMPONENT_NAME2_PROTOCOL  gRp1GemComponentName2;

#endif // RP1_GEM_DXE_H__
