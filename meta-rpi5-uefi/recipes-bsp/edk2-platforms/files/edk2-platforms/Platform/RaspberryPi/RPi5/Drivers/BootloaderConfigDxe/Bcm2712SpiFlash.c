/** @file

  Raspberry Pi 5 boot EEPROM reader: BCM2835-compatible SPI controller
  at soc "spi10" (CPU 0x10_7d004000), polled FIFO mode, chip-select
  bit-banged on soc GIO GPIO1 ("2712_BOOT_CS_N") exactly like Linux's
  spi10 cs-gpios binding.

  Hardware facts (from the firmware DTB and the BCM2835 periperhal
  datasheet / Linux pinctrl-bcm2712.c function tables):
  - spi10 clock is the fixed 750 MHz VPU clock; CDIV 64 gives ~11.7 MHz,
    well under the flash's 50 MHz plain-READ limit.
  - MISO/MOSI/SCLK are soc GPIO2/3/4 muxed to "vc_spi0"; the fsel encoding
    differs between the C0 and D0 steppings, so the values are table-driven
    off the pinctrl compatible in the live (firmware-fixed-up) DTB.
  - The CS pin muxes to plain GPIO (fsel 0) and is driven through the
    brcmstb GIO block PowerButtonDxe already uses.

  The VPU has finished with the flash once the armstub is running - it
  only touches it again for explicit mailbox-driven updates - so a
  one-shot polled read triggered from Setup cannot race it.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "Bcm2712SpiFlash.h"
#include "BootloaderConfig.h"

//
// BCM2835 SPI controller registers (BCM2835 ARM Peripherals datasheet).
//
#define SPI_REG_CS        0x00
#define SPI_REG_FIFO      0x04
#define SPI_REG_CLK       0x08

#define SPI_CS_CLEAR_TX   BIT4
#define SPI_CS_CLEAR_RX   BIT5
#define SPI_CS_TA         BIT7
#define SPI_CS_DONE       BIT16
#define SPI_CS_RXD        BIT17
#define SPI_CS_TXD        BIT18

//
// 750 MHz VPU clock / 64 = ~11.7 MHz SCLK.
//
#define SPI_CLK_DIVIDER   64

//
// brcmstb GIO per-bank registers (see PowerButtonDxe).
//
#define GIO_BANK_SIZE     0x20
#define GIO_REG_DATA      0x04
#define GIO_REG_IODIR     0x08   // 1 = input

#define GPIO_ACTIVE_LOW_FLAG  BIT0

//
// Iterations of the FIFO poll loop without progress before declaring the
// controller dead (each iteration is at least one MMIO read, so this is
// on the order of a second).
//
#define SPI_STALL_LIMIT   10000000

#define FLASH_CMD_RDID    0x9F
#define FLASH_CMD_READ    0x03

//
// Fixed BCM2712 addresses for the boot SPI path, used only when the live
// device tree does not describe it. Both are SoC constants, not board
// wiring: spi10 and the main (non-AON) GIO block sit at the same place on
// every BCM2712, and the soc "ranges" that translate them
// (<0x0 0x10 0x0 0x80000000>) are equally fixed. The chip select is
// board wiring, but the 2712_BOOT_CS_N line is GPIO1 active-low on every
// Pi 5 / CM5 variant Raspberry Pi has shipped.
//
// This matters because the tree at device_tree_address is whichever
// bcm2712-rpi-5-b.dtb sits at the root of the boot partition, and this
// image deliberately ships the one built from the Talos kernel rather than
// the Raspberry Pi firmware release (see recipes-bsp/images/rpi5-uefi-sdimg.bb).
// Mainline's bcm2712.dtsi has no boot-SPI node at all -- the downstream
// Raspberry Pi DTS is the only one that describes spi10 -- so without this
// fallback the EEPROM update path fails on a card that is otherwise
// working. Of every node this firmware looks up, spi10 is the only one
// mainline omits; pinctrl, the GIO blocks, blconfig and gpio-keys are all
// present in both trees, so the DTB stays authoritative everywhere else
// and for all of these fields whenever it does describe them.
//
#define BCM2712_BOOT_SPI_BASE     0x107d004000ULL
#define BCM2712_BOOT_CS_GIO_BASE  0x107d508500ULL
#define BCM2712_BOOT_CS_PIN       1

//
// vc_spi0 fsel values for soc GPIO1 (CS, kept at fsel 0 = gpio),
// GPIO2 (MISO), GPIO3 (MOSI), GPIO4 (SCLK). All four mux fields live in
// the first pinctrl register on both steppings; 4-bit fields, one nibble
// per pin, but at stepping-specific positions (Linux pinctrl-bcm2712.c
// bcm2712_{c0,d0}_gpio_pin_{regs,funcs}).
//
typedef struct {
  UINT8    Shift;   // bit position of the pin's fsel nibble in register 0
  UINT8    Fsel;    // function value to program
} SPI_MUX_FIELD;

STATIC CONST SPI_MUX_FIELD  mMuxC0[4] = {
  { 4, 0 }, { 8, 6 }, { 12, 5 }, { 16, 6 }
};

STATIC CONST SPI_MUX_FIELD  mMuxD0[4] = {
  { 0, 0 }, { 4, 7 }, { 8, 6 }, { 12, 6 }
};

EFI_STATUS
Bcm2712BootSpiLocate (
  IN  CONST VOID        *Fdt,
  OUT BCM2712_BOOT_SPI  *Spi
  )
{
  INT32         Node;
  INT32         GpioNode;
  INT32         Length;
  CONST UINT32  *Prop;
  UINT64        Address;
  UINT64        Size;
  UINT32        Phandle;
  UINT32        Pin;
  UINT32        Flags;

  Spi->PinctrlBase = 0;
  Spi->IsD0        = FALSE;

  //
  // The soc SPI controller in front of the boot flash is the only
  // "brcm,bcm2835-spi" node in the VPU DTB (the RP1's SPIs are PL022s).
  //
  Spi->SpiBase    = BCM2712_BOOT_SPI_BASE;
  Spi->CsBankBase = BCM2712_BOOT_CS_GIO_BASE + (BCM2712_BOOT_CS_PIN / 32) * GIO_BANK_SIZE;
  Spi->CsMask     = 1U << (BCM2712_BOOT_CS_PIN % 32);
  Spi->CsPin      = BCM2712_BOOT_CS_PIN;
  Spi->CsActiveLow = TRUE;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2835-spi");
  if (Node < 0) {
    //
    // Mainline-derived tree: no boot-SPI node to read. Keep the constants
    // above rather than failing -- see their comment.
    //
    DEBUG ((
      DEBUG_WARN,
      "BootloaderConfig: no bcm2835-spi node, using fixed BCM2712 addresses\n"
      ));
  } else if (!BlGetTranslatedRegAddress (Fdt, Node, &Address, &Size)) {
    DEBUG ((DEBUG_WARN, "BootloaderConfig: spi10 reg untranslatable\n"));
  } else {
    Spi->SpiBase = Address;

    //
    // Only override the chip select when the node actually describes one;
    // a node without cs-gpios leaves the board wiring above in place.
    //
    Prop = FdtGetProp (Fdt, Node, "cs-gpios", &Length);
    if ((Prop == NULL) || (Length < 3 * (INT32)sizeof (UINT32))) {
      DEBUG ((DEBUG_WARN, "BootloaderConfig: spi10 has no cs-gpios\n"));
    } else {
      Phandle = Fdt32ToCpu (Prop[0]);
      Pin     = Fdt32ToCpu (Prop[1]);
      Flags   = Fdt32ToCpu (Prop[2]);

      GpioNode = FdtNodeOffsetByPhandle (Fdt, Phandle);
      if ((GpioNode >= 0) && BlGetTranslatedRegAddress (Fdt, GpioNode, &Address, &Size)) {
        Spi->CsBankBase  = Address + (Pin / 32) * GIO_BANK_SIZE;
        Spi->CsMask      = 1U << (Pin % 32);
        Spi->CsPin       = Pin;
        Spi->CsActiveLow = (BOOLEAN)((Flags & GPIO_ACTIVE_LOW_FLAG) != 0);
      } else {
        DEBUG ((DEBUG_WARN, "BootloaderConfig: spi10 cs-gpios unresolvable\n"));
      }
    }
  }

  //
  // Main soc pinctrl block (not the AON one). The firmware fixes the
  // compatible up to the running stepping, so probe D0 first, then C0,
  // then the generic name. Without a match the mux is left as the VPU
  // set it and the JEDEC probe decides whether that was good enough.
  //
  Node = FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712d0-pinctrl");
  if (Node >= 0) {
    Spi->IsD0 = TRUE;
  } else {
    Node = FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712c0-pinctrl");
    if (Node < 0) {
      Node = FdtNodeOffsetByCompatible (Fdt, -1, "brcm,bcm2712-pinctrl");
    }
  }

  if ((Node >= 0) && BlGetTranslatedRegAddress (Fdt, Node, &Address, &Size)) {
    Spi->PinctrlBase = Address;
  }

  DEBUG ((
    DEBUG_INFO,
    "BootloaderConfig: spi10 @ 0x%Lx cs bank 0x%Lx mask 0x%x pinctrl 0x%Lx %a\n",
    Spi->SpiBase,
    Spi->CsBankBase,
    Spi->CsMask,
    Spi->PinctrlBase,
    Spi->IsD0 ? "(D0)" : "(C0)"
    ));
  return EFI_SUCCESS;
}

/**
  Route soc GPIO1-4 to the boot SPI: data/clock pins to vc_spi0, the CS
  pin to gpio. Returns the previous mux register value for restore.
**/
STATIC
UINT32
SpiMuxPins (
  IN CONST BCM2712_BOOT_SPI  *Spi
  )
{
  CONST SPI_MUX_FIELD  *Mux;
  UINT32               Value;
  UINT32               Original;
  UINTN                Index;

  Mux      = Spi->IsD0 ? mMuxD0 : mMuxC0;
  Original = MmioRead32 (Spi->PinctrlBase);
  Value    = Original;

  for (Index = 0; Index < 4; Index++) {
    //
    // Index 0 is the CS pin's field: only touch it when the CS really is
    // the expected pin 1 (board variants could move it elsewhere).
    //
    if ((Index == 0) && (Spi->CsPin != 1)) {
      continue;
    }

    Value &= ~(0xFU << Mux[Index].Shift);
    Value |= (UINT32)Mux[Index].Fsel << Mux[Index].Shift;
  }

  if (Value != Original) {
    MmioWrite32 (Spi->PinctrlBase, Value);
  }

  return Original;
}

