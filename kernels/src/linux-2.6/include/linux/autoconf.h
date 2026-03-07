/*
 * Automatically generated C config: don't edit
 */
#define AUTOCONF_INCLUDED
#define CONFIG_MIPS 1
#undef CONFIG_MIPS64
#undef CONFIG_64BIT
#define CONFIG_MIPS32 1

/*
 * Code maturity level options
 */
#define CONFIG_EXPERIMENTAL 1
#define CONFIG_CLEAN_COMPILE 1
#define CONFIG_BROKEN_ON_SMP 1

/*
 * General setup
 */
#undef CONFIG_SWAP
#define CONFIG_SYSVIPC 1
#undef CONFIG_POSIX_MQUEUE
#define CONFIG_BSD_PROCESS_ACCT 1
#undef CONFIG_BSD_PROCESS_ACCT_V3
#define CONFIG_SYSCTL 1
#undef CONFIG_AUDIT
#define CONFIG_LOG_BUF_SHIFT 14
#define CONFIG_HOTPLUG 1
#undef CONFIG_IKCONFIG
#define CONFIG_EMBEDDED 1
#define CONFIG_KALLSYMS 1
#undef CONFIG_KALLSYMS_EXTRA_PASS
#define CONFIG_FUTEX 1
#define CONFIG_EPOLL 1
#define CONFIG_IOSCHED_NOOP 1
#define CONFIG_IOSCHED_AS 1
#define CONFIG_IOSCHED_DEADLINE 1
#define CONFIG_IOSCHED_CFQ 1
#undef CONFIG_CC_OPTIMIZE_FOR_SIZE

/*
 * Loadable module support
 */
#undef CONFIG_MODULES

/*
 * Machine selection
 */
#undef CONFIG_MACH_JAZZ
#undef CONFIG_BAGET_MIPS
#define CONFIG_MACH_VR41XX 1
#undef CONFIG_CASIO_E55
#undef CONFIG_IBM_WORKPAD
#undef CONFIG_TANBAC_TB0226
#undef CONFIG_TANBAC_TB0229
#undef CONFIG_VICTOR_MPC30X
#undef CONFIG_ZAO_CAPCELLA
#undef CONFIG_VRC4171
#define CONFIG_CASIO_BE300 1
#undef CONFIG_TOSHIBA_JMR3927
#undef CONFIG_MIPS_COBALT
#undef CONFIG_MACH_DECSTATION
#undef CONFIG_MIPS_EV64120
#undef CONFIG_MIPS_EV96100
#undef CONFIG_MIPS_IVR
#undef CONFIG_LASAT
#undef CONFIG_MIPS_ITE8172
#undef CONFIG_MIPS_ATLAS
#undef CONFIG_MIPS_MALTA
#undef CONFIG_MIPS_SEAD
#undef CONFIG_MOMENCO_OCELOT
#undef CONFIG_MOMENCO_OCELOT_G
#undef CONFIG_MOMENCO_OCELOT_C
#undef CONFIG_MOMENCO_JAGUAR_ATX
#undef CONFIG_PMC_YOSEMITE
#undef CONFIG_DDB5074
#undef CONFIG_DDB5476
#undef CONFIG_DDB5477
#undef CONFIG_NEC_OSPREY
#undef CONFIG_SGI_IP22
#undef CONFIG_SGI_IP32
#undef CONFIG_SOC_AU1X00
#undef CONFIG_SIBYTE_SB1xxx_SOC
#undef CONFIG_SNI_RM200_PCI
#undef CONFIG_TOSHIBA_RBTX4927
#define CONFIG_RWSEM_GENERIC_SPINLOCK 1
#define CONFIG_HAVE_DEC_LOCK 1
#define CONFIG_DMA_NONCOHERENT 1
#define CONFIG_CPU_LITTLE_ENDIAN 1
#define CONFIG_IRQ_CPU 1
#define CONFIG_MIPS_L1_CACHE_SHIFT 5
#define CONFIG_FB 1

