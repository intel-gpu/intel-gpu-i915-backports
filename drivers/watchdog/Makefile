# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the WatchDog device drivers.
#

# The WatchDog Timer Driver Core.
# obj-$(CPTCFG_WATCHDOG_CORE)	+= watchdog.o

# watchdog-objs	+= watchdog_core.o watchdog_dev.o

# watchdog-$(CPTCFG_WATCHDOG_PRETIMEOUT_GOV)	+= watchdog_pretimeout.o
# watchdog-$(CPTCFG_WATCHDOG_HRTIMER_PRETIMEOUT)	+= watchdog_hrtimer_pretimeout.o

# obj-$(CPTCFG_WATCHDOG_PRETIMEOUT_GOV_NOOP)	+= pretimeout_noop.o
# obj-$(CPTCFG_WATCHDOG_PRETIMEOUT_GOV_PANIC)	+= pretimeout_panic.o

# Only one watchdog can succeed. We probe the ISA/PCI/USB based
# watchdog-cards first, then the architecture specific watchdog
# drivers and then the architecture independent "softdog" driver.
# This means that if your ISA/PCI/USB card isn't detected that
# you can fall back to an architecture specific driver and if
# that also fails then you can fall back to the software watchdog
# to give you some cover.

# ISA-based Watchdog Cards
#obj-$(CPTCFG_PCWATCHDOG += pcwd.o
#obj-$(CPTCFG_MIXCOMWD += mixcomwd.o
#obj-$(CPTCFG_WDT += wdt.o

# PCI-based Watchdog Cards
#obj-$(CPTCFG_PCIPCWATCHDOG += pcwd_pci.o
#obj-$(CPTCFG_WDTPCI += wdt_pci.o

# USB-based Watchdog Cards
#obj-$(CPTCFG_USBPCWATCHDOG += pcwd_usb.o

# ALPHA Architecture

