; ProDOS SYS relocator for the Apple II
; Aa-machine frontend.
;
; Designed for the xa65 assembler.
;
; ProDOS loads a SYS file at $2000 and jumps
; there, but the interpreter has to run at
; $0800 -- leaving it at $2000 costs 6 kB of
; page cache, which is more than a large
; story can spare.
;
; This stub is padded to exactly one page, so
; the payload it was concatenated with starts
; at $2100.  It installs a small mover at
; $0300 -- outside both the source and the
; destination range -- and lets that do the
; work.  The mover only ever addresses zero
; page absolutely and branches relatively, so
; it needs no relocation of its own.
;
; PAYLOADPAGES must be defined on the command
; line; the Makefile computes it from the
; size of the assembled payload.

	* = $2000

	sei
	cld

	ldx	#moverlen-1
copy
	lda	mover,x
	sta	$0300,x
	dex
	bpl	copy

	jmp	$0300

mover
	.(
	lda	#$00
	sta	$00
	lda	#$21
	sta	$01		; source = $2100

	lda	#$00
	sta	$02
	lda	#$08
	sta	$03		; dest = $0800

	ldx	#PAYLOADPAGES
pageloop
	ldy	#0
byteloop
	lda	($00),y
	sta	($02),y
	iny
	bne	byteloop

	inc	$01
	inc	$03
	dex
	bne	pageloop

	jmp	$0800
	.)
moverlen = *-mover

	.dsb	$2100-*,0