/*
 * CPU selection
 */
#undef CONFIG_CPU_MIPS32
#undef CONFIG_CPU_MIPS64
#undef CONFIG_CPU_R3000
#undef CONFIG_CPU_TX39XX
#define CONFIG_CPU_VR41XX 1
#undef CONFIG_CPU_R4300
#undef CONFIG_CPU_R4X00
#undef CONFIG_CPU_TX49XX
#undef CONFIG_CPU_R5000
#undef CONFIG_CPU_R5432
#undef CONFIG_CPU_R6000
#undef CONFIG_CPU_NEVADA
#undef CONFIG_CPU_R8000
#undef CONFIG_CPU_R10000
#undef CONFIG_CPU_RM7000
#undef CONFIG_CPU_RM9000
#undef CONFIG_CPU_SB1
#define CONFIG_PAGE_SIZE_4KB 1
#undef CONFIG_PAGE_SIZE_8KB
#undef CONFIG_PAGE_SIZE_16KB
#undef CONFIG_PAGE_SIZE_64KB
#undef CONFIG_CPU_ADVANCED
#define CONFIG_CPU_HAS_SYNC 1
#undef CONFIG_PREEMPT
#undef CONFIG_DEBUG_SPINLOCK_SLEEP

/*
 * Bus options (PCI, PCMCIA, EISA, ISA, TC)
 */
#define CONFIG_ISA 1
#define CONFIG_MMU 1

/*
 * PCMCIA/CardBus support
 */
#undef CONFIG_PCMCIA
#define CONFIG_PCMCIA_PROBE 1

/*
 * PCI Hotplug Support
 */

/*
 * Executable file formats
 */
#define CONFIG_BINFMT_ELF 1
#undef CONFIG_BINFMT_MISC
#define CONFIG_TRAD_SIGNALS 1

/*
 * MIPS initrd options
 */
#define CONFIG_EMBEDDED_RAMDISK 1
#define CONFIG_EMBEDDED_RAMDISK_IMAGE "ramdisk.gz"

/*
 * Device Drivers
 */

/*
 * Generic Driver Options
 */
#define CONFIG_STANDALONE 1
#define CONFIG_PREVENT_FIRMWARE_BUILD 1
#undef CONFIG_FW_LOADER

/*
 * Memory Technology Devices (MTD)
 */
#define CONFIG_MTD 1
#undef CONFIG_MTD_DEBUG
#undef CONFIG_MTD_PARTITIONS
#undef CONFIG_MTD_CONCAT

/*
 * User Modules And Translation Layers
 */
#define CONFIG_MTD_CHAR 1
#undef CONFIG_MTD_BLOCK
#undef CONFIG_MTD_BLOCK_RO
#undef CONFIG_FTL
#define CONFIG_NFTL 1
#undef CONFIG_NFTL_RW
#undef CONFIG_INFTL

/*
 * RAM/ROM/Flash chip drivers
 */
#undef CONFIG_MTD_CFI
#undef CONFIG_MTD_JEDECPROBE
#define CONFIG_MTD_MAP_BANK_WIDTH_1 1
#define CONFIG_MTD_MAP_BANK_WIDTH_2 1
#define CONFIG_MTD_MAP_BANK_WIDTH_4 1
#undef CONFIG_MTD_MAP_BANK_WIDTH_8
#undef CONFIG_MTD_MAP_BANK_WIDTH_16
#undef CONFIG_MTD_MAP_BANK_WIDTH_32
#define CONFIG_MTD_CFI_I1 1
#define CONFIG_MTD_CFI_I2 1
#undef CONFIG_MTD_CFI_I4
#undef CONFIG_MTD_CFI_I8
#undef CONFIG_MTD_RAM
#undef CONFIG_MTD_ROM
#undef CONFIG_MTD_ABSENT

