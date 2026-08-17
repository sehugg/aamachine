; A2 SmartPort boot loader ("sboot").
; Designed for the xa65 assembler.
;
; Replaces ProDOS on an 800k (or bigger) SmartPort volume.  Block 0 is
; the classic boot0 signature -- format byte, then SEC/BCS+3/JMP so a
; format-sniffing tool sees the same four bytes ProDOS's own boot0 uses
; -- followed by code that finds the slot from the boot ROM handoff,
; verifies the card answers the SmartPort ROM signature, and reads
; block 1 (stage 2) to $0a00.
;
; Stage 2 is an ordinary ProDOS volume-directory walk -- find AAM.SYSTEM
; by name in the root directory (chasing the directory chain if it is
; not in the first block), then load its data blocks -- seedling,
; sapling or tree, whichever it is -- straight to $2000, exactly where
; ProDOS would have put it.  AAM.SYSTEM does not need to sit at any
; particular block or be contiguous; the directory entry says where it
; is.  Once loaded, jump to $2003 (raw_entry in a2_frontend.s), the
; same entry 0boot uses, so the interpreter knows there is no ProDOS
; underneath.
;
; SmartPort calling convention (see e.g. 1000bit.it's Apple technote
; archive, smpt/tn.smpt.2 -- "SmartPort Technical Note #2")
;
;   Boot ROM hands off to $0801 with X = slot * 16.
;   $Cn00 is the slot's own I/O ROM.  A block device answers
;     $Cn01=$20, $Cn03=$00, $Cn05=$03, $Cn07=$00 (SmartPort; nonzero
;     at $Cn07 means "generic ProDOS block device", no raw SmartPort
;     calls).
;   $Cn00 + [$CnFF] is the ProDOS-style entry point; 3 bytes further in
;     is the raw SmartPort entry, called as
;       jsr entry
;       .byte cmd
;       .word paramlist
;     which returns with carry set and an error code in A on failure.
;   READ BLOCK (cmd=1) parameter list -- count=3, unit, buffer ptr (2
;     bytes), block # (3 bytes, little-endian).

	COUT		= $fded

	; zero page scratch -- nothing else is running yet, so any of it
	; is free to use.
	ptr		= $f0	; 2 bytes, generic indirect pointer
	cn_hi		= $f2	; 1 byte, $c0 + slot
	blknum		= $f3	; 3 bytes, block # for do_read
	storage		= $f6	; 1 byte, found entry's storage type
	key		= $f7	; 2 bytes, found entry's key block
	dest		= $f9	; 2 bytes, next load address
	idxblk		= $fb	; 2 bytes, index block # to walk
	tmp_y		= $fd	; 1 byte, saved loop index across a call
	msgptr		= $fe	; 2 bytes, error message pointer

	VOLUME_DIR_BLOCK = 2
	ENTRIES_PER_BLOCK = 13
	ENTRY_LENGTH	= $27
	namelen		= 10		; strlen("AAM.SYSTEM")

	dirbuf		= $1000		; 512 bytes -- also doubles as
	masterbuf	= $1000		; the tree's master index once the
					; directory search is done with it
	idxbuf		= $1200		; 512 bytes

; =====================================
; Block 0 -- stage 1
; =====================================

	* = $0800

	.byt	1		; format marker, not executed (entry is $0801)
	sec			; $0801 -- entry point, X = slot*16
	bcs	cont		; always taken -- skips the jmp below
	jmp	cont		; dead code, present for format-sniffing tools

cont
	.(
	txa
	lsr
	lsr
	lsr
	lsr			; a = slot number, 0-7
	ora	#$c0
	sta	cn_hi		; cn_hi = $c0 + slot
	sta	ptr+1
	lda	#0
	sta	ptr
	jsr	$fc58		; HOME

	; verify the SmartPort ROM signature at $Cn01/03/05/07
	ldy	#1
	lda	(ptr),y
	cmp	#$20
	bne	notsp
	ldy	#3
	lda	(ptr),y
	bne	notsp
	ldy	#5
	lda	(ptr),y
	cmp	#$03
	bne	notsp
	ldy	#7
	lda	(ptr),y
;	bne	notsp

	; entry = $Cn00 + [$CnFF]; raw SmartPort entry is 3 bytes further in
	ldy	#$ff
	lda	(ptr),y
	clc
	adc	#3
	sta	docall+1
	lda	cn_hi
	sta	docall+2

	; read stage2blocks blocks starting at block 1 into $0a00
	lda	#1
	sta	blknum
	lda	#0
	sta	blknum+1
	sta	blknum+2
	lda	#<stage2start
	sta	parambuf
	lda	#>stage2start
	sta	parambuf+1
	ldx	#stage2blocks
loadstage2
	jsr	do_read
	bcs	diskerr
	inc	blknum
	lda	parambuf+1
	clc
	adc	#2
	sta	parambuf+1
	dex
	bne	loadstage2

	jmp	stage2start

notsp
	ldx	#<msg_notsp
	ldy	#>msg_notsp
	jmp	fail

diskerr
	ldx	#<msg_diskerr
	ldy	#>msg_diskerr
	jmp	fail
	.)

; input blknum (3 bytes) + parambuf (2 bytes), pre-set by the caller;
; output c = error
do_read
	lda	blknum
	sta	paramblk
	lda	blknum+1
	sta	paramblk+1
	lda	blknum+2
	sta	paramblk+2
docall
	jsr	$0000		; operand patched at boot to the raw
				; SmartPort entry
	.byt	$01		; READ BLOCK
	.word	paramlist
	rts