# ARM Architecture
#obj-$(CPTCFG_ARM_SP805_WATCHDOG += sp805_wdt.o
#obj-$(CPTCFG_ARM_SBSA_WATCHDOG += sbsa_gwdt.o
#obj-$(CPTCFG_ARMADA_37XX_WATCHDOG += armada_37xx_wdt.o
#obj-$(CPTCFG_ASM9260_WATCHDOG += asm9260_wdt.o
#obj-$(CPTCFG_AT91RM9200_WATCHDOG += at91rm9200_wdt.o
#obj-$(CPTCFG_AT91SAM9X_WATCHDOG += at91sam9_wdt.o
#obj-$(CPTCFG_CADENCE_WATCHDOG += cadence_wdt.o
#obj-$(CPTCFG_OMAP_WATCHDOG += omap_wdt.o
#obj-$(CPTCFG_TWL4030_WATCHDOG += twl4030_wdt.o
#obj-$(CPTCFG_21285_WATCHDOG += wdt285.o
#obj-$(CPTCFG_977_WATCHDOG += wdt977.o
#obj-$(CPTCFG_FTWDT010_WATCHDOG += ftwdt010_wdt.o
#obj-$(CPTCFG_IXP4XX_WATCHDOG += ixp4xx_wdt.o
#obj-$(CPTCFG_S3C2410_WATCHDOG += s3c2410_wdt.o
#obj-$(CPTCFG_SA1100_WATCHDOG += sa1100_wdt.o
#obj-$(CPTCFG_SAMA5D4_WATCHDOG += sama5d4_wdt.o
#obj-$(CPTCFG_DW_WATCHDOG += dw_wdt.o
#obj-$(CPTCFG_EP93XX_WATCHDOG += ep93xx_wdt.o
#obj-$(CPTCFG_PNX4008_WATCHDOG += pnx4008_wdt.o
#obj-$(CPTCFG_IOP_WATCHDOG += iop_wdt.o
#obj-$(CPTCFG_DAVINCI_WATCHDOG += davinci_wdt.o
#obj-$(CPTCFG_K3_RTI_WATCHDOG += rti_wdt.o
#obj-$(CPTCFG_ORION_WATCHDOG += orion_wdt.o
#obj-$(CPTCFG_SUNXI_WATCHDOG += sunxi_wdt.o
#obj-$(CPTCFG_RN5T618_WATCHDOG += rn5t618_wdt.o
#obj-$(CPTCFG_NPCM7XX_WATCHDOG += npcm_wdt.o
#obj-$(CPTCFG_STMP3XXX_RTC_WATCHDOG += stmp3xxx_rtc_wdt.o
#obj-$(CPTCFG_TS4800_WATCHDOG += ts4800_wdt.o
#obj-$(CPTCFG_TS72XX_WATCHDOG += ts72xx_wdt.o
#obj-$(CPTCFG_IMX2_WDT += imx2_wdt.o
#obj-$(CPTCFG_IMX_SC_WDT += imx_sc_wdt.o
#obj-$(CPTCFG_IMX7ULP_WDT += imx7ulp_wdt.o
#obj-$(CPTCFG_UX500_WATCHDOG += ux500_wdt.o
#obj-$(CPTCFG_RETU_WATCHDOG += retu_wdt.o
#obj-$(CPTCFG_BCM2835_WDT += bcm2835_wdt.o
#obj-$(CPTCFG_MOXART_WDT += moxart_wdt.o
#obj-$(CPTCFG_ST_LPC_WATCHDOG += st_lpc_wdt.o
#obj-$(CPTCFG_QCOM_WDT += qcom-wdt.o
#obj-$(CPTCFG_BCM_KONA_WDT += bcm_kona_wdt.o
#obj-$(CPTCFG_TEGRA_WATCHDOG += tegra_wdt.o
#obj-$(CPTCFG_MESON_GXBB_WATCHDOG += meson_gxbb_wdt.o
#obj-$(CPTCFG_MESON_WATCHDOG += meson_wdt.o
#obj-$(CPTCFG_MEDIATEK_WATCHDOG += mtk_wdt.o
#obj-$(CPTCFG_DIGICOLOR_WATCHDOG += digicolor_wdt.o
#obj-$(CPTCFG_LPC18XX_WATCHDOG += lpc18xx_wdt.o
#obj-$(CPTCFG_BCM7038_WDT += bcm7038_wdt.o
#obj-$(CPTCFG_RENESAS_WDT += renesas_wdt.o
#obj-$(CPTCFG_RENESAS_RZAWDT += rza_wdt.o
#obj-$(CPTCFG_ASPEED_WATCHDOG += aspeed_wdt.o
#obj-$(CPTCFG_STM32_WATCHDOG += stm32_iwdg.o
#obj-$(CPTCFG_UNIPHIER_WATCHDOG += uniphier_wdt.o
#obj-$(CPTCFG_RTD119X_WATCHDOG += rtd119x_wdt.o
#obj-$(CPTCFG_SPRD_WATCHDOG += sprd_wdt.o
#obj-$(CPTCFG_PM8916_WATCHDOG += pm8916_wdt.o
#obj-$(CPTCFG_ARM_SMC_WATCHDOG += arm_smc_wdt.o
#obj-$(CPTCFG_VISCONTI_WATCHDOG += visconti_wdt.o
#obj-$(CPTCFG_MSC313E_WATCHDOG += msc313e_wdt.o