STATIC
VOID
SpiCsSet (
  IN CONST BCM2712_BOOT_SPI  *Spi,
  IN BOOLEAN                 Assert
  )
{
  BOOLEAN  High;

  High = Assert ? !Spi->CsActiveLow : Spi->CsActiveLow;
  if (High) {
    MmioOr32 (Spi->CsBankBase + GIO_REG_DATA, Spi->CsMask);
  } else {
    MmioAnd32 (Spi->CsBankBase + GIO_REG_DATA, ~Spi->CsMask);
  }
}

/**
  Full-duplex polled transfer: clock out TxLen command bytes then RxLen
  dummy bytes, capturing the RxLen response bytes. The caller owns the
  chip select.
**/
STATIC
EFI_STATUS
SpiXfer (
  IN  CONST BCM2712_BOOT_SPI  *Spi,
  IN  CONST UINT8             *Tx,
  IN  UINTN                   TxLen,
  OUT UINT8                   *Rx OPTIONAL,
  IN  UINTN                   RxLen
  )
{
  UINTN   Total;
  UINTN   Sent;
  UINTN   Received;
  UINT32  Status;
  UINT8   Byte;
  UINTN   Stall;

  Total    = TxLen + RxLen;
  Sent     = 0;
  Received = 0;
  Stall    = 0;

  MmioWrite32 (
    Spi->SpiBase + SPI_REG_CS,
    SPI_CS_TA | SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX
    );

  while (Received < Total) {
    Status = MmioRead32 (Spi->SpiBase + SPI_REG_CS);

    if (((Status & SPI_CS_TXD) != 0) && (Sent < Total)) {
      MmioWrite32 (
        Spi->SpiBase + SPI_REG_FIFO,
        (Sent < TxLen) ? Tx[Sent] : 0
        );
      Sent++;
    }

    if ((Status & SPI_CS_RXD) != 0) {
      Byte = (UINT8)MmioRead32 (Spi->SpiBase + SPI_REG_FIFO);
      if ((Received >= TxLen) && (Rx != NULL)) {
        Rx[Received - TxLen] = Byte;
      }

      Received++;
      Stall = 0;
    } else if (++Stall > SPI_STALL_LIMIT) {
      MmioWrite32 (Spi->SpiBase + SPI_REG_CS, SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);
      return EFI_TIMEOUT;
    }
  }

  Stall = 0;
  while ((MmioRead32 (Spi->SpiBase + SPI_REG_CS) & SPI_CS_DONE) == 0) {
    if (++Stall > SPI_STALL_LIMIT) {
      break;
    }
  }

  MmioWrite32 (Spi->SpiBase + SPI_REG_CS, SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);
  return EFI_SUCCESS;
}

