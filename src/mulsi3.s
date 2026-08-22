/*
 * Minimal 68000-compatible 32-bit multiply helper for the freestanding
 * AmiAirPrint printer driver.
 *
 * IMPORTANT: this file intentionally uses an uppercase .S suffix so GCC's
 * preprocessor expands __USER_LABEL_PREFIX__.  AmigaOS GCC uses a target-
 * specific external symbol prefix; spelling "__mulsi3" literally in a plain
 * .s file can therefore define a different linker symbol than the compiler's
 * generated libgcc call expects.
 *
 * This is the same symbol-naming strategy used by GCC's own m68k libgcc
 * assembly sources.
 */
#define AP_CONCAT2(a,b) a##b
#define AP_CONCAT1(a,b) AP_CONCAT2(a,b)
#define AP_SYM(x) AP_CONCAT1(__USER_LABEL_PREFIX__, x)

	.text
	.globl AP_SYM(__mulsi3)

	.even
AP_SYM(__mulsi3):
	/* ABI: 4(sp)=a, 8(sp)=b, result in d0. */
	move.l 4(sp),d0
	move.l 8(sp),d1
	move.l d2,-(sp)
	move.l d3,-(sp)
	moveq #0,d2
	moveq #31,d3
.Lap_mul_loop:
	btst #0,d1
	beq.s .Lap_mul_noadd
	add.l d0,d2
.Lap_mul_noadd:
	lsl.l #1,d0
	lsr.l #1,d1
	dbra d3,.Lap_mul_loop
	move.l d2,d0
	move.l (sp)+,d3
	move.l (sp)+,d2
	rts