# X86 (i386 + ia64 + x86_64) Architecture
#obj-$(CPTCFG_ACQUIRE_WDT += acquirewdt.o
#obj-$(CPTCFG_ADVANTECH_WDT += advantechwdt.o
#obj-$(CPTCFG_ALIM1535_WDT += alim1535_wdt.o
#obj-$(CPTCFG_ALIM7101_WDT += alim7101_wdt.o
#obj-$(CPTCFG_EBC_C384_WDT += ebc-c384_wdt.o
#obj-$(CPTCFG_F71808E_WDT += f71808e_wdt.o
#obj-$(CPTCFG_SP5100_TCO += sp5100_tco.o
#obj-$(CPTCFG_GEODE_WDT += geodewdt.o
#obj-$(CPTCFG_SC520_WDT += sc520_wdt.o
#obj-$(CPTCFG_SBC_FITPC2_WATCHDOG += sbc_fitpc2_wdt.o
#obj-$(CPTCFG_EUROTECH_WDT += eurotechwdt.o
#obj-$(CPTCFG_IB700_WDT += ib700wdt.o
#obj-$(CPTCFG_IBMASR += ibmasr.o
#obj-$(CPTCFG_WAFER_WDT += wafer5823wdt.o
#obj-$(CPTCFG_I6300ESB_WDT += i6300esb.o
#obj-$(CPTCFG_IE6XX_WDT += ie6xx_wdt.o
#obj-$(CPTCFG_ITCO_WDT += iTCO_wdt.o
ifeq ($(CPTCFG_ITCO_VENDOR_SUPPORT),y)
#obj-$(CPTCFG_ITCO_WDT += iTCO_vendor_support.o
endif
#obj-$(CPTCFG_IT8712F_WDT += it8712f_wdt.o
#obj-$(CPTCFG_IT87_WDT += it87_wdt.o
#obj-$(CPTCFG_HP_WATCHDOG += hpwdt.o
#obj-$(CPTCFG_KEMPLD_WDT += kempld_wdt.o
#obj-$(CPTCFG_SC1200_WDT += sc1200wdt.o
#obj-$(CPTCFG_SCx200_WDT += scx200_wdt.o
#obj-$(CPTCFG_PC87413_WDT += pc87413_wdt.o
#obj-$(CPTCFG_NV_TCO += nv_tco.o
#obj-$(CPTCFG_RDC321X_WDT += rdc321x_wdt.o
#obj-$(CPTCFG_60XX_WDT += sbc60xxwdt.o
#obj-$(CPTCFG_SBC8360_WDT += sbc8360.o
#obj-$(CPTCFG_SBC7240_WDT += sbc7240_wdt.o
#obj-$(CPTCFG_CPU5_WDT += cpu5wdt.o
#obj-$(CPTCFG_SMSC_SCH311X_WDT += sch311x_wdt.o
#obj-$(CPTCFG_SMSC37B787_WDT += smsc37b787_wdt.o
#obj-$(CPTCFG_TQMX86_WDT += tqmx86_wdt.o
#obj-$(CPTCFG_VIA_WDT += via_wdt.o
#obj-$(CPTCFG_W83627HF_WDT += w83627hf_wdt.o
#obj-$(CPTCFG_W83877F_WDT += w83877f_wdt.o
#obj-$(CPTCFG_W83977F_WDT += w83977f_wdt.o
#obj-$(CPTCFG_MACHZ_WDT += machzwd.o
#obj-$(CPTCFG_SBC_EPX_C3_WATCHDOG += sbc_epx_c3.o
#obj-$(CPTCFG_INTEL_MID_WATCHDOG += intel-mid_wdt.o
obj-$(CPTCFG_INTEL_MEI_WDT) += mei_wdt.o
#obj-$(CPTCFG_NI903X_WDT += ni903x_wdt.o
#obj-$(CPTCFG_NIC7018_WDT += nic7018_wdt.o
#obj-$(CPTCFG_MLX_WDT += mlx_wdt.o
#obj-$(CPTCFG_KEEMBAY_WATCHDOG += keembay_wdt.o

# M68K Architecture
#obj-$(CPTCFG_M54xx_WATCHDOG += m54xx_wdt.o

# MicroBlaze Architecture
#obj-$(CPTCFG_XILINX_WATCHDOG += of_xilinx_wdt.o

