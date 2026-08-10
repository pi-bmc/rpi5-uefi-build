/** @file

  Cadence GEM_GXL (r1p09) Gigabit Ethernet MAC register definitions, as
  integrated in the Raspberry Pi RP1 southbridge.

  Register map and descriptor layout adapted from FreeBSD
  sys/dev/cadence/if_cgem_hw.h (BSD-2-Clause), with the RP1-specific AXI
  Max Pipeline register (GEM_AMP) added per the RP1 integration.

  Copyright (c) 2012-2013 Thomas Skibo. All rights reserved.
  Copyright (c) 2025, the Rp1GemDxe contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef GEM_HW_H__
#define GEM_HW_H__

//
// Network Control Register
//
#define GEM_NET_CTRL                     0x000
#define  GEM_NET_CTRL_FLUSH_DPRAM_PKT    BIT18
#define  GEM_NET_CTRL_TX_HALT            BIT10
#define  GEM_NET_CTRL_START_TX           BIT9
#define  GEM_NET_CTRL_WREN_STAT_REGS     BIT7
#define  GEM_NET_CTRL_INCR_STAT_REGS     BIT6
#define  GEM_NET_CTRL_CLR_STAT_REGS      BIT5
#define  GEM_NET_CTRL_MGMT_PORT_EN       BIT4
#define  GEM_NET_CTRL_TX_EN              BIT3
#define  GEM_NET_CTRL_RX_EN              BIT2
#define  GEM_NET_CTRL_LOOP_LOCAL         BIT1

//
// Network Configuration Register
//
#define GEM_NET_CFG                      0x004
#define  GEM_NET_CFG_IGNORE_RX_FCS       BIT26
#define  GEM_NET_CFG_RX_CHKSUM_OFFLD_EN  BIT24
#define  GEM_NET_CFG_DBUS_WIDTH_32       (0U << 21)
#define  GEM_NET_CFG_DBUS_WIDTH_64       (1U << 21)
#define  GEM_NET_CFG_DBUS_WIDTH_128      (2U << 21)
#define  GEM_NET_CFG_DBUS_WIDTH_MASK     (3U << 21)
#define  GEM_NET_CFG_MDC_CLK_DIV_8       (0U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_16      (1U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_32      (2U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_48      (3U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_64      (4U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_96      (5U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_128     (6U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_224     (7U << 18)
#define  GEM_NET_CFG_MDC_CLK_DIV_MASK    (7U << 18)
#define  GEM_NET_CFG_FCS_REMOVE          BIT17
#define  GEM_NET_CFG_RX_BUF_OFFSET(n)    ((UINT32)(n) << 14)
#define  GEM_NET_CFG_PAUSE_EN            BIT13
#define  GEM_NET_CFG_GIGE_EN             BIT10
#define  GEM_NET_CFG_1536RXEN            BIT8
#define  GEM_NET_CFG_UNI_HASH_EN         BIT7
#define  GEM_NET_CFG_MULTI_HASH_EN       BIT6
#define  GEM_NET_CFG_NO_BCAST            BIT5
#define  GEM_NET_CFG_COPY_ALL            BIT4
#define  GEM_NET_CFG_FULL_DUPLEX         BIT1
#define  GEM_NET_CFG_SPEED100            BIT0

//
// Network Status Register
//
#define GEM_NET_STAT                     0x008
#define  GEM_NET_STAT_PHY_MGMT_IDLE      BIT2

//
// User I/O register: PHY interface select on SAMA-style GEM integrations.
// RP1's GEM uses the sama7g5 layout (u-boot rp1_gem_config ->
// sama7g5_usrio: mii=0, rmii=1, rgmii=2); the working u-boot port writes
// the RGMII value here before touching the PHY.
//
#define GEM_USER_IO                      0x00C
#define  GEM_USER_IO_RGMII               0x2

//
// DMA Configuration Register
//
#define GEM_DMA_CFG                      0x010
#define  GEM_DMA_CFG_ADDR_BUS_64         BIT30
#define  GEM_DMA_CFG_DISC_WHEN_NO_AHB    BIT24
#define  GEM_DMA_CFG_RX_BUF_SIZE(sz)     ((UINT32)(((sz) + 63) / 64) << 16)
#define  GEM_DMA_CFG_CHKSUM_GEN_EN       BIT11
#define  GEM_DMA_CFG_TX_PKTBUF_FULL      BIT10
#define  GEM_DMA_CFG_RX_PKTBUF_FULL      (3U << 8)
#define  GEM_DMA_CFG_AHB_BURST_LEN_16    (16U << 0)

//
// Transmit Status Register (write-1-to-clear)
//
#define GEM_TX_STAT                      0x014
#define  GEM_TX_STAT_HRESP_NOT_OK        BIT8
#define  GEM_TX_STAT_LATE_COLL           BIT7
#define  GEM_TX_STAT_UNDERRUN            BIT6
#define  GEM_TX_STAT_COMPLETE            BIT5
#define  GEM_TX_STAT_CORRUPT_AHB_ERR     BIT4
#define  GEM_TX_STAT_GO                  BIT3
#define  GEM_TX_STAT_RETRY_LIMIT_EXC     BIT2
#define  GEM_TX_STAT_COLLISION           BIT1
#define  GEM_TX_STAT_USED_BIT_READ       BIT0
#define  GEM_TX_STAT_ALL                 0x1FF

//
// Receive/Transmit Buffer Queue Base Address (queue 0)
//
#define GEM_RX_QBAR                      0x018
#define GEM_TX_QBAR                      0x01C

//
// Receive Status Register (write-1-to-clear)
//
#define GEM_RX_STAT                      0x020
#define  GEM_RX_STAT_HRESP_NOT_OK        BIT3
#define  GEM_RX_STAT_OVERRUN             BIT2
#define  GEM_RX_STAT_FRAME_RECD          BIT1
#define  GEM_RX_STAT_BUF_NOT_AVAIL       BIT0
#define  GEM_RX_STAT_ALL                 0xF

//
// Interrupt registers (polled driver: everything stays disabled)
//
#define GEM_INTR_STAT                    0x024
#define GEM_INTR_EN                      0x028
#define GEM_INTR_DIS                     0x02C
#define GEM_INTR_MASK                    0x030
#define  GEM_INTR_ALL                    0x7FFFEFF

//
// PHY Maintenance Register (Clause 22 MDIO)
//
#define GEM_PHY_MAINT                    0x034
#define  GEM_PHY_MAINT_CLAUSE_22         BIT30
#define  GEM_PHY_MAINT_OP_READ           (2U << 28)
#define  GEM_PHY_MAINT_OP_WRITE          (1U << 28)
#define  GEM_PHY_MAINT_PHY_ADDR_SHIFT    23
#define  GEM_PHY_MAINT_REG_ADDR_SHIFT    18
#define  GEM_PHY_MAINT_MUST_10           (2U << 16)
#define  GEM_PHY_MAINT_DATA_MASK         0xFFFF

//
// AXI Max Pipeline Register.
// RP1 integration requirement (see u-boot's RP1 macb port, gem_init_axi):
// AR2R_MAX_PIPE = 8, AW2W_MAX_PIPE = 8, AW2B_FILL = 1 for throughput on the
// PCIe-attached AXI fabric.
//
#define GEM_AMP                          0x054
#define  GEM_AMP_AR2R_MAX_PIPE_SHIFT     0
#define  GEM_AMP_AR2R_MAX_PIPE_MASK      (0xFFU << 0)
#define  GEM_AMP_AW2W_MAX_PIPE_SHIFT     8
#define  GEM_AMP_AW2W_MAX_PIPE_MASK      (0xFFU << 8)
#define  GEM_AMP_AW2B_FILL               BIT16

//
// Hash and specific address registers
//
#define GEM_HASH_BOT                     0x080
#define GEM_HASH_TOP                     0x084
#define GEM_SPEC_ADDR_LOW(n)             (0x088 + (n) * 8)
#define GEM_SPEC_ADDR_HI(n)              (0x08C + (n) * 8)

//
// Module ID Register
//
#define GEM_MODULE_ID                    0x0FC

//
// Design Configuration Registers
//
#define GEM_DESIGN_CFG1                  0x280
#define  GEM_DESIGN_CFG1_DBW_MASK        (7U << 25)
#define  GEM_DESIGN_CFG1_DBW_32          (1U << 25)
#define  GEM_DESIGN_CFG1_DBW_64          (2U << 25)
#define  GEM_DESIGN_CFG1_DBW_128         (4U << 25)

#define GEM_DESIGN_CFG6                  0x294
#define  GEM_DESIGN_CFG6_ADDR_64B        BIT23
#define  GEM_DESIGN_CFG6_PRIO_Q_MASK     0xFFFE

//
// Priority queue base address registers (queues 1..15) and the upper-32-bit
// base registers shared by all queues of a direction.
//
#define GEM_TX_QN_BAR(n)                 (0x440 + ((n) - 1) * 4)
#define GEM_RX_QN_BAR(n)                 (0x480 + ((n) - 1) * 4)
#define GEM_TX_QBAR_HI                   0x4C8
#define GEM_RX_QBAR_HI                   0x4D4

//
// DMA descriptor, 64-bit addressing layout (GEM_DMA_CFG_ADDR_BUS_64 set):
//   word0: buffer address [31:0] (RX: bit0 = OWN, bit1 = WRAP)
//   word1: control/status
//   word2: buffer address [63:32]
//   word3: unused
//
typedef struct {
  UINT32    Addr;
  UINT32    Ctrl;
  UINT32    AddrHi;
  UINT32    Unused;
} GEM_DMA_DESC;

//
// RX descriptor word0 bits
//
#define GEM_RXDESC_ADDR_OWN              BIT0
#define GEM_RXDESC_ADDR_WRAP             BIT1
#define GEM_RXDESC_ADDR_MASK             0xFFFFFFFC

//
// RX descriptor word1 (control/status) bits
//
#define GEM_RXDESC_CTRL_EOF              BIT15
#define GEM_RXDESC_CTRL_SOF              BIT14
#define GEM_RXDESC_CTRL_LENGTH_MASK      0x1FFF

//
// TX descriptor word1 (control/status) bits
//
#define GEM_TXDESC_CTRL_USED             BIT31
#define GEM_TXDESC_CTRL_WRAP             BIT30
#define GEM_TXDESC_CTRL_RETRY_ERR        BIT29
#define GEM_TXDESC_CTRL_AHB_ERR          BIT27
#define GEM_TXDESC_CTRL_LATE_COLL        BIT26
#define GEM_TXDESC_CTRL_NO_CRC           BIT16
#define GEM_TXDESC_CTRL_LAST_BUF         BIT15
#define GEM_TXDESC_CTRL_LENGTH_MASK      0x3FFF

#endif // GEM_HW_H__
