	.text
	.globl AP_PrinterTag
	.globl AP_PEDData
	.globl AP_DriverBrand
	.extern AP_PrinterName
	.extern AP_Init
	.extern AP_Expunge
	.extern AP_Open
	.extern AP_Close
	.extern AP_Commands
	.extern AP_DoSpecial
	.extern AP_Render
	.extern AP_ConvFunc

	.even
AP_PrinterTag:
	moveq #0,d0
	rts
	dc.w 43
	dc.w 43
AP_PEDData:
	dc.l AP_PrinterName
	dc.l AP_Init
	dc.l AP_Expunge
	dc.l AP_Open
	dc.l AP_Close
	dc.b 3
	dc.b 10
	dc.b 80
	dc.b 1
	dc.w 1
	dc.l 4960
	dc.l 7015
	dc.w 600
	dc.w 600
	dc.l AP_Commands
	dc.l AP_DoSpecial
	dc.l AP_Render
	dc.l 60
	dc.l 0
	dc.l 0
	dc.l AP_ConvFunc

	/* Keep ownership metadata near the top of the first driver HUNK. */
AP_DriverBrand:
	.asciz "AmiAirPrint Printer Driver (c) Andreas 'Andiweli' Stuermer"
	.even
