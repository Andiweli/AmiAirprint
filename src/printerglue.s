	.text
	.globl AP_Init
	.globl AP_Expunge
	.globl AP_Open
	.globl AP_Close
	.globl AP_DoSpecial
	.extern AP_PD
	.extern SysBase
	.extern AP_OpenSetup
	.extern AP_CloseSetup
	.extern AP_DoSpecialC
	.extern AP_ConvFuncC

	.even
/*
 * Open-time glue for printer.device.
 *
 * Keep these entry points in plain 68000 assembly, matching the classic
 * Commodore printer-driver ABI exactly.  printer.device passes Init/Open
 * arguments on the stack; Init receives struct PrinterData * at 4(sp).
 *
 * The classic Commodore printer-driver init code also initializes SysBase
 * from AbsExecBase during Init.  Do the same here.  Do not defer this until
 * Render(), because Render() is the first place our PWG path calls Exec.
 */
AP_Init:
	move.l 4(sp),AP_PD
	move.l 4,SysBase
	moveq #0,d0
	rts

AP_Expunge:
	clr.l AP_PD
	clr.l SysBase
	rts

AP_Open:
	jsr AP_OpenSetup
	rts

AP_Close:
	jsr AP_CloseSetup
	moveq #0,d0
	rts

AP_DoSpecial:
	jmp AP_DoSpecialC

	.globl AP_ConvFunc
AP_ConvFunc:
	jmp AP_ConvFuncC
