/** @file

  RP1 DesignWare I2C master driver - private declarations.

  DesignWare DW_apb_i2c register layout and programming model adapted from
  Ampere's DwI2cLib (Silicon/Ampere/AmpereAltraPkg/Library/DwI2cLib);
  driver-binding shape after Socionext's SynQuacerI2cDxe.

  Copyright (c) 2020 - 2021, Ampere Computing LLC. All rights reserved.<BR>
  Copyright (c) 2017, Linaro, Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RP1_DW_I2C_DXE_H_
#define RP1_DW_I2C_DXE_H_

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/Rp1GpioLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/Rp1Bus.h>

//
// DesignWare DW_apb_i2c registers (offsets from the block base).
//
#define DW_IC_CON                   0x00
#define DW_IC_CON_MASTER            BIT0
#define DW_IC_CON_SPEED_STD         BIT1
#define DW_IC_CON_SPEED_FAST        BIT2
#define DW_IC_CON_RESTART_EN        BIT5
#define DW_IC_CON_SLAVE_DISABLE     BIT6
#define DW_IC_TAR                   0x04
#define DW_IC_DATA_CMD              0x10
#define DW_IC_DATA_CMD_DAT_MASK     0xFF
#define DW_IC_DATA_CMD_CMD          BIT8
#define DW_IC_DATA_CMD_STOP         BIT9
#define DW_IC_DATA_CMD_RESTART      BIT10
#define DW_IC_SS_SCL_HCNT           0x14
#define DW_IC_SS_SCL_LCNT           0x18
#define DW_IC_FS_SCL_HCNT           0x1C
#define DW_IC_FS_SCL_LCNT           0x20
#define DW_IC_INTR_MASK             0x30
#define DW_IC_RAW_INTR_STAT         0x34
#define DW_IC_INTR_TX_ABRT          BIT6
#define DW_IC_INTR_STOP_DET         BIT9
#define DW_IC_CLR_INTR              0x40
#define DW_IC_CLR_TX_ABRT           0x54
#define DW_IC_CLR_STOP_DET          0x60
#define DW_IC_ENABLE                0x6C
#define DW_IC_ENABLE_ENABLE         BIT0
#define DW_IC_STATUS                0x70
#define DW_IC_STATUS_TFNF           BIT1
#define DW_IC_STATUS_TFE            BIT2
#define DW_IC_STATUS_RFNE           BIT3
#define DW_IC_STATUS_MST_ACTIVITY   BIT5
#define DW_IC_TXFLR                 0x74
#define DW_IC_RXFLR                 0x78
#define DW_IC_SDA_HOLD              0x7C
#define DW_IC_TX_ABRT_SOURCE        0x80
#define DW_IC_ABRT_7B_ADDR_NOACK    BIT0
#define DW_IC_FS_SPKLEN             0xA0
#define DW_IC_ENABLE_STATUS         0x9C
#define DW_IC_COMP_PARAM_1          0xF4
#define DW_IC_COMP_TYPE             0xFC
#define DW_IC_COMP_TYPE_VALUE       0x44570140

#define DW_IC_COMP_PARAM_1_RX_BUFFER_DEPTH(x)  ((((x) >> 8) & 0xFF) + 1)
#define DW_IC_COMP_PARAM_1_TX_BUFFER_DEPTH(x)  ((((x) >> 16) & 0xFF) + 1)

//
// The RP1's DW_apb_i2c instances are clocked from clk_sys, which the RP1
// clock generator runs at a fixed 200 MHz (there is no clock driver in
// this firmware, and neither the VPU firmware nor Linux ever reparents
// it), so the input clock is hardcoded rather than discovered.
//
#define RP1_DW_I2C_INPUT_CLOCK_KHZ  200000

//
// SCL high/low counts, derived from the DW databook: the high phase must
// cover tHIGH plus the SCL fall time tf (counted by the controller before
// it considers SCL high), minus a fixed 3-cycle IC internal latency; the
// low phase covers tLOW + tf minus 1 cycle. I2C-spec minimum timings with
// tf = 300 ns; +500000 rounds the ns->cycles conversion to nearest.
//
//   Standard (100 kHz): tHIGH 4000 ns, tLOW 4700 ns -> HCNT 857, LCNT 999
//   Fast     (400 kHz): tHIGH  600 ns, tLOW 1300 ns -> HCNT 177, LCNT 319
//
#define DW_I2C_SCL_HCNT(HighNs)  \
  ((RP1_DW_I2C_INPUT_CLOCK_KHZ * ((HighNs) + 300) + 500000) / 1000000 - 3)
#define DW_I2C_SCL_LCNT(LowNs)   \
  ((RP1_DW_I2C_INPUT_CLOCK_KHZ * ((LowNs) + 300) + 500000) / 1000000 - 1)

#define DW_I2C_SS_SCL_HCNT_VALUE  DW_I2C_SCL_HCNT (4000)
#define DW_I2C_SS_SCL_LCNT_VALUE  DW_I2C_SCL_LCNT (4700)
#define DW_I2C_FS_SCL_HCNT_VALUE  DW_I2C_SCL_HCNT (600)
#define DW_I2C_FS_SCL_LCNT_VALUE  DW_I2C_SCL_LCNT (1300)

//
// Spike suppression: 50 ns at 200 MHz. SDA hold: 300 ns at 200 MHz.
//
#define DW_I2C_FS_SPKLEN_VALUE    (RP1_DW_I2C_INPUT_CLOCK_KHZ * 50 / 1000000)
#define DW_I2C_SDA_HOLD_VALUE     (RP1_DW_I2C_INPUT_CLOCK_KHZ * 300 / 1000000)

//
// Polled-transfer timeouts: ~50 ms guard per byte, polled in 10 us steps
// via gBS->Stall; controller enable/disable handshake polled in 25 us
// steps for up to 2.5 ms.
//
#define DW_I2C_POLL_INTERVAL_US     10
#define DW_I2C_BYTE_TIMEOUT_US      50000
#define DW_I2C_ENABLE_INTERVAL_US   25
#define DW_I2C_ENABLE_TIMEOUT_US    2500

//
// The RP1 muxes I2C1 SDA/SCL onto GPIO2/GPIO3 as alt3.
//
#define RP1_I2C1_GPIO_SDA  2
#define RP1_I2C1_GPIO_SCL  3

#define RP1_DW_I2C_SIGNATURE  SIGNATURE_32 ('R', '1', 'I', 'C')

typedef struct {
  UINT32                     Signature;
  EFI_HANDLE                 ControllerHandle;
  NON_DISCOVERABLE_DEVICE    *Dev;
  EFI_PHYSICAL_ADDRESS       MmioBase;
  UINT32                     TxFifoDepth;
  UINT32                     RxFifoDepth;
  UINTN                      BusClockHertz;
  EFI_I2C_MASTER_PROTOCOL    I2cMaster;
} RP1_DW_I2C_MASTER;

#define RP1_DW_I2C_FROM_THIS(a)  \
  CR (a, RP1_DW_I2C_MASTER, I2cMaster, RP1_DW_I2C_SIGNATURE)

extern EFI_COMPONENT_NAME_PROTOCOL   gRp1DwI2cComponentName;
extern EFI_COMPONENT_NAME2_PROTOCOL  gRp1DwI2cComponentName2;

#endif // RP1_DW_I2C_DXE_H_
