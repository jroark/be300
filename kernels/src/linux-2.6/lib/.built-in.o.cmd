cmd_lib/built-in.o :=  mipsel-linux-ld  --oformat elf32-tradlittlemips  -r -o lib/built-in.o lib/kref.o lib/crc-ccitt.o lib/zlib_inflate/built-in.o lib/zlib_deflate/built-in.o