/**
  One JEDEC RDID exchange. Returns the three ID bytes; an all-zero or
  all-ones answer means nothing is driving MISO, which is failure, not data.
**/
STATIC
EFI_STATUS
SpiProbeJedec (
  IN  CONST BCM2712_BOOT_SPI  *Spi,
  OUT UINT8                   *Id
  )
{
  EFI_STATUS  Status;
  UINT8       Cmd;
  UINTN       Try;

  for (Try = 0; Try < 2; Try++) {
    Id[0] = 0;
    Id[1] = 0;
    Id[2] = 0;
    Cmd   = FLASH_CMD_RDID;

    SpiCsSet (Spi, TRUE);
    Status = SpiXfer (Spi, &Cmd, 1, Id, 3);
    SpiCsSet (Spi, FALSE);

    if (!EFI_ERROR (Status) &&
        !((Id[0] == 0x00) && (Id[1] == 0x00) && (Id[2] == 0x00)) &&
        !((Id[0] == 0xFF) && (Id[1] == 0xFF) && (Id[2] == 0xFF)))
    {
      return EFI_SUCCESS;
    }

    gBS->Stall (10000);
  }

  return EFI_DEVICE_ERROR;
}

/**
  Bring the controller to a clean mode-0 idle with CS deasserted and the
  CS pin an output.
**/
STATIC
VOID
SpiIdle (
  IN CONST BCM2712_BOOT_SPI  *Spi
  )
{
  SpiCsSet (Spi, FALSE);
  MmioAnd32 (Spi->CsBankBase + GIO_REG_IODIR, ~Spi->CsMask);
  MmioWrite32 (Spi->SpiBase + SPI_REG_CS, SPI_CS_CLEAR_RX | SPI_CS_CLEAR_TX);
  MmioWrite32 (Spi->SpiBase + SPI_REG_CLK, SPI_CLK_DIVIDER);
}

