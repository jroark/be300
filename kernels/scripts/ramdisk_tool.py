#!/usr/bin/env python3
import sys
import subprocess
import argparse
import struct
import io
import gzip

def get_symbol_vaddr(readelf_cmd, kernel_file, sym_name):
    """Run readelf -s and return the virtual address of a symbol."""
    cmd = [readelf_cmd, "-s", kernel_file]
    try:
        output = subprocess.check_output(cmd, stderr=subprocess.DEVNULL, universal_newlines=True)
    except subprocess.CalledProcessError:
        sys.exit(f"Error: Could not run {readelf_cmd} -s {kernel_file}")

    for line in output.splitlines():
        if sym_name in line:
            parts = line.split()
            # typical format: 1234: 80123456 1234 OBJECT GLOBAL DEFAULT 1 __rd_start
            for i, p in enumerate(parts):
                if p == sym_name:
                    # The address is usually the 2nd column
                    vaddr_str = parts[1]
                    return int(vaddr_str, 16)
    return None

def get_file_offset(readelf_cmd, kernel_file, vaddr):
    """Run readelf -l and find the file offset for a given virtual address."""
    cmd = [readelf_cmd, "-l", kernel_file]
    try:
        output = subprocess.check_output(cmd, stderr=subprocess.DEVNULL, universal_newlines=True)
    except subprocess.CalledProcessError:
        sys.exit(f"Error: Could not run {readelf_cmd} -l {kernel_file}")

    # Parse program headers to find the LOAD segment containing vaddr
    # Format of readelf -l output:
    # Program Headers:
    #   Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
    #   LOAD           0x001000 0x80000000 0x80000000 0x12345 0x12345 R E 0x1000
    
    in_ph = False
    for line in output.splitlines():
        if line.startswith("Program Headers:"):
            in_ph = True
            continue
        if line.startswith(" Section to Segment mapping:"):
            break
        if not in_ph:
            continue
            
        parts = line.strip().split()
        if len(parts) >= 6 and parts[0] == "LOAD":
            try:
                offset = int(parts[1], 16)
                p_vaddr = int(parts[2], 16)
                memsz = int(parts[5], 16)
                if p_vaddr <= vaddr < p_vaddr + memsz:
                    # Found the segment
                    return offset + (vaddr - p_vaddr)
            except ValueError:
                pass
    return None

def extract_ramdisk(args):
    start_vaddr = get_symbol_vaddr(args.readelf, args.kernel, "__rd_start")
    end_vaddr = get_symbol_vaddr(args.readelf, args.kernel, "__rd_end")
    
    if start_vaddr is None or end_vaddr is None:
        sys.exit("Error: Could not find __rd_start or __rd_end in the kernel symbol table.")
        
    reserved_size = end_vaddr - start_vaddr
    print(f"[*] Found __rd_start at 0x{start_vaddr:08x}")
    print(f"[*] Found __rd_end   at 0x{end_vaddr:08x}")
    print(f"[*] Reserved size: {reserved_size} bytes")
    
    file_offset = get_file_offset(args.readelf, args.kernel, start_vaddr)
    if file_offset is None:
        sys.exit("Error: Could not map __rd_start to a file offset.")
        
    print(f"[*] File offset: 0x{file_offset:08x}")
    
    with open(args.kernel, "rb") as f:
        f.seek(file_offset)
        data = f.read(reserved_size)
        
    if not data:
        sys.exit("Error: Failed to read data from kernel file.")
        
    # Check if data is gzipped (magic bytes 1F 8B)
    if data.startswith(b"\x1f\x8b"):
        print("[*] Detected gzip compression. Decompressing...")
        try:
            # We use GzipFile to read, as it gracefully stops at the end of the gzip stream,
            # ignoring the trailing null bytes padded in the kernel.
            with gzip.GzipFile(fileobj=io.BytesIO(data), mode="rb") as gz:
                decompressed_data = gz.read()
            print(f"[*] Decompressed size: {len(decompressed_data)} bytes")
            
            with open(args.output, "wb") as f_out:
                f_out.write(decompressed_data)
            print(f"[+] Successfully extracted uncompressed ramdisk to {args.output}")
        except Exception as e:
            sys.exit(f"Error during decompression: {e}")
    else:
        print("[*] No gzip compression detected. Extracting raw data...")
        with open(args.output, "wb") as f_out:
            f_out.write(data)
        print(f"[+] Successfully extracted raw ramdisk to {args.output}")

