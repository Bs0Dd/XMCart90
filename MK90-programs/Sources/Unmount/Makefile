.PHONY: xmumount clean

xmumount:
	macro11 xmumount.mac -o xmumount.obj
	obj2bin.pl --rt11 --binary --bytes=100000 --outfile=- xmumount.obj | tail -c +7 | head -c -8 > xmumount.bin
	macro11 xmucreat.mac -o xmucreat.obj
	obj2bin.pl --rt11 --binary --bytes=100000 --outfile=- xmucreat.obj | tail -c +7 | head -c -8 > xmucreat.bin
	cat xmumount.bin >> xmucreat.bin

clean:
	rm -f xmumount.obj
	rm -f xmumount.bin
	rm -f xmucreat.obj
	rm -f xmucreat.bin