EFI_STATUS
Bcm2712BootSpiReadImage (
  IN  CONST BCM2712_BOOT_SPI    *Spi,
  OUT UINT8                     *Buffer,
  IN  UINTN                     Len,
  OUT BCM2712_BOOT_SPI_RESULT   *Result OPTIONAL
  )
{
  EFI_STATUS        Status;
  BCM2712_BOOT_SPI  Work;
  UINT32            SavedMux;
  BOOLEAN           Muxed;
  UINT8             Cmd[4];
  UINT8             Id[3];
  UINTN             Attempt;

  Status   = EFI_DEVICE_ERROR;
  SavedMux = 0;
  Muxed    = FALSE;
  Id[0]    = 0;
  Id[1]    = 0;
  Id[2]    = 0;
  Work     = *Spi;

  //
  // The C0 and D0 steppings put the pin-mux nibbles in different places, and
  // the only thing that tells the two apart is the pinctrl "compatible" in
  // the live device tree. That is a claim about the tree, not about the
  // silicon: it is right when the VPU fixed the tree up for this board, and
  // wrong when the tree came from somewhere that never knew about steppings.
  // Rather than trust it, try the encoding it named and then the other one.
  // A wrong encoding costs one failed JEDEC probe -- it muxes four soc pins
  // that belong to the boot SPI and nothing else, and the original register
  // value goes back before the next attempt.
  //
  for (Attempt = 0; Attempt < 2; Attempt++) {
    Work.IsD0 = (Attempt == 0) ? Spi->IsD0 : (BOOLEAN)(!Spi->IsD0);

    Muxed = FALSE;
    if (Work.PinctrlBase != 0) {
      SavedMux = SpiMuxPins (&Work);
      Muxed    = TRUE;
    }

    SpiIdle (&Work);

    Status = SpiProbeJedec (&Work, Id);
    if (!EFI_ERROR (Status)) {
      break;
    }

    if (Muxed) {
      MmioWrite32 (Work.PinctrlBase, SavedMux);
      Muxed = FALSE;
    } else {
      //
      // Nothing to re-mux, so the second attempt would repeat the first.
      //
      break;
    }
  }

  if (Result != NULL) {
    Result->JedecId[0] = Id[0];
    Result->JedecId[1] = Id[1];
    Result->JedecId[2] = Id[2];
    Result->UsedD0     = Work.IsD0;
    Result->Muxed      = Muxed;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "BootloaderConfig: EEPROM not responding (JEDEC %02x %02x %02x)\n",
      Id[0],
      Id[1],
      Id[2]
      ));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_INFO,
    "BootloaderConfig: EEPROM JEDEC ID %02x %02x %02x (%a mux)\n",
    Id[0],
    Id[1],
    Id[2],
    Work.IsD0 ? "D0" : "C0"
    ));

  Cmd[0] = FLASH_CMD_READ;
  Cmd[1] = 0;
  Cmd[2] = 0;
  Cmd[3] = 0;
  SpiCsSet (&Work, TRUE);
  Status = SpiXfer (&Work, Cmd, sizeof (Cmd), Buffer, Len);
  SpiCsSet (&Work, FALSE);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BootloaderConfig: EEPROM read failed: %r\n", Status));
  }

  //
  // Hand the pins back exactly as they were found, on success as well as
  // failure: the encoding above may be the one the device tree did not name.
  //
  if (Muxed) {
    MmioWrite32 (Work.PinctrlBase, SavedMux);
  }

  return Status;
}