def insert_ramdisk(args):
    start_vaddr = get_symbol_vaddr(args.readelf, args.kernel, "__rd_start")
    end_vaddr = get_symbol_vaddr(args.readelf, args.kernel, "__rd_end")
    
    if start_vaddr is None or end_vaddr is None:
        sys.exit("Error: Could not find __rd_start or __rd_end in the kernel symbol table.")
        
    reserved_size = end_vaddr - start_vaddr
    print(f"[*] Found __rd_start at 0x{start_vaddr:08x}")
    print(f"[*] Found __rd_end   at 0x{end_vaddr:08x}")
    print(f"[*] Reserved size: {reserved_size} bytes")
    
    file_offset = get_file_offset(args.readelf, args.kernel, start_vaddr)
    if file_offset is None:
        sys.exit("Error: Could not map __rd_start to a file offset.")
        
    print(f"[*] File offset: 0x{file_offset:08x}")
    
    with open(args.ramdisk, "rb") as f:
        ramdisk_data = f.read()
        
    print(f"[*] Input ramdisk size: {len(ramdisk_data)} bytes")
    
    # Check if we need to compress
    if not ramdisk_data.startswith(b"\x1f\x8b"):
        print("[*] Input is not gzipped. Compressing with max level (9)...")
        # Compress in memory
        out_buf = io.BytesIO()
        with gzip.GzipFile(fileobj=out_buf, mode="wb", compresslevel=9, filename="", mtime=0) as gz:
            gz.write(ramdisk_data)
        final_data = out_buf.getvalue()
        print(f"[*] Compressed size: {len(final_data)} bytes")
    else:
        print("[*] Input is already gzipped.")
        final_data = ramdisk_data
        
    if len(final_data) > reserved_size:
        sys.exit(f"Error: Final ramdisk data ({len(final_data)} bytes) is larger than reserved size ({reserved_size} bytes).")
        
    padding_len = reserved_size - len(final_data)
    padded_data = final_data + (b"\x00" * padding_len)
    
    print(f"[*] Writing {len(padded_data)} bytes to kernel at offset 0x{file_offset:08x}...")
    with open(args.kernel, "r+b") as f:
        f.seek(file_offset)
        f.write(padded_data)
        
    print("[+] Successfully inserted ramdisk into the kernel.")

def main():
    parser = argparse.ArgumentParser(description="Tool to extract and insert embedded initrd (ramdisk) in MIPS ELF kernels.")
    parser.add_argument("--readelf", default="mipsel-linux-gnu-readelf", help="Path to readelf executable (default: mipsel-linux-gnu-readelf)")
    
    subparsers = parser.add_subparsers(dest="command", required=True)
    
    # Extract parser
    extract_parser = subparsers.add_parser("extract", help="Extract ramdisk from a kernel")
    extract_parser.add_argument("kernel", help="Kernel ELF file (e.g., vmlinux)")
    extract_parser.add_argument("output", help="Output ramdisk file (e.g., ramdisk.ext2)")
    
    # Insert parser
    insert_parser = subparsers.add_parser("insert", help="Insert ramdisk into a kernel")
    insert_parser.add_argument("kernel", help="Kernel ELF file to modify (e.g., vmlinux)")
    insert_parser.add_argument("ramdisk", help="Replacement ramdisk file (e.g., ramdisk.ext2)")
    
    args = parser.parse_args()
    
    if args.command == "extract":
        extract_ramdisk(args)
    elif args.command == "insert":
        insert_ramdisk(args)

if __name__ == "__main__":
    main()