/*
 * Mapping drivers for chip access
 */
#undef CONFIG_MTD_COMPLEX_MAPPINGS

/*
 * Self-contained MTD device drivers
 */
#undef CONFIG_MTD_SLRAM
#undef CONFIG_MTD_PHRAM
#undef CONFIG_MTD_MTDRAM
#undef CONFIG_MTD_BLKMTD

/*
 * Disk-On-Chip Device Drivers
 */
#undef CONFIG_MTD_DOC2000
#undef CONFIG_MTD_DOC2001
#undef CONFIG_MTD_DOC2001PLUS

/*
 * NAND Flash Device Drivers
 */
#define CONFIG_MTD_NAND 1
#undef CONFIG_MTD_NAND_VERIFY_WRITE
#define CONFIG_MTD_NAND_IDS 1
#undef CONFIG_MTD_NAND_DISKONCHIP

/*
 * Parallel port support
 */
#undef CONFIG_PARPORT

/*
 * Plug and Play support
 */
#undef CONFIG_PNP

/*
 * Block devices
 */
#undef CONFIG_BLK_DEV_FD
#undef CONFIG_BLK_DEV_XD
#undef CONFIG_BLK_DEV_LOOP
#undef CONFIG_BLK_DEV_NBD
#define CONFIG_BLK_DEV_RAM 1
#define CONFIG_BLK_DEV_RAM_SIZE 4096
#define CONFIG_BLK_DEV_INITRD 1
#undef CONFIG_LBD

/*
 * ATA/ATAPI/MFM/RLL support
 */
#undef CONFIG_IDE

/*
 * SCSI device support
 */
#undef CONFIG_SCSI

/*
 * Old CD-ROM drivers (not SCSI, not IDE)
 */
#undef CONFIG_CD_NO_IDESCSI

/*
 * Multi-device support (RAID and LVM)
 */
#undef CONFIG_MD

/*
 * Fusion MPT device support
 */

/*
 * IEEE 1394 (FireWire) support
 */

/*
 * I2O device support
 */

/*
 * Networking support
 */
#define CONFIG_NET 1

/*
 * Networking options
 */
#define CONFIG_PACKET 1
#define CONFIG_PACKET_MMAP 1
#undef CONFIG_NETLINK_DEV
#define CONFIG_UNIX 1
#undef CONFIG_NET_KEY
#define CONFIG_INET 1
#undef CONFIG_IP_MULTICAST
#undef CONFIG_IP_ADVANCED_ROUTER
#undef CONFIG_IP_PNP
#undef CONFIG_NET_IPIP
#undef CONFIG_NET_IPGRE
#undef CONFIG_ARPD
#undef CONFIG_SYN_COOKIES
#undef CONFIG_INET_AH
#undef CONFIG_INET_ESP
#undef CONFIG_INET_IPCOMP
#undef CONFIG_IPV6
#undef CONFIG_NETFILTER

/*
 * SCTP Configuration (EXPERIMENTAL)
 */
#undef CONFIG_IP_SCTP
#undef CONFIG_ATM
#undef CONFIG_BRIDGE
#undef CONFIG_VLAN_8021Q
#undef CONFIG_DECNET
#undef CONFIG_LLC2
#undef CONFIG_IPX
#undef CONFIG_ATALK
#undef CONFIG_X25
#undef CONFIG_LAPB
#undef CONFIG_NET_DIVERT
#undef CONFIG_ECONET
#undef CONFIG_WAN_ROUTER
#undef CONFIG_NET_FASTROUTE
#undef CONFIG_NET_HW_FLOWCONTROL

/*
 * QoS and/or fair queueing
 */
#undef CONFIG_NET_SCHED
#undef CONFIG_NET_CLS_ROUTE

/*
 * Network testing
 */
