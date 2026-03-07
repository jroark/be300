/*
 * arch/mips/vr4131/casio-be300/pci_fixup.c
 *
 * PCI fixup routines for the Casio Cassiopeia BE-300
 *
 * Copyright (C) 2002 Paul Mundt <lethal@chaoticdreams.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/pci.h>

#include <asm/vr41xx/vrc4173.h>
#include <asm/vr41xx/vr41xx.h>
#include <asm/pci_channel.h>

struct pci_channel mips_pci_channels[] = {
	{ 0, }
};

void __init pcibios_fixup_resources(struct pci_dev *pdev)
{
}

void __init pcibios_fixup(void)
{
}

void __init pcibios_fixup_irqs(void)
{
}

unsigned int pcibios_assign_all_busses(void)
{
	return 0;
}

