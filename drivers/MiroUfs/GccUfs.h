// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8750 GCC registers used by UFS, transcribed as facts from the public Linux
// gcc-sm8750.c clock driver (miro_kernel/linux/drivers/clk/qcom/gcc-sm8750.c).
// This is the board's own table: the SM8550 values of the ShengUfs component and
// their annotations are not carried here.
#ifndef NATIVE_UFS_GCC_UFS_H
#define NATIVE_UFS_GCC_UFS_H

// Physical windows from the stock miro device tree (clock-controller@100000, ufshc@1d84000).
#define GCC_BASE                      0x00100000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

// Power domains.
#define GCC_UFS_PHY_GDSCR             0x77004
#define GCC_UFS_MEM_PHY_GDSCR         0x9e000

// Branch halt/enable registers (clk_branch::halt_reg in gcc-sm8750.c).
// Every branch uses halt_bit 0 and the enable bit is bit 0; hwcg_bit is 1 for the
// branches that gate in hardware, so CLK_OFF on an idle one is normal.
#define GCC_UFS_PHY_AXI_ENABLE        0x77018
#define GCC_UFS_PHY_AHB_ENABLE        0x77028
#define GCC_UFS_PHY_TX_SYMBOL0_ENABLE 0x7702c
#define GCC_UFS_PHY_RX_SYMBOL0_ENABLE 0x77030
#define GCC_UFS_PHY_UNIPRO_CORE_ENABLE 0x7706c
#define GCC_UFS_PHY_ICE_CORE_ENABLE   0x7707c
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x770bc
#define GCC_UFS_PHY_RX_SYMBOL1_ENABLE 0x770d8
// bus_aggr_clk in the board DT: gcc_aggre_ufs_phy_axi_clk, hwcg_bit 1
#define GCC_AGGRE_UFS_PHY_AXI_ENABLE  0x770f0

// RCG2 command registers (clk_rcg2::cmd_rcgr in gcc-sm8750.c). MiroUfs does not
// program them yet; they are listed so a future rate change starts from the
// board's own values.
#define GCC_UFS_PHY_AXI_CLK_SRC_CMD       0x77034
#define GCC_UFS_PHY_ICE_CORE_CLK_SRC_CMD  0x7708c
#define GCC_UFS_PHY_UNIPRO_CLK_SRC_CMD    0x770a4
#define GCC_UFS_PHY_PHY_AUX_CLK_SRC_CMD   0x770c0

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U

// gcc_ufs_phy_ahb_clk and gcc_aggre_ufs_phy_axi_clk set hwcg_bit=1 in gcc-sm8750.c, so CLK_OFF=1 on
// an idle branch is normal.
// The reset and RPMh/TCSR/QMP-PHY programming are not represented here: their
// semantics are not qualified and MiroUfs must not write them speculatively.
#endif