#undef CONFIG_NET_PKTGEN
#undef CONFIG_NETPOLL
#undef CONFIG_NET_POLL_CONTROLLER
#undef CONFIG_HAMRADIO
#undef CONFIG_IRDA
#undef CONFIG_BT
#define CONFIG_NETDEVICES 1
#define CONFIG_DUMMY 1
#undef CONFIG_BONDING
#undef CONFIG_EQUALIZER
#undef CONFIG_TUN

/*
 * ARCnet devices
 */
#undef CONFIG_ARCNET

/*
 * Ethernet (10 or 100Mbit)
 */
#undef CONFIG_NET_ETHERNET

/*
 * Ethernet (1000 Mbit)
 */

/*
 * Ethernet (10000 Mbit)
 */

/*
 * Token Ring devices
 */
#undef CONFIG_TR

/*
 * Wireless LAN (non-hamradio)
 */
#undef CONFIG_NET_RADIO

/*
 * Wan interfaces
 */
#undef CONFIG_WAN
#define CONFIG_PPP 1
#undef CONFIG_PPP_MULTILINK
#undef CONFIG_PPP_FILTER
#define CONFIG_PPP_ASYNC 1
#undef CONFIG_PPP_SYNC_TTY
#define CONFIG_PPP_DEFLATE 1
#define CONFIG_PPP_BSDCOMP 1
#undef CONFIG_PPPOE
#undef CONFIG_SLIP
#undef CONFIG_SHAPER
#undef CONFIG_NETCONSOLE

/*
 * ISDN subsystem
 */
#undef CONFIG_ISDN

/*
 * Telephony Support
 */
#undef CONFIG_PHONE

/*
 * Input device support
 */
#define CONFIG_INPUT 1

/*
 * Userland interfaces
 */
#undef CONFIG_INPUT_MOUSEDEV
#undef CONFIG_INPUT_JOYDEV
#undef CONFIG_INPUT_TSDEV
#undef CONFIG_INPUT_EVDEV
#undef CONFIG_INPUT_EVBUG

/*
 * Input I/O drivers
 */
#undef CONFIG_GAMEPORT
#define CONFIG_SOUND_GAMEPORT 1
#define CONFIG_SERIO 1
#undef CONFIG_SERIO_I8042
#define CONFIG_SERIO_SERPORT 1
#undef CONFIG_SERIO_CT82C710

/*
 * Input Device Drivers
 */
#undef CONFIG_INPUT_KEYBOARD
#undef CONFIG_INPUT_MOUSE
#undef CONFIG_INPUT_JOYSTICK
#define CONFIG_INPUT_TOUCHSCREEN 1
#undef CONFIG_TOUCHSCREEN_GUNZE
#undef CONFIG_INPUT_MISC

/*
 * Character devices
 */
#define CONFIG_VT 1
#define CONFIG_VT_CONSOLE 1
#define CONFIG_HW_CONSOLE 1
#define CONFIG_SERIAL_NONSTANDARD 1
#undef CONFIG_COMPUTONE
#undef CONFIG_ROCKETPORT
#undef CONFIG_CYCLADES
#undef CONFIG_DIGIEPCA
#undef CONFIG_DIGI
#undef CONFIG_ESPSERIAL
#undef CONFIG_MOXA_INTELLIO
#undef CONFIG_MOXA_SMARTIO
#undef CONFIG_SYNCLINKMP
#undef CONFIG_N_HDLC
#undef CONFIG_RISCOM8
#undef CONFIG_SPECIALIX
#undef CONFIG_SX
#undef CONFIG_RIO
#undef CONFIG_STALDRV
#undef CONFIG_SERIAL_BE300

/*
 * Serial drivers
 */
#undef CONFIG_SERIAL_8250

/*
 * Non-8250 serial port support
 */
#define CONFIG_UNIX98_PTYS 1
#undef CONFIG_LEGACY_PTYS
#undef CONFIG_QIC02_TAPE

/*
 * IPMI
 */
#undef CONFIG_IPMI_HANDLER

