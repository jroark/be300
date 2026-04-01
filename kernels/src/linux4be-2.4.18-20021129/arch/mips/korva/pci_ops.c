/*
 * Markham (Korva) specific PCI support.
 *
 * Ported by Geoffrey Espin espin@idiom.com for NEC Electronics
 * based on pb1000 and ev96100 versions.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/delay.h>

#include <asm/pci_channel.h>

#include "pinot.h"

#define PCI_ACCESS_READ  0
#define PCI_ACCESS_WRITE 1

#undef DEBUG
#define DEBUG
#ifdef 	DEBUG
#define	DBG(x...)	printk(x)
#else
#define	DBG(x...)
#endif
#define	DBGX(x...)		/* extensive debug disabled */

static struct resource pci_io_resource = {
	"pci IO space",
	PCI_IO_START, PCI_IO_END,	/* 4Kbyte PCI I/O address window (PIAW) */
	IORESOURCE_IO
};

static struct resource pci_mem_resource = {
	"pci memory space",
	PCI_IOMEM_START, PCI_IOMEM_END,	/* 2Mbyte PCI MEM window */
	IORESOURCE_MEM
};

extern struct pci_ops my_pci_ops;

struct pci_channel mips_pci_channels[] = {
	{&my_pci_ops, &pci_io_resource, &pci_mem_resource, 0, 0xff},
	{NULL, NULL, NULL, 0, 0}
};

#define	make_tag(bus,dev_fn)	((bus << 16) | (dev_fn << 8))

static int
config_access(u8 access_type, struct pci_dev *dev, u8 where, u32 * data)
{
	u8 bus = dev->bus->number;
	u8 dev_fn = dev->devfn;
	u32 tmp;

	DBGX("%s:%s %d %s bus %#x dev_fn %#x tag %#x where %#x\n",
	     __FILE__, __FUNCTION__, __LINE__,
	     ((access_type == PCI_ACCESS_READ) ? "READ" : "WRITE"),
	     (int) bus, (int) dev_fn, (int) make_tag(bus, dev_fn), (int) where);

	tmp = *P_PCI_COMMAND & 0x0000ffff;
	*P_PCI_COMMAND = 0xffff0000 | tmp;

	*P_PCAR = CFGEN | make_tag(bus, dev_fn) | (where & ~3);

	if (access_type == PCI_ACCESS_WRITE) {
		*P_PCDR = *data;
	} else {
		*data = *P_PCDR;
	}

	tmp = *P_PCI_COMMAND;

	if ((tmp & ((PCI_STATUS_PARITY | PCI_STATUS_SIG_TARGET_ABORT |
		     PCI_STATUS_REC_TARGET_ABORT |
		     PCI_STATUS_REC_MASTER_ABORT |
		     PCI_STATUS_SIG_SYSTEM_ERROR |
		     PCI_STATUS_DETECTED_PARITY) << 16)) != 0) {
		*P_PCI_COMMAND = 0xffff0000 | tmp;

		DBG("%s:%s %d %s bus %#x dev_fn %#x tag %#x where %#x reg %#x\n", __FILE__, __FUNCTION__, __LINE__, ((access_type == PCI_ACCESS_READ) ? "READ" : "WRITE"), (int) bus, (int) dev_fn, (int) make_tag(bus, dev_fn), (int) where, tmp);

		return -1;
	}

	DBGX("config_access: %d bus %d dev_fn %x at %x *data %x\n",
	     access_type, bus, dev_fn, where, *data);

	return PCIBIOS_SUCCESSFUL;
}

static int
read_config_byte(struct pci_dev *dev, int where, u8 * val)
{
	u32 data = 0;
	int ret = config_access(PCI_ACCESS_READ, dev, where, &data);

	*val = (data >> ((where & 3) << 3)) & 0xff;

	return ret;
}

static int
read_config_word(struct pci_dev *dev, int where, u16 * val)
{
	u32 data = 0;
	int ret = config_access(PCI_ACCESS_READ, dev, where, &data);

	*val = (data >> ((where & 3) << 3)) & 0xffff;

	return ret;
}

static int
read_config_dword(struct pci_dev *dev, int where, u32 * val)
{
	return config_access(PCI_ACCESS_READ, dev, where, val);
}

static int
write_config_byte(struct pci_dev *dev, int where, u8 val)
{
	u32 data = 0;

	if (config_access(PCI_ACCESS_READ, dev, where, &data))
		return -1;

	data = (data & ~(0xff << ((where & 3) << 3))) |
	    (val << ((where & 3) << 3));

	if (config_access(PCI_ACCESS_WRITE, dev, where, &data))
		return -1;

	return PCIBIOS_SUCCESSFUL;
}

static int
write_config_word(struct pci_dev *dev, int where, u16 val)
{
	u32 data = 0;

	if (where & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (config_access(PCI_ACCESS_READ, dev, where, &data))
		return -1;

	data = (data & ~(0xffff << ((where & 3) << 3))) |
	    (val << ((where & 3) << 3));

	if (config_access(PCI_ACCESS_WRITE, dev, where, &data))
		return -1;

	return PCIBIOS_SUCCESSFUL;
}

static int
write_config_dword(struct pci_dev *dev, int where, u32 val)
{
	if (where & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (config_access(PCI_ACCESS_WRITE, dev, where, &val))
		return -1;

	return PCIBIOS_SUCCESSFUL;
}

struct pci_ops my_pci_ops = {
	read_config_byte,
	read_config_word,
	read_config_dword,
	write_config_byte,
	write_config_word,
	write_config_dword
};