paramlist
	.byt	3
paramunit
	.byt	1
parambuf
	.word	0
paramblk
	.byt	0,0,0

; input x/y = message, low/high; never returns
fail
	.(
	stx	msgptr
	sty	msgptr+1
	ldy	#0
loop
	lda	(msgptr),y
	beq	halt
	jsr	COUT
	iny
	bne	loop
halt
	jmp	halt
	.)

msg_notsp
	.asc	"NOT A SMARTPORT DEVICE",0
msg_diskerr
	.asc	"DISK ERROR",0
msg_notfound
	.asc	"NO AAM.SYSTEM",0

aamname
	.asc	"AAM.SYSTEM"

; =====================================
; Block 1+ -- stage 2
; =====================================

	.dsb	$0a00-*,0	; pad block 0 out to exactly 512 bytes
stage2start
	.(
	lda	#VOLUME_DIR_BLOCK
	sta	blknum
	lda	#0
	sta	blknum+1
	sta	blknum+2
dirloop
	lda	#<dirbuf
	sta	parambuf
	lda	#>dirbuf
	sta	parambuf+1
	jsr	do_read
	bcc	dirok
	jmp	fail_disk
dirok
	lda	#<(dirbuf+4)
	sta	ptr
	lda	#>(dirbuf+4)
	sta	ptr+1
	ldx	#0
entryloop
	ldy	#0
	lda	(ptr),y
	and	#$0f
	cmp	#namelen
	bne	nextentry
cmploop
	iny
	lda	(ptr),y
	cmp	aamname-1,y
	bne	nextentry
	cpy	#namelen
	bne	cmploop
	jmp	found

nextentry
	clc
	lda	ptr
	adc	#ENTRY_LENGTH
	sta	ptr
	bcc	nocarry
	inc	ptr+1
nocarry
	inx
	cpx	#ENTRIES_PER_BLOCK
	bne	entryloop

	; not in this block -- chase the directory chain
	lda	dirbuf+2
	ora	dirbuf+3
	bne	haschain
	jmp	fail_notfound
haschain
	lda	dirbuf+2
	sta	blknum
	lda	dirbuf+3
	sta	blknum+1
	jmp	dirloop

found
	ldy	#0
	lda	(ptr),y
	lsr
	lsr
	lsr
	lsr
	sta	storage
	ldy	#$11
	lda	(ptr),y
	sta	key
	iny
	lda	(ptr),y
	sta	key+1

	lda	#0
	sta	dest
	lda	#$20
	sta	dest+1		; dest = $2000, where ProDOS would load it

	lda	storage
	cmp	#1
	beq	doseedling
	cmp	#2
	beq	dosapling
	cmp	#3
	beq	dotree
	jmp	fail_notfound	; can't happen -- not a file, or bad volume

doseedling
	lda	key
	sta	blknum
	lda	key+1
	sta	blknum+1
	lda	#0
	sta	blknum+2
	lda	dest
	sta	parambuf
	lda	dest+1
	sta	parambuf+1
	jsr	do_read
	bcc	seedok
	jmp	fail_disk
seedok
	jmp	loaded

dosapling
	lda	key
	sta	idxblk
	lda	key+1
	sta	idxblk+1
	jsr	walk_index
	jmp	loaded

dotree
	lda	key
	sta	blknum
	lda	key+1
	sta	blknum+1
	lda	#0
	sta	blknum+2
	lda	#<masterbuf
	sta	parambuf
	lda	#>masterbuf
	sta	parambuf+1
	jsr	do_read
	bcc	treeok
	jmp	fail_disk
treeok
	ldy	#0
treeloop
	sty	tmp_y
	lda	masterbuf,y
	sta	idxblk
	lda	masterbuf+256,y
	sta	idxblk+1
	ora	idxblk
	beq	loaded		; both zero -- end of tree
	jsr	walk_index
	ldy	tmp_y
	iny
	beq	loaded		; wrapped past 255 -- exhausted the master
	jmp	treeloop

loaded
	jmp	$2003		; raw_entry -- no ProDOS underneath us

fail_disk
	ldx	#<msg_diskerr
	ldy	#>msg_diskerr
	jmp	fail
fail_notfound
	ldx	#<msg_notfound
	ldy	#>msg_notfound
	jmp	fail
	.)

; input idxblk (2 bytes); reads it into idxbuf, then reads every
; nonzero (lo,hi) block pair in it to (dest), advancing dest by $200
; each time.  A zero pair, or exhausting all 256 entries, ends it.
walk_index
	.(
	lda	idxblk
	sta	blknum
	lda	idxblk+1
	sta	blknum+1
	lda	#0
	sta	blknum+2
	lda	#<idxbuf
	sta	parambuf
	lda	#>idxbuf
	sta	parambuf+1
	jsr	do_read
	bcs	fail_disk

	ldy	#0
loop
	sty	tmp_y
	lda	idxbuf,y
	sta	blknum
	lda	idxbuf+256,y
	sta	blknum+1
	ora	blknum
	beq	done
	lda	#0
	sta	blknum+2
	lda	dest
	sta	parambuf
	lda	dest+1
	sta	parambuf+1
	jsr	do_read
	bcs	fail_disk
	lda	dest+1
	clc
	adc	#2
	sta	dest+1
	ldy	tmp_y
	iny
	bne	loop
done
	rts

fail_disk
	ldx	#<msg_diskerr
	ldy	#>msg_diskerr
	jmp	fail
	.)

stage2end
stage2blocks = (stage2end - stage2start + 511) >> 9