/*
 * Watchdog Cards
 */
#undef CONFIG_WATCHDOG
#undef CONFIG_RTC
#undef CONFIG_GEN_RTC
#undef CONFIG_DTLK
#undef CONFIG_R3964

/*
 * Ftape, the floppy tape device driver
 */
#undef CONFIG_AGP
#undef CONFIG_DRM
#undef CONFIG_RAW_DRIVER

/*
 * I2C support
 */
#undef CONFIG_I2C

/*
 * Dallas's 1-wire bus
 */
#undef CONFIG_W1

/*
 * Misc devices
 */

/*
 * Multimedia devices
 */
#undef CONFIG_VIDEO_DEV

/*
 * Digital Video Broadcasting Devices
 */
#undef CONFIG_DVB

/*
 * Graphics support
 */
#define CONFIG_FB_SIMPLE 1
#define CONFIG_FB_SIMPLE_MEMBASE 0xaa200000
#define CONFIG_FB_SIMPLE_MEMSIZE 0x0
#undef CONFIG_FB_VIRTUAL

/*
 * Console display driver support
 */
#undef CONFIG_VGA_CONSOLE
#undef CONFIG_MDA_CONSOLE
#define CONFIG_DUMMY_CONSOLE 1
#define CONFIG_FRAMEBUFFER_CONSOLE 1
#define CONFIG_FONTS 1
#define CONFIG_FONT_8x8 1
#define CONFIG_FONT_8x16 1
#undef CONFIG_FONT_6x11
#undef CONFIG_FONT_PEARL_8x8
#undef CONFIG_FONT_ACORN_8x8
#undef CONFIG_FONT_MINI_4x6
#undef CONFIG_FONT_SUN8x16
#undef CONFIG_FONT_SUN12x22

/*
 * Logo configuration
 */
#define CONFIG_LOGO 1
#undef CONFIG_LOGO_LINUX_MONO
#define CONFIG_LOGO_LINUX_VGA16 1
#define CONFIG_LOGO_BE300_CLUT224 1
#undef CONFIG_LOGO_LINUX_CLUT224

/*
 * Sound
 */
#undef CONFIG_SOUND

/*
 * USB support
 */

/*
 * USB Gadget Support
 */
#undef CONFIG_USB_GADGET

/*
 * File systems
 */
#define CONFIG_EXT2_FS 1
#undef CONFIG_EXT2_FS_XATTR
#undef CONFIG_EXT3_FS
#undef CONFIG_JBD
#undef CONFIG_REISERFS_FS
#undef CONFIG_JFS_FS
#undef CONFIG_XFS_FS
#undef CONFIG_MINIX_FS
#undef CONFIG_ROMFS_FS
#undef CONFIG_QUOTA
#undef CONFIG_AUTOFS_FS
#define CONFIG_AUTOFS4_FS 1

/*
 * CD-ROM/DVD Filesystems
 */
#undef CONFIG_ISO9660_FS
#undef CONFIG_UDF_FS

/*
 * DOS/FAT/NT Filesystems
 */
#define CONFIG_FAT_FS 1
#define CONFIG_MSDOS_FS 1
#define CONFIG_VFAT_FS 1
#undef CONFIG_NTFS_FS

/*
 * Pseudo filesystems
 */
#define CONFIG_PROC_FS 1
#define CONFIG_PROC_KCORE 1
#define CONFIG_SYSFS 1
#undef CONFIG_DEVFS_FS
#undef CONFIG_DEVPTS_FS_XATTR
#define CONFIG_TMPFS 1
#undef CONFIG_HUGETLB_PAGE
#define CONFIG_RAMFS 1

/*
 * Miscellaneous filesystems
 */
