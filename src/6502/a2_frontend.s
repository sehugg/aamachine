; Apple II (ProDOS) Aa-machine frontend.
; Designed for the xa65 assembler.
;
; ProDOS Version:
;
; The machine is identified at boot from the
; ProDOS global page, and the screen width,
; the character set and the use of auxiliary
; memory follow from that.
;
;  Apple ][ / ][+	40 columns, upper case
;  //e / //c / IIgs	80 columns if an
;			80-column card is
;			present, 40 otherwise
;  128 kB machines	undo ring in aux RAM
;
; Screen output is delegated to the Monitor
; ROM, and to the //e 80-column firmware when
; that is active.  Keyboard input is NOT
; delegated, because RDKEY and KEYIN increment
; RNDL/RNDH at $4e/$4f, which alias the
; engine's rsim register.
;
; Storage is ProDOS 8 via the MLI.  ProDOS
; lives in the language card; system programs
; run with ROM banked in and trampoline
; through the $bf00 global page, so $fded and
; the MLI are both available at all times.
;
; For standard 143,360 byte floppy disks:
; You can put the entire story file into a single
; file called STORY on the main disk, but this limits
; you to about 100 KB size story files.
; On Apple IIGS you can use 800 KB disk images, and on
; emulators you can use up to 32 MB.
;
; ProDOS isn't very memory-efficient, as it takes up the
; entire 16 KB language card and 1 KB of RAM per open file.
; Many games require too much heap space and will crash.
;
; Main memory map (ProDOS)
; =====================================
;  0000 - 001f	frontend zero page
;  0020 - 002b	Monitor text variables
;  002c - 0031	frontend zero page
;  0032 - 0039	Monitor text variables
;  003a - 003f	frontend and disk driver temporaries
;  0040 - 00ff	engine zero page
;  0100 - 01ff	stack
;  0200 - 02ff	frontend buffers and MLI
;		parameter blocks
;  0300 - 03ff	aux page cache tables
;  0400 - 07ff	text page 1
;  0800 -~4000	interpreter code
; ~4000 -~4800	init code, reclaimed as cache
; ~4000 - baff	page buffers / dynamic data
;  bb00 - beff	ProDOS file buffer
;  bf00 - bfff	ProDOS global page
;  d000 - ffff	ProDOS 8 (language card)
;
; Auxiliary memory, when there is any
; =====================================
;  0400 - 07ff	80-column text page
;  0800 - 87ff  aux page cache
;  8800 - beff	aux undo ring
;
; RWTS18 Version (TODO):
;
; This version will open up the 16K of language card memory,
; making larger-footprint games possible. It uses the
; RWTS18 disk format, giving you 161,280 bytes of disk.
;
; Main memory map (RWTS18)
; =====================================
;  0800 -~1000	interpreter code
;  1000 - bfff  page buffers / dynamic data
;  d000 - ffff  interpreter code
;
; TODO The 80STORE soft switch remaps $0400-$07ff
; and $2000-$3fff, so staying above both

; whatever the video firmware has left the
; switches set to?

; DEFWIDTH only feeds initengine0; coldstart
; overwrites screenw with the detected width
; immediately afterwards.
;
; See also:
;   https://prodos8.com/docs/techref/memory-use/
;   https://savagetaylor.com/til/TA33130.html
;
; TODO
; - save/load
; - Impossible Stairs .aastory doesn't work
; - inverse mode sometimes seems confused in 80 column
;   (but styles are turned off for now except for status)
; - 2-disk mode - "STORY-xx-yy"
; - use the packer instead of boot mover
; - RWTS18 non-ProDOS verison using lang card RAM
; - use AUX memory more

DEFWIDTH	= 80

PREXTRA		= 2
PRSHIFT		= 0
HAVE_QUIT	= 0	; we don't have BASIC.SYSTEM
HAVE_STATUS	= 1
HAVE_STYLE	= 0

TRACE_INST	= 0
TRACE_STORE	= 0

; ---- Monitor entry points ----

COUT		= $fded
COUT1		= $fdf0
VIDOUT		= $fbfd
HOME		= $fc58
VTAB		= $fc22
SETTXT		= $fb39
PRBYTE		= $fdda
IDROUTINE	= $fe1f		; rts, except on a IIgs
AUXMOVE		= $c311
SLOT3		= $c300

CSW		= $36
A1		= $3c
A2		= $3e
A4		= $42

WNDLFT		= $20
WNDWDTH		= $21
WNDTOP		= $22
WNDBTM		= $23
CH		= $24
OURCH		= 1403
CV		= $25
INVFLG		= $32

KBD		= $c000
KBDSTRB		= $c010
CLR80STORE	= $c000
CLR80COL	= $c00c
ALTCHRSET_OFF	= $c00e
ALTCHRSET_ON	= $c00f

; ---- auxiliary memory ----

AUXCACHESTART	= $800
AUXCACHEPAGES	= $80

AUXCACHELO	= $300		; virtual page # for each aux cache page
AUXCACHEHI	= $380

AUXUNDOLO	= $8800
AUXUNDOHI	= $beff		; TODO inclusive?

; ---- frontend zero page ----

ioparam		= $06	; word, used by the engine
wrappos		= $08
xpos		= $09
pendspc		= $0a
f_temp		= $0b
f_temp2		= $0c
coutx		= $0d
couty		= $0e
tr0		= $0f
tr1		= $10
stxoffs		= $11
statush		= $12
cury		= $13
seed		= $14	; word
rp_page		= $16
savedch		= $17
savedcv		= $18
strptr		= $19	; word
strow		= $1b
scrw		= $1c	; 40 or 80
col80		= $1d	; $80 if the //e 80-column
			; firmware is driving COUT
foldup		= $1e	; $80 to fold output to
			; upper case
auxram		= $1f	; $80 if aux RAM is usable

; ---- frontend buffers ----

; wrapbuf doubles as the buffer for status
; row 0, exactly as on the C64. io_sprepare
; clears it, io_sputc fills it via stxoffs,
; and io_scommit copies it to the screen.
; The two uses never interleave, because
; stflag routes every character to exactly
; one of io_mputc and io_sputc.

wrapbuf		= $0200	; 80 bytes
zpbuf		= $0250	; 32, MLI zero-page save

auxregsbuf	= $0270	; AUXMOVE zero-page save

; Undo ring bookkeeping.  Snapshots are all
; the same size, so the ring is a plain array
; of slots between AUXUNDOLO and AUXUNDOHI.

u_next		= $0278	; word, where the next
			; snapshot goes
u_old		= $027a	; word, oldest snapshot
u_last		= $027c	; word, highest slot start
u_size		= $027e	; word, snapshot size
u_tmp		= $0280	; word, scratch
u_end		= $0282	; word, scratch
u_max		= $0284	; slot capacity, max 255
u_count		= $0285	; snapshots stored
u_ready		= $0286	; nonzero once sized
u_dir		= $0287	; $80 = main to aux

#if PRODOS || PRORWTS
MACHID		= $bf98
#endif

#if PRODOS

; MLI parameter blocks have to survive into
; the run, so they live down here rather than
; in the init segment.

p_open		= $0288	; 6 bytes
p_read		= $028e	; 8 bytes
p_mark		= $0296	; 5 bytes
p_quit		= $029b	; 7 bytes

MLI		= $bf00
DEVADR		= $bf10
DEVCNT		= $bf31
DEVLST		= $bf32
FILEBUF		= $bb00

#endif

#if PRORWTS
next_rpagelo	= $029e
next_rpagehi	= $029f
rwregsbuf	= $02a0	; PRORWTS zero-page save

rwregs_first	= $3b
rwregs_last	= $67
rwregs_count	= rwregs_last - rwregs_first + 1

prorwts_zp = $50
prorwts_status	= prorwts_zp+0
prorwts_auxreq	= prorwts_zp+1
prorwts_sizelo	= prorwts_zp+2
prorwts_sizehi	= prorwts_zp+3
prorwts_reqcmd	= prorwts_zp+4
prorwts_ldrlo	= prorwts_zp+5
prorwts_ldrhi	= prorwts_zp+6
prorwts_namlo	= prorwts_zp+7
prorwts_namhi	= prorwts_zp+8

prorwts_reloc    = $d000
prorwts_rdwrpart = prorwts_reloc + 0
prorwts_opendir	 = prorwts_reloc + 3
#endif

; ProDOS loads a SYS file at $2000 and jumps
; there, but the interpreter has to run at
; $0800 -- leaving it at $2000 costs 6 kB of
; page cache, which is more than a large
; story can spare.

	* = $2000
#if PRODOS || PRORWTS

; First, we move the mover to $300...

	.(
	sei
	cld
	ldx	#moverlen
copy
	lda	mover,x
	sta	$0300,x
	dex
	bpl	copy

	jmp	$0300
	.)

; Then we run the mover at $300
mover
	.(
	lda	#<bootcode
	sta	$00
	lda	#>bootcode
	sta	$01		; source = bootcode

	lda	#$00
	sta	$02
	lda	#$08
	sta	$03		; dest = $0800

	ldx	#SAFEPG-$8
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
	.)

	jmp	coldstart

bootcode = *
moverlen = * - mover

#endif

; =====================================
; Main code
; =====================================

	* = $0800

do_foldup
	.(
	bit	foldup
	bpl	nofold

	cmp	#'a'
	bcc	nolower

	cmp	#'z'+1
	bcs	nolower

	and	#$df
nolower
nofold
	rts
	.)

io_mputc
	; input a = char

	.(
	cmp	#$20
	beq	space
	bcc	ignore

	cmp	#$80
	bcs	extended
plain
	jsr	do_foldup
	ldx	xpos
	cpx	scrw
	bcs	wrap
postwrap
	ldx	wrappos
	sta	wrapbuf,x
	inc	wrappos
	inc	xpos

	cmp	#'-'
	beq	dash
ignore
	rts
dash
	jmp	io_mflush
space
	jsr	io_mflush
	inc	pendspc
	inc	xpos
	rts
wrap
	pha
	jsr	io_mline_raw
	ldx	wrappos
	stx	xpos
	jsr	io_mflush
	pla
	jmp	postwrap
extended
	jsr	translit

	lda	tr0
	jsr	io_mputc	; always < $80

	lda	tr1
	beq	done

	jmp	io_mputc	; always < $80
done
	rts
	.)

io_mflush
	.(
	ldx	pendspc
	beq	nospc
spcloop
	lda	#$a0
	jsr	cout
	dex
	bne	spcloop

	stx	pendspc
nospc
	ldx	#0
	cpx	wrappos
	beq	done
loop
	lda	wrapbuf,x
	ora	#$80
	jsr	cout
	inx
	cpx	wrappos
	bne	loop
done
	lda	#0
	sta	wrappos
	rts
	.)

io_mline
	jsr	io_mflush
io_mline_raw
	.(
	ldx	xpos
	dex
	cpx	scrw	; signed cmp
	bpl	nocr	; don't CR if we filled entire line
	lda	#$8d
	jsr	cout
nocr
	lda	#0
	sta	xpos
	sta	pendspc

	; wrappos is deliberately preserved --
	; word-wrap path carries a pending word
	; across the line break.

	ldx	cury
	cpx	#23
	bcs	nobump

	inc	cury
nobump
	rts
	.)

io_mclear
	.(
	lda	#0
	sta	xpos
	sta	pendspc
	sta	wrappos

	jsr	clrwin		; clears the window
				; only, so the status
				; area survives
	lda	statush
	sta	cury
	rts
	.)

	; TODO some bugs in 80-column mode
io_mstyle
	rts
	; input a = style bits
	/*
	.(
	pha
	jsr	io_mflush
	pla
	and	#6		; bold and italic
	beq	normal

	jmp	set_inverse
normal
	jmp	set_normal
	.)
	*/

; TODO use MouseText?
io_mprogress
	; input x = progress
	; input y = total
	; where 0 <= x <= y
	; and y =
	; (width << PRSHIFT) - PREXTRA

	.(
	jsr	io_mflush

	stx	f_temp
	sty	f_temp2

	lda	#$db		; '['
	jsr	cout

	ldx	#0
loop
	lda	#$a0
	cpx	f_temp
	bcs	past

	lda	#$bd		; '='
past
	jsr	cout
	inx
	cpx	f_temp2
	bcc	loop

	lda	#$dd		; ']'
	jmp	cout
	.)

; =====================================
; Status bar area
; =====================================

; The status area occupies the top statush
; rows.  It is kept outside the text window
; (WNDTOP = statush) so that every scroll
; performed by COUT leaves it alone.  To
; write into it, the window is opened up
; temporarily and closed again by io_scommit.

io_sprepare
	; input a = height

	.(
	sta	statush

	lda	CH
	bit	col80
	bpl	noourch
	lda	OURCH
noourch
	sta	savedch
	lda	cury
	sta	savedcv

	; clear the status rows by pointing the
	; window at them and homing

	lda	#0
	sta	WNDTOP
	lda	statush
	sta	WNDBTM
	jsr	clrwin

	lda	statush
	sta	WNDTOP
	lda	#24
	sta	WNDBTM

	; blank the row 0 buffer

	ldy	scrw
	dey
	lda	#$20
clr0
	sta	wrapbuf,y
	dey
	bpl	clr0

	; put the cursor back, clamped into the
	; main area

	lda	savedcv
	cmp	statush
	bcs	keep

	lda	statush
	ldx	#0
	stx	savedch
keep
	sta	cury
	tay
	ldx	savedch
	jsr	gotoxy
	rts
	.)

io_slocate
	; input x = column
	; input y = row
	;
	; Row 0 is buffered in wrapbuf and
	; committed by io_scommit.  Rows 1 and up
	; are written straight to the screen.

	.(
	stx	stxoffs

	dey
	bpl	notrow0

	lda	#$ff
	sta	strow
	rts
notrow0
	iny
	sty	strow

	; open the window over the status area so
	; that gotoxy can reach it

	lda	#0
	sta	WNDTOP
	ldx	stxoffs
	jsr	gotoxy
	rts
	.)

io_sputc
	; input a = char

	.(
	jsr	do_foldup
	ldy	strow
	bpl	screen

	ldy	stxoffs
	cpy	scrw
	bcs	skip

	sta	wrapbuf,y
	inc	stxoffs
skip
	rts
screen
	ora	#$80
	jsr	cout
	inc	stxoffs
	rts
	.)

io_sprogress
	; input x = progress
	; input y = total
	; where 0 <= x <= y
	; and y =
	; (width << PRSHIFT) - PREXTRA

	.(
	stx	f_temp
	sty	f_temp2

	lda	#'['
	jsr	io_sputc

	ldx	#0
loop
	lda	#$20
	cpx	f_temp
	bcs	past

	lda	#'='
past
	jsr	io_sputc
	inx
	cpx	f_temp2
	bcc	loop

	lda	#']'
	jmp	io_sputc
	.)

io_scommit
	.(
	lda	#0
	sta	WNDTOP

	ldx	#0
	ldy	#0
	jsr	gotoxy
	jsr	set_inverse

	ldx	#0
loop
	lda	wrapbuf,x
	ora	#$80
	jsr	cout
	inx
	cpx	scrw
	bcc	loop

	lda	statush
	sta	WNDTOP

	ldx	xpos
	ldy	cury
	jsr	gotoxy
	jsr	set_normal	; do we have to restore old value?
	rts
	.)

; =====================================
; Input
; =====================================

io_getc
	; output a = char

	.(
	jsr	io_mflush
	jmp	getkey
	.)

io_gets
	; input ioparam = buffer
	; (room for 64 chars)
	; output y = length

	.(
	jsr	io_mflush

	ldy	#0
loop
	sty	f_temp2
	jsr	getkey
	ldy	f_temp2

	cmp	#13
	beq	done

	cmp	#8
	beq	backspace

	cmp	#$7f
	beq	backspace

	cmp	#$20
	bcc	loop

	cpy	#64
	bcs	loop

	sta	(ioparam),y
	iny

	ora	#$80
	jsr	cout
	inc	xpos
	jmp	loop
backspace
	cpy	#0
	beq	loop

	dey
	lda	#$88
	jsr	cout
	lda	#$a0
	jsr	cout
	lda	#$88
	jsr	cout
	dec	xpos
	jmp	loop
done
	sty	f_temp2
	jsr	io_mline_raw
	ldy	f_temp2
	rts
	.)

getkey
	; output a = key, high bit clear
	;
	; A block cursor is drawn while waiting.
	; The wait loop also stirs the random
	; seed, which is the only entropy source
	; on a machine with no clock.

	.(
	jsr	set_inverse
	lda	#$a0
	jsr	cout
	lda	#$88
	jsr	cout
	jsr	set_normal
wait
	inc	seed+0
	bne	nohi

	inc	seed+1
nohi
	lda	KBD
	bpl	wait

	sta	KBDSTRB
	and	#$7f
	pha

	lda	#$a0
	jsr	cout
	lda	#$88
	jsr	cout

	pla
	rts
	.)

; =====================================
; Character translation
; =====================================

translit
	; input a = char code, >= $80
	; output tr0, tr1
	; tr1 is zero for a single character

	.(
	jsr	lookupchar
	bcs	unknown

	ldy	#2
	lda	(ioparam),y
	bne	unknown		; outside the BMP

	iny
	lda	(ioparam),y
	sta	f_temp
	iny
	lda	(ioparam),y
	sta	f_temp2

	ldx	#NTRANS-1
loop
	lda	trhi,x
	cmp	f_temp
	bne	next

	lda	trlo,x
	cmp	f_temp2
	beq	found
next
	dex
	bpl	loop
unknown
	lda	#'?'
	sta	tr0
	lda	#0
	sta	tr1
	rts
found
	lda	trc0,x
	sta	tr0
	lda	trc1,x
	sta	tr1
	rts
	.)

; =====================================
; Screen primitives
; =====================================

cout
	; input a = char, high bit set
	;
	; COUT and the 80-column firmware both
	; clobber x and y.

	.(
	stx	coutx
	sty	couty
	jsr	COUT
	ldx	coutx
	ldy	couty
	rts
	.)

clrwin
	; clear the text window and home the
	; cursor within it

	.(
	bit	col80
	bmi	firmware

	jmp	HOME
firmware
	lda	#$8c		; ctrl-L
	jmp	cout
	.)

gotoxy
	; input x = column, y = row

	.(
	stx	CH
; https://www.atarimagazines.com/compute/issue76/Feedback_3.php
	stx	OURCH	; synchronize CH and OURCH
	sty	CV
	jmp	VTAB
	.)

set_inverse
	.(
	bit	col80
	bmi	firmware

	lda	#$3f
	sta	INVFLG
	rts
firmware
	lda	#$8f		; ctrl-O
	jmp	cout
	.)

set_normal
	.(
	bit	col80
	bmi	firmware

	lda	#$ff
	sta	INVFLG
	rts
firmware
	lda	#$8e		; ctrl-N
	jmp	cout
	.)

; =====================================
; System
; =====================================

;; TODO ProDOS thinks the system is corrupted?
;; "RESTART SYSTEM-$0B"
io_quit
	.(
#if PRODOS
	jsr	set_normal
	jsr	io_mline

	lda	#$65		; QUIT
	ldx	#<p_quit
	ldy	#>p_quit
	jsr	mlicall
#endif
halt
	jmp	halt
	.)

io_restart
	; The engine reloads the story itself.
	; All the frontend has to do is throw
	; away the undo history.

	.(
	lda	#0
	sta	u_count

	lda	#<AUXUNDOLO
	sta	u_next+0
	sta	u_old+0
	lda	#>AUXUNDOLO
	sta	u_next+1
	sta	u_old+1
	rts
	.)

io_random
	; output x = lsb, y = msb
	; range is 0..7fff

	.(
	lsr	seed+1
	ror	seed+0
	bcc	noeor

	lda	seed+1
	eor	#$b4
	sta	seed+1
noeor
	ldx	seed+0
	lda	seed+1
	and	#$7f
	tay
	rts
	.)

io_save
	; input x = size lsb
	; input y = size msb
	; data at SAVEADDR
	; output c = success

	.(
	; TODO
	clc
	rts
	.)

io_load
	; verify AASV header, 12 bytes
	; if ok, put file at SAVEADDR
	; and set c
	; otherwise, clear c

	.(
	; TODO
	clc
	rts
	.)

; =====================================
; Undo, in auxiliary memory
; =====================================

; Snapshots go into a ring of equal-sized
; slots between AUXUNDOLO and AUXUNDOHI.  The engine
; asks for the same size every time, so the
; layout is computed once, on the first save.

io_undosupp
	; output c = undo supported

	.(
	bit	auxram
	bpl	no

	sec
	rts
no
	clc
	rts
	.)

io_saveundo
	; input x = byte size lsb
	; input y = byte size msb
	; input ioparam = start of data
	; output c = success

	.(
	bit	auxram
	bpl	no

	jsr	undoprep
	bcc	no

	lda	u_next+0
	sta	u_tmp+0
	lda	u_next+1
	sta	u_tmp+1
	lda	#$80
	sta	u_dir
	jsr	undomove

	; a full ring forgets its oldest entry

	lda	u_count
	cmp	u_max
	bcc	room

	lda	u_old+0
	sta	u_tmp+0
	lda	u_old+1
	sta	u_tmp+1
	jsr	undoadv
	lda	u_tmp+0
	sta	u_old+0
	lda	u_tmp+1
	sta	u_old+1
	jmp	bump
room
	inc	u_count
bump
	lda	u_next+0
	sta	u_tmp+0
	lda	u_next+1
	sta	u_tmp+1
	jsr	undoadv
	lda	u_tmp+0
	sta	u_next+0
	lda	u_tmp+1
	sta	u_next+1

	sec
	rts
no
	clc
	rts
	.)

io_loadundo
	; input x = byte size lsb
	; input y = byte size msb
	; input ioparam = dest address
	; output a = status
	;	0 ok
	;	1 no more entries
	;	2 error

	.(
	bit	auxram
	bpl	err

	jsr	undoprep
	bcc	err

	lda	u_count
	beq	empty

	; step back onto the newest snapshot

	lda	u_next+0
	sta	u_tmp+0
	lda	u_next+1
	sta	u_tmp+1
	jsr	undoback
	lda	u_tmp+0
	sta	u_next+0
	lda	u_tmp+1
	sta	u_next+1

	lda	#0
	sta	u_dir
	jsr	undomove

	dec	u_count
	lda	#0
	rts
empty
	lda	#1
	rts
err
	lda	#2
	rts
	.)

undoprep
	; input x = byte size lsb
	; input y = byte size msb
	; output c = the ring holds at least one
	; snapshot of that size

	.(
	lda	u_ready
	beq	init

	cpx	u_size+0
	bne	init

	cpy	u_size+1
	bne	init
check
	lda	u_max
	beq	nofit

	sec
	rts
nofit
	clc
	rts
init
	jsr	undoinit
	jmp	check
	.)

undoinit
	; input x = byte size lsb
	; input y = byte size msb
	;
	; Walks the ring one slot at a time
	; rather than dividing.  It runs once.

	.(
	stx	u_size+0
	sty	u_size+1

	lda	#1
	sta	u_ready

	lda	#0
	sta	u_max
	sta	u_count

	lda	#<AUXUNDOLO
	sta	u_tmp+0
	sta	u_last+0
	sta	u_next+0
	sta	u_old+0
	lda	#>AUXUNDOLO
	sta	u_tmp+1
	sta	u_last+1
	sta	u_next+1
	sta	u_old+1
loop
	lda	u_tmp+0
	clc
	adc	u_size+0
	sta	u_end+0
	lda	u_tmp+1
	adc	u_size+1
	sta	u_end+1
	bcs	done

	; the slot has to end at or below AUXUNDOHI

	lda	#<AUXUNDOHI
	cmp	u_end+0
	lda	#>AUXUNDOHI
	sbc	u_end+1
	bcc	done

	ldx	u_max
	inx
	beq	done		; 255 slots is plenty

	stx	u_max

	lda	u_tmp+0
	sta	u_last+0
	lda	u_tmp+1
	sta	u_last+1

	lda	u_end+0
	sta	u_tmp+0
	lda	u_end+1
	sta	u_tmp+1
	jmp	loop
done
	rts
	.)

undoadv
	; input/output u_tmp = slot address

	.(
	lda	u_tmp+0
	clc
	adc	u_size+0
	sta	u_end+0
	lda	u_tmp+1
	adc	u_size+1
	sta	u_end+1

	; wrap once past the last slot

	lda	u_last+0
	cmp	u_end+0
	lda	u_last+1
	sbc	u_end+1
	bcs	store

	lda	#<AUXUNDOLO
	sta	u_end+0
	lda	#>AUXUNDOLO
	sta	u_end+1
store
	lda	u_end+0
	sta	u_tmp+0
	lda	u_end+1
	sta	u_tmp+1
	rts
	.)

undoback
	; input/output u_tmp = slot address

	.(
	lda	u_tmp+0
	sec
	sbc	u_size+0
	sta	u_end+0
	lda	u_tmp+1
	sbc	u_size+1
	sta	u_end+1
	bcc	wrap

	lda	u_end+0
	cmp	#<AUXUNDOLO
	lda	u_end+1
	sbc	#>AUXUNDOLO
	bcs	store
wrap
	lda	u_last+0
	sta	u_end+0
	lda	u_last+1
	sta	u_end+1
store
	lda	u_end+0
	sta	u_tmp+0
	lda	u_end+1
	sta	u_tmp+1
	rts
	.)

swapauxregs
	.(
	ldx	#7
save
	lda	A1,x
	pha
	lda	auxregsbuf,x
	sta	A1,x
	pla
	sta	auxregsbuf,x
	dex
	bpl	save
	rts
	.)

#if PRORWTS
swaprwregs
	.(
	ldx	#rwregs_count-1
save
	lda	rwregs_first,x
	pha
	lda	rwregsbuf,x
	sta	rwregs_first,x
	pla
	sta	rwregsbuf,x
	dex
	bpl	save
	rts
	.)
#endif

undomove
	; input u_tmp = address in aux memory
	; input ioparam = address in main memory
	; input u_dir bit 7 set = main to aux
	;
	; The ROM move routine works through
	; $3c-$43, and $40-$43 belong to the
	; engine, so that range is saved.

	.(
	jsr	swapauxregs

	bit	u_dir
	bmi	toaux

	lda	u_tmp+0		; src in aux
	sta	A1+0
	lda	u_tmp+1
	sta	A1+1
	lda	ioparam+0	; dest in main
	sta	A4+0
	lda	ioparam+1
	sta	A4+1
	jmp	setend
toaux
	lda	ioparam+0	; src in main
	sta	A1+0
	lda	ioparam+1
	sta	A1+1
	lda	u_tmp+0		; dest in aux
	sta	A4+0
	lda	u_tmp+1
	sta	A4+1
setend
	; A2 = A1 + size - 1, inclusive

	lda	A1+0
	clc
	adc	u_size+0
	sta	A2+0
	lda	A1+1
	adc	u_size+1
	sta	A2+1

	lda	A2+0
	sec
	sbc	#1
	sta	A2+0
	lda	A2+1
	sbc	#0
	sta	A2+1

	lda	u_dir
	asl			; bit 7 to carry
	jsr	AUXMOVE

	jmp	swapauxregs
	.)

; =====================================
; Storage
; =====================================

io_readpage
	; input x = physical ram target page
	; input ioparam = virtual address 23..8, l-e
	; output a = physical ram target page

	.(
	stx	rp_page

	; do we have it in aux cache?
	bit	col80
	bpl	noloadfromaux
	; check aux cache table for virtual page #
	lda	ioparam
	and	#AUXCACHEPAGES-1
	tay
	lda	ioparam
	cmp	AUXCACHELO,y
	bne	noloadfromaux
	lda	ioparam+1
	cmp	AUXCACHEHI,y
	bne	noloadfromaux

	; we have it, transfer page from aux cache
	jsr	swapauxregs
	lda	#0
	sta	A1+0
	sta	A4+0
	tya			; y is preserved
	clc
	adc	#>AUXCACHESTART
	sta	A1+1		; src in aux
	sta	A2+1		; src end in aux
	lda	rp_page
	sta	A4+1		; dest in main
	; A2 = A1 + size - 1, inclusive
	lda	#$ff
	sta	A2+0		; src ending addr
	clc			; aux -> main
	jsr	AUXMOVE
	jsr	swapauxregs

	jmp	nosaveinaux	; page is loaded

noloadfromaux
	; byte position = virtual page << 8
#if PRODOS
	lda	#0
	sta	p_mark+2
	sta	p_read+2
	lda	ioparam
	sta	p_mark+3
	lda	ioparam+1
	sta	p_mark+4

	lda	rp_page
	sta	p_read+3

	lda	#$ce		; SET_MARK
	ldx	#<p_mark
	ldy	#>p_mark
	jsr	mlicall
	bcs	err

	lda	#$ca		; READ
	ldx	#<p_read
	ldy	#>p_read
	jsr	mlicall
	bcs	err
#endif

#if PRORWTS
	jsr	swaprwregs

; are we seeking backwards? if so rewind
	lda	ioparam+1
	cmp	next_rpagehi
	bcc	rewind
	lda	ioparam
	cmp	next_rpagelo
	bcc	rewind
; if last = current, don't even seek
	bne	norewind
	lda	ioparam+1
	cmp	next_rpagehi
	beq	noseek

pages = f_temp
; be kind, rewind
rewind
#ifdef DEBUG
	lda	#$41
	jsr	cout
#endif
	lda     #0
        sta     $5f ; blkidx
        sta     $63 ; blkofflo
        sta     $64 ; blkoffhi
        sta     $5b ; treeidx (TODO set B >> 8)
	sta	next_rpagelo
	sta	next_rpagehi
norewind
#ifdef DEBUG
	lda	#$42
	jsr	cout
#endif
	lda	ioparam
	sec
	sbc	next_rpagelo
	sta	prorwts_sizehi
#ifdef DEBUG
	jsr	PRBYTE
#endif
        lda     #0	; cmdseek
        sta     prorwts_reqcmd
        sta     prorwts_sizelo
        lda	$C08B		; lc_bank=1  (use $C083 for lc_bank=2) — read twice
        lda	$C08B
        jsr     prorwts_rdwrpart
        lda	$C081		; ROMIN
noseek
	lda     rp_page
	sta	prorwts_ldrhi
        lda     #1	;cmdread
        sta     prorwts_reqcmd
        sta     prorwts_sizehi
        lda     #0
        sta     prorwts_sizelo
	sta	prorwts_ldrlo
        lda	$C08B		; lc_bank=1  (use $C083 for lc_bank=2) — read twice
        lda	$C08B
        jsr     prorwts_rdwrpart
        lda	$C081		; ROMIN

	ldy	prorwts_status
	jsr	swaprwregs
	tya
	bne	err

	lda	ioparam
	clc
	adc	#1
	sta	next_rpagelo
	lda	ioparam+1
	adc	#0
	sta	next_rpagehi
#ifdef DEBUG
	lda	ioparam+1
	jsr	PRBYTE
	lda	ioparam
	jsr	PRBYTE
	lda	$5f - rwregs_first + rwregsbuf
	jsr	PRBYTE
	lda	#$a0
	jsr	COUT1
#endif
#endif

	; store in aux memory
	bit	col80
	bpl	nosaveinaux

	jsr	swapauxregs
	lda	#0
	sta	A1+0
	sta	A4+0
	lda	ioparam
	and	#AUXCACHEPAGES-1
	tax
	clc
	adc	#>AUXCACHESTART
	sta	A4+1		; dest in aux

	lda	ioparam
	sta	AUXCACHELO,x	; store virtual page # for this aux cache slot
	lda	ioparam+1
	sta	AUXCACHEHI,x

	lda	rp_page
	sta	A1+1		; src in main
	sta	A2+1		; src end in main
	; A2 = A1 + size - 1, inclusive
	lda	#$ff
	sta	A2		; src ending addr
	sec			; main -> aux
	jsr	AUXMOVE
	jsr	swapauxregs
;	lda	ioparam
;	jsr	PRBYTE
;	lda	ioparam+1
;	jsr	PRBYTE
;	lda	#$a0
;	jsr	COUT

nosaveinaux
	lda	rp_page
	rts
err
	jmp	diskerror
	.)

#if PRODOS
; =====================================
; ProDOS glue
; =====================================

mlicall
	; input a = command
	; input x/y = parameter block, l/h
	; output c = error, a = error code
	;
	; The engine owns zero page from $40 up
	; and ProDOS is documented to use part of
	; that range, so it is saved across every
	; call.  Every caller is disk-bound, so
	; the copy costs nothing in practice.

	.(
	sta	cmd
	stx	parm+0
	sty	parm+1

	ldx	#31
save
	lda	$40,x
	sta	zpbuf,x
	dex
	bpl	save

	jsr	MLI
cmd	.byt	0
parm	.word	0

	php
	pha

	ldx	#31
restore
	lda	zpbuf,x
	sta	$40,x
	dex
	bpl	restore

	pla
	plp
	rts
	.)

nodev
	; Stands in for the /RAM driver once the
	; RAM disk has been unhooked.  It has to
	; be resident, because ProDOS keeps the
	; pointer.

	lda	#$28		; no device connected
	sec
	rts
#endif

diskerror
	.(
	pha
	jsr	io_mline
	ldx	#<txt_diskerr
	ldy	#>txt_diskerr
	jsr	putstr
	pla
	jsr	PRBYTE	; print error code
	lda	rp_page
	jsr	PRBYTE	; print some debugging info
	lda	ioparam
	jsr	PRBYTE
#if PRODOS
	lda	p_read+3
	jsr	PRBYTE
	lda	p_read+2
	jsr	PRBYTE
#endif
halt
	jmp	halt
	.)

putstr
	; input x/y = string, l/h
	; plain ascii, nul terminated

	.(
	stx	strptr+0
	sty	strptr+1
	ldy	#0
loop
	sty	f_temp
	lda	(strptr),y
	beq	done

	cmp	#10
	bne	nonl

	jsr	io_mline
	jmp	next
nonl
	jsr	io_mputc
next
	ldy	f_temp
	iny
	bne	loop
done
	jmp	io_mflush
	.)

; =====================================
; Resident data
; =====================================

txt_diskerr
	.asc	"DiskErr:",0

; Transliteration table.  Text mode draws
; from the character ROM, so anything outside
; ASCII has to be approximated.  Codepoints
; are BMP only, and the replacement is at
; most two characters wide.

trhi
	.byt	$00,$00,$00,$00,$00,$00,$00,$00
	.byt	$00,$00,$00,$00,$00,$00,$00,$00
	.byt	$00,$00,$00,$00,$00,$00,$00,$00
	.byt	$00,$00,$00,$00,$00,$00
	.byt	$20,$20,$20,$20,$20,$20,$20,$20
trlo
	.byt	$a0,$ab,$bb,$c4,$c5,$c6,$c9,$d6
	.byt	$d8,$dc,$df,$e0,$e1,$e2,$e4,$e5
	.byt	$e6,$e7,$e8,$e9,$ea,$eb,$ee,$ef
	.byt	$f1,$f4,$f6,$f8,$fc,$fd
	.byt	$10,$13,$14,$18,$19,$1c,$1d,$26
trc0
	.byt	' ','"','"','A','A','A','E','O'
	.byt	'O','U','s','a','a','a','a','a'
	.byt	'a','c','e','e','e','e','i','i'
	.byt	'n','o','o','o','u','y'
	.byt	'-','-','-',$27,$27,$22,$22,'.'
trc1
	.byt	0,0,0,0,0,'E',0,0
	.byt	0,0,'s',0,0,0,0,0
	.byt	'e',0,0,0,0,0,0,0
	.byt	0,0,0,0,0,0
	.byt	0,0,'-',0,0,0,0,'.'

NTRANS = trlo-trhi

; =====================================
; Cold start
; =====================================

; This sequence has to be resident.
; initengine5 prefills the page cache
; starting at the init segment, so any
; caller living there would be overwritten
; while it ran.  initengine5 itself is
; resident for the same reason.

coldstart
	.(
#if PRORWTS
	jsr	prorwts2_init	; TODO check status?
	jsr	swaprwregs	; load prorwts registers into save bank
#endif

	jsr	initsystem

	jsr	initengine0

	; initengine0 has just set screenw from
	; DEFWIDTH; the real width is whatever
	; was detected.

	lda	scrw
	jsr	setwidth

	jsr	initengine1
	jsr	initengine2
	jsr	initengine3
	jsr	initengine4
	jsr	initengine5
	jmp	startengine
	.)

#include "engine.s"

; =====================================
; Initialization
; =====================================

; Everything from here on is overwritten by
; page buffers once the engine is running.

initsystem
	.(
	; Apple ][ boot doesn't clear engine zero page so we do it here
	lda	#0
	ldx	#$c0
clrlp
	sta	$3f,x
	dex
	bne	clrlp

	; clear the aux cache lookup table
	lda	#$ff
	ldx	#AUXCACHEPAGES
auxclrlp
	sta	AUXCACHEHI-1,x
	dex
	bne	auxclrlp

	lda	#0
	sta	wrappos
	sta	xpos
	sta	pendspc
	sta	statush
	sta	stxoffs
	sta	cury
	sta	strow
	sta	u_ready
	sta	u_count

	lda	#$e1
	sta	seed+0
	lda	#$27
	sta	seed+1

	jsr	detect
	jsr	setupvideo
#if PRODOS
	jsr	initparms
	jsr	removeram
#endif
	jsr	banner
	jmp	openstory
	.)

detect
	; ProDOS has already worked out what it
	; is running on, so ask it rather than
	; poking at soft switches.
	;
	; MACHID
	; (Bit 3 off) BITS 7,6- 00=II 01=II+ 10=IIe 11=///
	; (Bit 3 on) BITS 7,6- 00=NA 01=NA 10=//c 11=NA
	; BITS 5,4- 00=NA 01=48K 10=64K 11=128K
	; BIT 3 - Modifier for MACHID Bits 7,6.
	; BIT 1=1- 80 Column card

	.(
	lda	#0
	sta	col80
	sta	auxram
	sta	foldup

	lda	#40
	sta	scrw

	lda	MACHID
	and	#$c0
	cmp	#$80
	beq	notplus

	; The Apple ][ and ][+ character
	; generator has no lower case, and an
	; 80-column card in one of those is not
	; the //e firmware, so it goes unused.

	lda	#$80
	sta	foldup
	rts
notplus
	sta	ALTCHRSET_ON	; display inverse lowercase
	lda	MACHID
	and	#$02
	beq	no80

	lda	#$80
	sta	col80
	lda	#80
	sta	scrw
no80
	lda	MACHID
	and	#$20
	cmp	#$20
	bne	done

	lda	#$80
	sta	auxram
done
	rts
	.)

setupvideo
	.(
	bit	col80
	bmi	firmware

	; Force plain 40-column Monitor output.
	; A previously run system program may
	; have left the firmware hooked up.

	sta	CLR80COL
	sta	CLR80STORE
	jsr	SETTXT

	lda	#<COUT1		; force video output for COUT
	sta	CSW+0
	lda	#>COUT1
	sta	CSW+1
	jmp	window
firmware
	jsr	SETTXT
	jsr	SLOT3		; hook up the //e
				; 80-column firmware

	lda	#$92		; ctrl-R, 80 columns
	jsr	cout
window
	lda	#0
	sta	WNDLFT
	sta	WNDTOP
	lda	scrw
	sta	WNDWDTH
	lda	#24
	sta	WNDBTM
	jsr	set_normal
	jmp	clrwin
	.)

#if PRODOS

initparms
	.(
	ldx	#nparm-1
loop
	lda	parmsrc,x
	sta	p_open,x
	dex
	bpl	loop
	rts
	.)

removeram
	; ProDOS puts its RAM disk in auxiliary
	; memory, which is where the undo ring / aux page cache
	; goes, so unhook it first, if it's there.

	.(
	bit	auxram
	bpl	done

	ldx	DEVCNT
scan
	lda	DEVLST,x
	and	#$f0
	cmp	#$b0		; slot 3, drive 2
	beq	found

	dex
	bpl	scan
done
	rts
found
	cpx	DEVCNT
	bcs	last
shift
	lda	DEVLST+1,x
	sta	DEVLST,x
	inx
	cpx	DEVCNT
	bcc	shift
last
	dec	DEVCNT

	lda	#<nodev
	sta	DEVADR+$16	; unit $bx
	lda	#>nodev
	sta	DEVADR+$17
	rts
	.)
#endif

banner
	.(
	ldx	#<txt_banner
	ldy	#>txt_banner
	jsr	putstr

	jsr	io_mline
	jmp	io_mline
	.)

#if PRODOS
openstory
	.(
	lda	#$c8		; OPEN
	ldx	#<p_open
	ldy	#>p_open
	jsr	mlicall
	bcc	opened

	ldx	#<txt_nostory
	ldy	#>txt_nostory
	jsr	putstr
nofile
	jmp	nofile
opened
	lda	p_open+5
	sta	p_read+1
	sta	p_mark+1
	rts
	.)

; Image of the MLI parameter blocks, copied
; down to $0288 at boot.

parmsrc
	; p_open
	.byt	3
	.word	pathname
	.word	FILEBUF
	.byt	0
	; p_read
	.byt	4
	.byt	0		; ref num
	.word	0		; data buffer
	.word	$0100		; request count
	.word	0		; bytes read
	; p_mark
	.byt	2
	.byt	0
	.byt	0,0,0
	; p_quit
	.byt	4
	.byt	0
	.word	0
	.byt	0
	.word	0
nparm = *-parmsrc
#endif

#if PRORWTS
openstory
	.(
	jsr	swaprwregs
        lda     #<pathname
        sta     prorwts_namlo
        lda     #>pathname
        sta     prorwts_namhi
	lda	#0
	sta	prorwts_sizelo
	sta	prorwts_ldrlo
	sta	prorwts_sizehi
	lda	#4		; TODO remove
	sta	prorwts_ldrhi
        lda	$C08B		; lc_bank=1  (use $C083 for lc_bank=2) — read twice
        lda	$C08B
        jsr	prorwts_opendir
        lda	$C081		; ROMIN
	; reset seek pos to 0
	lda	#0
	sta	$5f		;blkidx
	; next page read will be 0
	sta	next_rpagelo
	sta	next_rpagehi
	ldy	prorwts_status
	jsr	swaprwregs
	tya
	bne	nofile
	rts
nofile
	ldx	#<txt_nostory
	ldy	#>txt_nostory
	jsr	putstr
	jmp	diskerror	; TODO
	.)
#endif

pathname
	.byt	5
	.asc	"STORY"

txt_banner
	.asc	"Aa-machine "
	.asc	VERSION
	.asc	0

txt_nostory
	.asc	"Cannot open STORY.",10,0

#if PRODOS
RAMEND = $bb00
#endif
#if PRORWTS
RAMEND = $c000
#endif

; this library is at the end of the file
#if PRORWTS
prorwts2_init = *
;	.bin	0,0,"a2_prorwts2.bin"
SAFEPG = (* + $8ff) >> 8
#endif
#if PRODOS
SAFEPG = (* + $ff) >> 8
#endif
SAVEADDR = SAFEPG << 8
