.PHONY: all clean

all: 
	macro11 cartcrash.mac -o cartcrash.obj
	obj2bin.pl --rt11 --binary --bytes=100000 --outfile=- cartcrash.obj | tail -c +7 | head -c -8 > cartcrash.bin

clean:
	rm -f cartcrash.obj
	rm -f cartcrash.bin
