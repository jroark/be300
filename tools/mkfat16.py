#!/usr/bin/env python3
"""Write a 16 MiB CompactFlash image: an MBR and one FAT16 partition holding
HELLO.TXT. For --cf tests on hosts without dosfstools or mtools.

    tools/mkfat16.py out.img
"""
import struct, sys
SEC=512; total=32768; pstart=63; psecs=total-pstart
spc=4; rsvd=1; nfat=2; rootent=512; rootsecs=rootent*32//SEC
# solve FAT size
fatsz=1
while True:
    clusters=(psecs-rsvd-nfat*fatsz-rootsecs)//spc
    need=((clusters+2)*2+SEC-1)//SEC
    if need<=fatsz: break
    fatsz=need
img=bytearray(total*SEC)
mbr=bytearray(SEC)
mbr[446:462]=struct.pack('<B3sB3sII',0x80,b'\x01\x01\x00',0x06,b'\xfe\x3f\x01',pstart,psecs)
mbr[510:512]=b'\x55\xaa'; img[0:SEC]=mbr
bs=bytearray(SEC)
bs[0:3]=b'\xeb\x3c\x90'; bs[3:11]=b'MSDOS5.0'
struct.pack_into('<HBHBHHBHHHII',bs,11,SEC,spc,rsvd,nfat,rootent,total-pstart if psecs<65536 else 0,0xF8,fatsz,63,16,pstart,0)
bs[36]=0x80; bs[38]=0x29; struct.pack_into('<I',bs,39,0x12345678)
bs[43:54]=b'BE300CF    '; bs[54:62]=b'FAT16   '; bs[510:512]=b'\x55\xaa'
p=pstart*SEC; img[p:p+SEC]=bs
fat=bytearray(fatsz*SEC); struct.pack_into('<HHH',fat,0,0xFFF8,0xFFFF,0xFFFF)  # cluster 2 = file, EOC
for k in range(nfat):
    o=(pstart+rsvd+k*fatsz)*SEC; img[o:o+len(fat)]=fat
root=(pstart+rsvd+nfat*fatsz)*SEC
data=b'hello from the emulated CF card\r\n'
img[root:root+32]=struct.pack('<11sB10xHHHI',b'BE300CF    ',0x08,0,0x5B21,0,0)
img[root+32:root+64]=struct.pack('<11sB10xHHHI',b'HELLO   TXT',0x20,0,0x5B21,2,len(data))
d0=root+rootsecs*SEC; img[d0:d0+len(data)]=data
open(sys.argv[1],'wb').write(img)
print('fatsz',fatsz,'clusters',clusters)
