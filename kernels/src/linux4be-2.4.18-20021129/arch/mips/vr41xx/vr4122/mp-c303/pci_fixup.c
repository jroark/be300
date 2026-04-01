/*
 * FILE NAME
 *	arch/mips/vr41xx/vr4122/mp-c303/pci_fixup.c
 *
 * BRIEF MODULE DESCRIPTION
 *	The Victor MP-C303/304 specific PCI fixups.
 *
 * Copyright 2002 Yoichi Yuasa
 *                yuasa@hh.iij4u.or.jp
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 */
#include <linux/config.h>
#include <linux/init.h>
#include <linux/pci.h>

#include <asm/vr41xx/mp-c303.h>
#ifdef CONFIG_VRC4173
#include <asm/vr41xx/vrc4173.h>
#endif

void __init pcibios_fixup_resources(struct pci_dev *dev)
{
}

void __init pcibios_fixup(void)
{
}

void __init pcibios_fixup_irqs(void)
{
	struct pci_dev *dev;
	u8 slot, func, pin;

	pci_for_each_dev(dev) {
		slot = PCI_SLOT(dev->devfn);
		func = PCI_FUNC(dev->devfn);
		dev->irq = 0;

		switch (slot) {
#ifdef CONFIG_VRC4173
		case 12:
			dev->irq = VRC4173_CARDU1_IRQ;
			break;
		case 13:
			dev->irq = VRC4173_CARDU2_IRQ;
			break;
		case 24:
			dev->irq = VRC4173_CARDU1_IRQ;
			break;
		case 25:
			dev->irq = VRC4173_CARDU2_IRQ;
			break;
		case 30:
			switch (func) {
			case 0:
				dev->irq = VRC4173_IRQ;
				break;
			case 1:
				dev->irq = VRC4173_AC97U_IRQ;
				break;
			case 2:
				dev->irq = VRC4173_USBU_IRQ;
				break;
			}
			break;
#endif
		}

		pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
	}
}

unsigned int pcibios_assign_all_busses(void)
{
	return 0;
}
