.PHONY: xmcism clean

xmcism: 
	macro11 xmcism.mac -o xmcism.obj
	obj2bin.pl --rt11 --binary --bytes=100000 --outfile=- xmcism.obj | tail -c +7 | head -c -8 > autorun.bin

clean:
	rm -f xmcism.obj
	rm -f autorun.bin