# MIPS Architecture
#obj-$(CPTCFG_ATH79_WDT += ath79_wdt.o
#obj-$(CPTCFG_BCM47XX_WDT += bcm47xx_wdt.o
#obj-$(CPTCFG_BCM63XX_WDT += bcm63xx_wdt.o
#obj-$(CPTCFG_RC32434_WDT += rc32434_wdt.o
#obj-$(CPTCFG_INDYDOG += indydog.o
#obj-$(CPTCFG_JZ4740_WDT += jz4740_wdt.o
#obj-$(CPTCFG_WDT_MTX1 += mtx-1_wdt.o
#obj-$(CPTCFG_SIBYTE_WDOG += sb_wdog.o
#obj-$(CPTCFG_AR7_WDT += ar7_wdt.o
#obj-$(CPTCFG_TXX9_WDT += txx9wdt.o
obj-$(CPTCFG_OCTEON_WDT) += octeon-wdt.o
octeon-wdt-y := octeon-wdt-main.o octeon-wdt-nmi.o
#obj-$(CPTCFG_LANTIQ_WDT += lantiq_wdt.o
#obj-$(CPTCFG_LOONGSON1_WDT += loongson1_wdt.o
#obj-$(CPTCFG_RALINK_WDT += rt2880_wdt.o
#obj-$(CPTCFG_IMGPDC_WDT += imgpdc_wdt.o
#obj-$(CPTCFG_MT7621_WDT += mt7621_wdt.o
#obj-$(CPTCFG_PIC32_WDT += pic32-wdt.o
#obj-$(CPTCFG_PIC32_DMT += pic32-dmt.o

# PARISC Architecture

# POWERPC Architecture
#obj-$(CPTCFG_GEF_WDT += gef_wdt.o
#obj-$(CPTCFG_8xxx_WDT += mpc8xxx_wdt.o
#obj-$(CPTCFG_PIKA_WDT += pika_wdt.o
#obj-$(CPTCFG_BOOKE_WDT += booke_wdt.o
#obj-$(CPTCFG_MEN_A21_WDT += mena21_wdt.o

# PPC64 Architecture
#obj-$(CPTCFG_WATCHDOG_RTAS += wdrtas.o

# S390 Architecture
#obj-$(CPTCFG_DIAG288_WATCHDOG += diag288_wdt.o

# SUPERH (sh + sh64) Architecture
#obj-$(CPTCFG_SH_WDT += shwdt.o

# SPARC Architecture

# SPARC64 Architecture
#
#obj-$(CPTCFG_WATCHDOG_RIO	+= riowd.o
#obj-$(CPTCFG_WATCHDOG_CP1XXX		+= cpwd.o
#obj-$(CPTCFG_WATCHDOG_SUN4V		+= sun4v_wdt.o

# XTENSA Architecture

# Xen
#obj-$(CPTCFG_XEN_WDT += xen_wdt.o

# Architecture Independent
#obj-$(CPTCFG_BD957XMUF_WATCHDOG += bd9576_wdt.o
#obj-$(CPTCFG_DA9052_WATCHDOG += da9052_wdt.o
#obj-$(CPTCFG_DA9055_WATCHDOG += da9055_wdt.o
#obj-$(CPTCFG_DA9062_WATCHDOG += da9062_wdt.o
#obj-$(CPTCFG_DA9063_WATCHDOG += da9063_wdt.o
#obj-$(CPTCFG_GPIO_WATCHDOG	+= gpio_wdt.o
#obj-$(CPTCFG_WDAT_WDT += wdat_wdt.o
#obj-$(CPTCFG_WM831X_WATCHDOG += wm831x_wdt.o
#obj-$(CPTCFG_WM8350_WATCHDOG += wm8350_wdt.o
#obj-$(CPTCFG_MAX63XX_WATCHDOG += max63xx_wdt.o
#obj-$(CPTCFG_MAX77620_WATCHDOG += max77620_wdt.o
#obj-$(CPTCFG_ZIIRAVE_WATCHDOG += ziirave_wdt.o
#obj-$(CPTCFG_SOFT_WATCHDOG += softdog.o
#obj-$(CPTCFG_MENF21BMC_WATCHDOG += menf21bmc_wdt.o
#obj-$(CPTCFG_MENZ069_WATCHDOG += menz69_wdt.o
#obj-$(CPTCFG_RAVE_SP_WATCHDOG += rave-sp-wdt.o
#obj-$(CPTCFG_STPMIC1_WATCHDOG += stpmic1_wdt.o
#obj-$(CPTCFG_SL28CPLD_WATCHDOG += sl28cpld_wdt.o
