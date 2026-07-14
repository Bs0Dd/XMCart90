.PHONY: xmcman clean

xmcman: 
	macro11 xmcman.mac -o xmcman.obj
	obj2bin.pl --rt11 --binary --bytes=100000 --outfile=- xmcman.obj | tail -c +7 | head -c -8 > rawmem.bin
	# Pack for SMP image - remove 1KB reserved block
	dd if=rawmem.bin of=autorun.bin bs=1 count=3072
	dd if=rawmem.bin of=autorun.bin bs=1 skip=4096 seek=3072
	rm -f rawmem.bin

clean:
	rm -f xmcman.obj
	rm -f rawmem.bin
	rm -f autorun.bin
