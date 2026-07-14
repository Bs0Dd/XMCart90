.PHONY: smpman clean

smpman: 
	macro11 smpman.mac -o smpman.obj
	obj2bin.pl --rt11 --binary --bytes=100000 --outfile=- smpman.obj | tail -c +7 | head -c -8 > smpman.bin

clean:
	rm -f smpman.obj
	rm -f smpman.bin
