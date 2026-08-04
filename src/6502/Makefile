VERSION=1.0.2
CFLAGS=-Wall -O2 -DVERSION=\"$(VERSION)\"

all:			aambox6502 aambox_frontend.bin c64_crunched.bin c64_drivecode.bin c64_loader.prg a2.system

clean:
			rm -rf aambox6502 cruncher labels mkfont
			rm -f a2_frontend.bin a2.labels a2.system

aambox6502:		aambox6502.c fake6502.c

aambox_frontend.bin:	aambox_frontend.s engine.s
			xa -l aambox.labels -o $@ $< -DRAMTOP=49152

c64_frontend.bin:	c64_frontend.s engine.s font.bin
			xa -l labels -o $@ $< -DVERSION=\"$(VERSION)\"

font.bin:		fontdef.txt mkfont
			./mkfont <$< >$@

c64_drivecode.bin:	c64_drivecode.s
			xa -o $@ $<

c64_loader.prg:		c64_loader.s
			xa -o $@ $<

decruncher.bin:		decruncher.s
			xa -o $@ $<

c64_crunched.bin:	c64_frontend.bin decruncher.bin cruncher
			./cruncher $< decruncher.bin >$@

a2_frontend.bin:	a2_frontend.s engine.s
			xa -l a2.labels -o $@ $< -DUNDO=1 -DSAVERESTORE=0 -DVERSION=\"$(VERSION)\"

a2.system:		a2_frontend.bin
			cat a2_frontend.bin >$@

.PHONY: all clean