#undef CONFIG_ADFS_FS
#undef CONFIG_AFFS_FS
#undef CONFIG_HFS_FS
#undef CONFIG_HFSPLUS_FS
#undef CONFIG_BEFS_FS
#undef CONFIG_BFS_FS
#undef CONFIG_EFS_FS
#undef CONFIG_JFFS_FS
#undef CONFIG_JFFS2_FS
#undef CONFIG_JFFS2_COMPRESSION_OPTIONS
#undef CONFIG_CRAMFS
#undef CONFIG_VXFS_FS
#undef CONFIG_HPFS_FS
#undef CONFIG_QNX4FS_FS
#undef CONFIG_SYSV_FS
#undef CONFIG_UFS_FS

/*
 * Network File Systems
 */
#undef CONFIG_NFS_FS
#undef CONFIG_NFSD
#undef CONFIG_EXPORTFS
#undef CONFIG_SMB_FS
#undef CONFIG_CIFS
#undef CONFIG_NCP_FS
#undef CONFIG_CODA_FS
#undef CONFIG_AFS_FS

/*
 * Partition Types
 */
#undef CONFIG_PARTITION_ADVANCED
#define CONFIG_MSDOS_PARTITION 1

/*
 * Native Language Support
 */
#define CONFIG_NLS 1
#define CONFIG_NLS_DEFAULT "iso8859-1"
#undef CONFIG_NLS_CODEPAGE_437
#undef CONFIG_NLS_CODEPAGE_737
#undef CONFIG_NLS_CODEPAGE_775
#undef CONFIG_NLS_CODEPAGE_850
#undef CONFIG_NLS_CODEPAGE_852
#undef CONFIG_NLS_CODEPAGE_855
#undef CONFIG_NLS_CODEPAGE_857
#undef CONFIG_NLS_CODEPAGE_860
#undef CONFIG_NLS_CODEPAGE_861
#undef CONFIG_NLS_CODEPAGE_862
#undef CONFIG_NLS_CODEPAGE_863
#undef CONFIG_NLS_CODEPAGE_864
#undef CONFIG_NLS_CODEPAGE_865
#undef CONFIG_NLS_CODEPAGE_866
#undef CONFIG_NLS_CODEPAGE_869
#undef CONFIG_NLS_CODEPAGE_936
#undef CONFIG_NLS_CODEPAGE_950
#undef CONFIG_NLS_CODEPAGE_932
#undef CONFIG_NLS_CODEPAGE_949
#undef CONFIG_NLS_CODEPAGE_874
#undef CONFIG_NLS_ISO8859_8
#undef CONFIG_NLS_CODEPAGE_1250
#undef CONFIG_NLS_CODEPAGE_1251
#undef CONFIG_NLS_ASCII
#undef CONFIG_NLS_ISO8859_1
#undef CONFIG_NLS_ISO8859_2
#undef CONFIG_NLS_ISO8859_3
#undef CONFIG_NLS_ISO8859_4
#undef CONFIG_NLS_ISO8859_5
#undef CONFIG_NLS_ISO8859_6
#undef CONFIG_NLS_ISO8859_7
#undef CONFIG_NLS_ISO8859_9
#undef CONFIG_NLS_ISO8859_13
#undef CONFIG_NLS_ISO8859_14
#undef CONFIG_NLS_ISO8859_15
#undef CONFIG_NLS_KOI8_R
#undef CONFIG_NLS_KOI8_U
#undef CONFIG_NLS_UTF8

/*
 * Kernel hacking
 */
#define CONFIG_CROSSCOMPILE 1
#define CONFIG_CMDLINE ""
#undef CONFIG_DEBUG_KERNEL

/*
 * Security options
 */
#undef CONFIG_SECURITY

/*
 * Cryptographic options
 */
#undef CONFIG_CRYPTO

/*
 * Library routines
 */
#define CONFIG_CRC_CCITT 1
#undef CONFIG_CRC32
#undef CONFIG_LIBCRC32C
#define CONFIG_ZLIB_INFLATE 1
#define CONFIG_ZLIB_DEFLATE 1
