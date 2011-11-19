/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 2000-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#include "EmCommon.h"
#include "EmRegs328.h"
#include "EmRegs328Prv.h"

#include "Byteswapping.h"		// Canonical
#include "EmHAL.h"				// EmHAL
#include "EmMemory.h"			// gMemAccessFlags, EmMem_memcpy
#include "EmPixMap.h"			// SetSize, SetRowBytes, etc.
#include "EmScreen.h"			// EmScreenUpdateInfo
#include "EmSession.h"			// GetDevice
#include "Hordes.h"				// Hordes::IsOn
#include "Logging.h"			// LogAppendMsg
#include "Miscellaneous.h"		// GetHostTime
#include "PreferenceMgr.h"		// Preference
#include "SessionFile.h"		// WriteHwrDBallType, etc.
#include "UAE.h"				// regs, SPCFLAG_INT
#include "EmPalmStructs.h"

#include "PalmPack.h"
#define NON_PORTABLE
	#include "HwrMiscFlags.h"						// hwrMiscFlagID1

	// Some platform-specific -- yet fairly portable -- defines.
	#define	hwrTD1PortENoBacklight		0x80			// (H) high if no backlight present

#undef NON_PORTABLE
#include "PalmPackPop.h"

/*
	This file controls the emulation of the Dragonball registers.

	As a subclass of EmRegs, EmRegs328 registers with EmBankRegs
	for control of the memory range 0xFFFFF000 to 0xFFFFFB14.  If
	an application accesses a memory location in that range,
	EmBankRegs calls on of the Set/GetLong/Word/Byte methods of
	this class.

	EmRegs328 provides handlers when particular memory addresses
	are accessed.  For instance, if the UART TX register is written
	to,  EmRegs328 will arrange for any data byte to be sent out
	the host computer's serial port.  If the UART RX register is
	read, EmRegs328 will respond with any byte in the buffer that
	contains data received from the host computer's serial port.

	Not all Dragonball registers are emulated the same way as on
	an actual device.  Some registers control hardware that exists
	on the device only, there being no analog on the host computer.
	For those registers, simple handlers are installed that write
	the specified value to the register and return that value later
	when the register is read.

	Other registers require extensive support.  The UART registers
	are examples of that, as the above text indicates.

	In the Palm OS source code, the Dragonball registers are
	represented by th HwrM68328Type data type.  We use this same type
	in Poser to provide the "backing memory" for the registers.  That
	is, if some emulated code writes to a Dragonball register, we
	store that value in a variable of type HwrM68328Type.

	(Note that there's really no guarantee that the compiler used to
	build Poser will lay out the fields of HwrM68328Type in the same
	way expected by the Palm OS.  Poser has a mechanism for creating
	buffers with fields that have the same layout as Palm OS-defined
	structs (see EmPalmStructs.h).  However, using that mechanism in
	this module is deadly to performance.  In particular, the Cycle
	method is called after every opcode is executed, and making that
	method go through the indirection and byteswapping required by
	the EmPalmStructs facilities totally kills performance.  In one
	test, a Gremlins run increased from 1m 24s to 1m 56s, or by about
	30%.  With targetted caching of the fields used in Cycle, we can
	regain most of that performance.  However, not all the performance
	is regained, and the resulting code is not very maintainable.)
*/

	#define hwr328chipID328		0x33
	#define hwr328maskID1H58B	0x30

static const uint32	ADDRESS_MASK = 0x0000FFF0;

#define PRINTF	if (1) ; else LogAppendMsg

// Values used to initialize the DragonBall registers.

const HwrM68328Type	kInitial68328RegisterValues =
{
	0x0C,		//	Byte		scr;						// $000: System Control Register
	0,		//	Byte										___filler0[0x004-0x001];

	// The following ID stuff is not present on earlier chips (before ??)
	hwr328chipID328,	//	Byte		chipID;						// $004: Chip ID Register
	hwr328maskID1H58B,	//	Byte		maskID;						// $005: Mask ID Register
	0x00,		//	Word		swID;						// $006: Software ID Register
	0,		//	Byte										___filler1[0x100-0x008];				 

	0x0000,		//	Word		csAGroupBase;				// $100: Chip Select Group A Base Register
	0x0000,		//	Word		csBGroupBase;				// $102: Chip Select Group B Base Register
	0x0000,		//	Word		csCGroupBase;				// $104: Chip Select Group C Base Register
	0x0000,		//	Word		csDGroupBase;				// $106: Chip Select Group D Base Register

	0x0000,		//	Word		csAGroupMask;				// $108: Chip Select Group A Mask Register
	0x0000,		//	Word		csBGroupMask;				// $10A: Chip Select Group B Mask Register
	0x0000,		//	Word		csCGroupMask;				// $10C: Chip Select Group C Mask Register
	0x0000,		//	Word		csDGroupMask;				// $10E: Chip Select Group D Mask Register

	0x00010006,	//	DWord		csASelect0;					// $110: Group A Chip Select 0 Register
	0x00010006,	//	DWord		csASelect1;					// $114: Group A Chip Select 1 Register
	0x00010006,	//	DWord		csASelect2;					// $118: Group A Chip Select 2 Register
	0x00010006,	//	DWord		csASelect3;					// $11C: Group A Chip Select 3 Register

	0x00000000,	//	DWord		csBSelect0;					// $120: Group B Chip Select 0 Register
	0x00000000,	//	DWord		csBSelect1;					// $124: Group B Chip Select 1 Register
	0x00000000,	//	DWord		csBSelect2;					// $128: Group B Chip Select 2 Register
	0x00000000,	//	DWord		csBSelect3;					// $12C: Group B Chip Select 3 Register

	0x00010006,	//	DWord		csCSelect0;					// $130: Group C Chip Select 0 Register
	0x00010006,	//	DWord		csCSelect1;					// $134: Group C Chip Select 1 Register
	0x00010006,	//	DWord		csCSelect2;					// $138: Group C Chip Select 2 Register
	0x00010006,	//	DWord		csCSelect3;					// $13C: Group C Chip Select 3 Register

	0x00000000,	//	DWord		csDSelect0;					// $140: Group D Chip Select 0 Register
	0x00000000,	//	DWord		csDSelect1;					// $144: Group D Chip Select 1 Register
	0x00000000,	//	DWord		csDSelect2;					// $148: Group D Chip Select 2 Register
	0x00000000,	//	DWord		csDSelect3;					// $14C: Group D Chip Select 3 Register

	0x0000,		//	Word		csDebug;					// $150: Chip Select debug register
	0,		//	Byte										___filler2[0x200-0x152];

	0x2400,		//	Word		pllControl;					// $200: PLL Control Register
	0x0123,		//	Word		pllFreqSel;					// $202: PLL Frequency Select Register
	0x0000,		//	Word		pllTest;					// $204: PLL Test Register
	0,		//	Byte										__filler44;
	0x1F,		//	Byte		pwrControl;					// $207: Power Control Register

	0,		//	Byte										___filler3[0x300-0x208];

	0x00,		//	Byte		intVector;					// $300: Interrupt Vector Register
	0,		//	Byte										___filler4;
	0x0000,		//	Word		intControl;					// $302: Interrupt Control Register
	0x00FF,		//	Word		intMaskHi;					// $304: Interrupt Mask Register/HIGH word
	0xFFFF,		//	Word		intMaskLo;					// $306: Interrupt Mask Register/LOW word
	0x00FF,		//	Word		intWakeupEnHi;				// $308: Interrupt Wakeup Enable Register
	0xFFFF,		//	Word		intWakeupEnLo;				// $30A: Interrupt Wakeup Enable Register
	0x0000,		//	Word		intStatusHi;				// $30C: Interrupt Status Register/HIGH word
	0x0000,		//	Word		intStatusLo;				// $30E: Interrupt Status Register/LOW word
	0x0000,		//	Word		intPendingHi;				// $310: Interrupt Pending Register
	0x0000,		//	Word		intPendingLo;				// $312: Interrupt Pending Register

	0,		//	Byte 										___filler4a[0x400-0x314];

	0x00,		//	Byte		portADir;					// $400: Port A Direction Register
	0x00,		//	Byte		portAData;					// $401: Port A Data Register
	0,		//	Byte										___filler5;
	0x00,		//	Byte		portASelect;				// $403: Port A Select Register

	0,		//	Byte										___filler6[4];

	0x00,		//	Byte		portBDir;					// $408: Port B Direction Register
	0x00,		//	Byte		portBData;					// $409: Port B Data Register
	0,		//	Byte										___filler7;
	0x00,		//	Byte		portBSelect;				// $40B: Port B Select Register

	0,		//	Byte										___filler8[4];

	0x00,		//	Byte		portCDir;					// $410: Port C Direction Register
	0x00,		//	Byte		portCData;					// $411: Port C Data Register
	0,		//	Byte										___filler9;
	0x00,		//	Byte		portCSelect;				// $413: Port C Select Register

	0,		//	Byte										___filler10[4];

	0x00,		//	Byte		portDDir;					// $418: Port D Direction Register
	0x00,		//	Byte		portDData;					// $419: Port D Data Register
	0xFF,		//	Byte		portDPullupEn;				// $41A: Port D Pull-up Enable
	0,		//	Byte										___filler11;
	0x00,		//	Byte		portDPolarity;				// $41C: Port D Polarity Register
	0x00,		//	Byte		portDIntReqEn;				// $41D: Port D Interrupt Request Enable
	0,		//	Byte										___filler12;
	0x00,		//	Byte		portDIntEdge;				// $41F: Port D IRQ Edge Register

	0x00,		//	Byte		portEDir;					// $420: Port E Direction Register
	0x00,		//	Byte		portEData;					// $421: Port E Data Register
	0x80,		//	Byte		portEPullupEn;				// $422: Port E Pull-up Enable
	0x80,		//	Byte		portESelect;				// $423: Port E Select Register

	0,		//	Byte										___filler14[4];

	0x00,		//	Byte		portFDir;					// $428: Port F Direction Register
	0x00,		//	Byte		portFData;					// $429: Port F Data Register
	0xFF,		//	Byte		portFPullupEn;				// $42A: Port F Pull-up Enable
	0xFF,		//	Byte		portFSelect;				// $42B: Port F Select Register

	0,		//	Byte										___filler16[4];

	0x00,		//	Byte		portGDir;					// $430: Port G Direction Register
	0x00,		//	Byte		portGData;					// $431: Port G Data Register
	0xFF,		//	Byte		portGPullupEn;				// $432: Port G Pull-up Enable
	0xFF,		//	Byte		portGSelect;				// $433: Port G Select Register

	0,		//	Byte										___filler18[4];

	0x00,		//	Byte		portJDir;					// $438: Port J Direction Register
	0x00,		//	Byte		portJData;					// $439: Port J Data Register
	0,		//	Byte										___filler19;
	0x00,		//	Byte		portJSelect;				// $43B: Port J Select Register

	0,		//	Byte										___filler19a[4];

	0x00,		//	Byte		portKDir;					// $440: Port K Direction Register
	0x00,		//	Byte		portKData;					// $441: Port K Data Register
	0x3F,		//	Byte		portKPullupEn;				// $442: Port K Pull-up Enable
	0x3F,		//	Byte		portKSelect;				// $443: Port K Select Register

	0,		//	Byte										___filler21[4];

	0x00,		//	Byte		portMDir;					// $448: Port M Direction Register
	0x00,		//	Byte		portMData;					// $449: Port M Data Register
	0xFF,		//	Byte		portMPullupEn;				// $44A: Port M Pull-up Enable Register
	0x02,		//	Byte		portMSelect;				// $44B: Port M Select Register

	0,		//	Byte										___filler22[4];

	0,		//	Byte										___filler23[0x500-0x450];

	0x0000,		//	Word		pwmControl;					// $500: PWM Control Register
	0x0000,		//	Word		pwmPeriod;					// $502: PWM Period Register
	0x0000,		//	Word		pwmWidth;					// $504: PWM Width Register
	0x0000,		//	Word		pwmCounter;					// $506: PWM Counter

	0,		//	Byte										___filler24[0x600-0x508];

	0x0000,		//	Word		tmr1Control;				// $600: Timer 1 Control Register
	0x0000,		//	Word		tmr1Prescaler;				// $602: Timer 1 Prescaler Register
	0xFFFF,		//	Word		tmr1Compare;				// $604: Timer 1 Compare Register
	0x0000,		//	Word		tmr1Capture;				// $606: Timer 1 Capture Register
	0x0000,		//	Word		tmr1Counter;				// $608: Timer 1 Counter Register
	0x0000,		//	Word		tmr1Status;					// $60A: Timer 1 Status Register

	0x0000,		//	Word		tmr2Control;				// $60C: Timer 2 Control Register
	0x0000,		//	Word		tmr2Prescaler;				// $60E: Timer 2 Prescaler Register
	0xFFFF,		//	Word		tmr2Compare;				// $610: Timer 2 Compare Register
	0x0000,		//	Word		tmr2Capture;				// $612: Timer 2 Capture Register
	0x0000,		//	Word		tmr2Counter;				// $614: Timer 2 Counter Register
	0x0000,		//	Word		tmr2Status;					// $616: Timer 2 Status Register

	0x0000,		//	Word		wdControl;					// $618: Watchdog Control Register
	0x0000,		//	Word		wdReference;				// $61A: Watchdog Reference Register
	0x0000,		//	Word		wdCounter;					// $61C: Watchdog Counter

	0,		//	Byte										___filler25[0x700-0x61E];

	0x0000,		//	Word		spiSlave;					// $700: SPI Slave Register

	0,		//	Byte										___filler26[0x800-0x702];

	0x0000,		//	Word		spiMasterData;				// $800: SPI Master Data Register
	0x0000,		//	Word		spiMasterControl;			// $802: SPI Master Control Register

	0,		//	Byte										___filler27[0x900-0x804];

	0x0000,		//	Word		uControl;					// $900: Uart Control Register
	0x003F,		//	Word		uBaud;						// $902: Uart Baud Control Register
	0x0000,		//	Word		uReceive;					// $904: Uart Receive Register
	0x0000,		//	Word		uTransmit;					// $906: Uart Transmit Register
	0x0000,		//	Word		uMisc;						// $908: Uart Miscellaneous Register

	0,		//	Byte										___filler28[0xA00-0x90A];

	0x00000000,	//	DWord		lcdStartAddr;				// $A00: Screen Starting Address Register
	0,		//	Byte										___filler29;
	0xFF,		//	Byte		lcdPageWidth;				// $A05: Virtual Page Width Register
	0,		//	Byte										___filler30[2];
	0x03FF,		//	Word		lcdScreenWidth;				// $A08: Screen Width Register
	0x01FF,		//	Word		lcdScreenHeight;			// $A0A: Screen Height Register
	0,		//	Byte										___filler31[0xA18-0xA0C];
	0x0000,		//	Word		lcdCursorXPos;				// $A18: Cursor X Position
	0x0000,		//	Word		lcdCursorYPos;				// $A1A:	Cursor Y Position
	0x0101,		//	Word		lcdCursorWidthHeight;		// $A1C: Cursor Width and Height
	0,		//	Byte										___filler32;
	0x7F,		//	Byte		lcdBlinkControl;			// $A1F: Blink Control Register
	0x00,		//	Byte		lcdPanelControl;			// $A20: Panel Interface Control Register
	0x00,		//	Byte		lcdPolarity;				// $A21: Polarity Config Register
	0x00,		//	Byte										___filler33;
	0x00,		//	Byte		lcdACDRate;					// $A23: ACD (M) Rate Control Register
	0x00,		//	Byte										___filler34;
	0x00,		//	Byte		lcdPixelClock;				// $A25: Pixel Clock Divider Register
	0x00,		//	Byte										___filler35;
	0x40,		//	Byte		lcdClockControl;			// $A27: Clocking Control Register
	0x00,		//	Byte										___filler36;
	0x3E,		//	Byte		lcdLastBufferAddr;			// $A29: Last Buffer Address Register
	0x00,		//	Byte										___filler37;
	0x3F,		//	Byte		lcdOctetTermCount;			// $A2B: Octet Terminal Count Register
	0x00,		//	Byte										___filler38;
	0x00,		//	Byte		lcdPanningOffset;			// $A2D: Panning Offset Register
	0,		//	Byte										___filler39[3];
	0xB9,		//	Byte		lcdFrameRate;				// $A31: Frame Rate Control Modulation Register
	0x1073,		//	Word		lcdGrayPalette;				// $A32: Gray Palette Mapping Register
	0x00,		//	Byte		lcdReserved;				// $A34: Reserved

	0,		//	Byte										___filler40[0xB00-0xA35];

	0x00000000,	//	DWord		rtcHourMinSec;				// $B00: RTC Hours, Minutes, Seconds Register
	0x00000000,	//	DWord		rtcAlarm;					// $B04: RTC Alarm Register
	0x00000000,	//	DWord		rtcReserved;				// $B08: RTC Reserved
	0x0000,		//	Word		rtcControl;					// $B0C: RTC Control Register
	0x0000,		//	Word		rtcIntStatus;				// $B0E: RTC Interrupt Status Register
	0x0000,		//	Word		rtcIntEnable;				// $B10: RTC Interrupt Enable Register
	0x0000		//	Word		stopWatch;					// $B12: Stopwatch Minutes
};


// ---------------------------------------------------------------------------
//		� EmRegs328::EmRegs328
// ---------------------------------------------------------------------------

EmRegs328::EmRegs328 (void) :
	EmRegs (),
	f68328Regs (),
	fHotSyncButtonDown (0),
	fTmr2CurrentMilliseconds (0),
	fTmr2StartMilliseconds (0),
	fKeyBits (0),
	fLastTmr1Status (0),
	fLastTmr2Status (0),
	fPortDEdge (0),
	fPortDDataCount (0),
	fHour (0),
	fMin (0),
	fSec (0),
	fTick (0),
	fCycle (0),
	fUART (NULL)
{
}


// ---------------------------------------------------------------------------
//		� EmRegs328::~EmRegs328
// ---------------------------------------------------------------------------

EmRegs328::~EmRegs328 (void)
{
}


// ---------------------------------------------------------------------------
//		� EmRegs328::Initialize
// ---------------------------------------------------------------------------

void EmRegs328::Initialize (void)
{
	EmRegs::Initialize ();

	fUART = new EmUARTDragonball (EmUARTDragonball::kUART_Dragonball, 0);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::Reset
// ---------------------------------------------------------------------------

void EmRegs328::Reset (Bool hardwareReset)
{
	EmRegs::Reset (hardwareReset);

	if (hardwareReset)
	{
		f68328Regs = kInitial68328RegisterValues;

		// Byteswap all the words in the Dragonball registers (if necessary).

		Canonical (f68328Regs);
		ByteswapWords (&f68328Regs, sizeof(f68328Regs));

		fKeyBits		= 0;
		fLastTmr1Status	= 0;
		fLastTmr2Status	= 0;
		fPortDEdge		= 0;
		fPortDDataCount	= 0;

		// React to the new data in the UART registers.

		Bool	sendTxData = false;
		EmRegs328::UARTStateChanged (sendTxData);
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::Save
// ---------------------------------------------------------------------------

void EmRegs328::Save (SessionFile& f)
{
	EmRegs::Save (f);

	StWordSwapper		swapper1 (&f68328Regs, sizeof(f68328Regs));
	f.WriteHwrDBallType (f68328Regs);
	f.FixBug (SessionFile::kBugByteswappedStructs);

	const long	kCurrentVersion = 4;

	Chunk			chunk;
	EmStreamChunk	s (chunk);

	s << kCurrentVersion;
	s << fHotSyncButtonDown;
	s << fLastTmr1Status;
	s << fLastTmr2Status;
	s << fPortDEdge;

	// Added in version 2.

	s << fKeyBits;

	// Added in version 3.

	s << fHour;
	s << fMin;
	s << fSec;
	s << fTick;
	s << fCycle;

	// Added in version 4.

	s << fPortDDataCount;

	f.WriteDBallState (chunk);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::Load
// ---------------------------------------------------------------------------

void EmRegs328::Load (SessionFile& f)
{
	EmRegs::Load (f);

	if (f.ReadHwrDBallType (f68328Regs))
	{
		// The Windows version of Poser 2.1d29 and earlier did not write
		// out structs in the correct format.  The fields of the struct
		// were written out in Little-Endian format, not Big-Endian.  To
		// address this problem, the bug has been fixed, and a new field
		// is added to the file format indicating that the bug has been
		// fixed.  With the new field (the "bug bit"), Poser can identify
		// old files from new files and read them in accordingly.
		// 
		// With the bug fixed, the .psf files should now be interchangeable
		// across platforms (modulo other bugs...).

		if (!f.IncludesBugFix (SessionFile::kBugByteswappedStructs))
		{
			Canonical (f68328Regs);
		}
		ByteswapWords (&f68328Regs, sizeof(f68328Regs));

		// React to the new data in the UART registers.

		Bool	sendTxData = false;
		EmRegs328::UARTStateChanged (sendTxData);

		// Reset gMemAccessFlags.fProtect_SRAMSet

		gMemAccessFlags.fProtect_SRAMSet = (READ_REGISTER (csASelect1) & 0x0008) != 0;
	}
	else
	{
		f.SetCanReload (false);
	}

	Chunk		chunk;
	if (f.ReadDBallState (chunk))
	{
		long			version;
		EmStreamChunk	s (chunk);

		s >> version;

		if (version >= 1)
		{
			s >> fHotSyncButtonDown;
			s >> fTmr2CurrentMilliseconds;
			s >> fTmr2StartMilliseconds;
			s >> fLastTmr1Status;
			s >> fLastTmr2Status;
			s >> fPortDEdge;
		}

		if (version >= 2)
		{
			s >> fKeyBits;
		}

		if (version >= 3)
		{
			s >> fHour;
			s >> fMin;
			s >> fSec;
			s >> fTick;
			s >> fCycle;
		}

		if (version >= 4)
		{
			s >> fPortDDataCount;
		}
	}
	else
	{
		f.SetCanReload (false);
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::Dispose
// ---------------------------------------------------------------------------

void EmRegs328::Dispose (void)
{
	delete fUART;
	fUART = NULL;

	EmRegs::Dispose ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::SetSubBankHandlers
// ---------------------------------------------------------------------------

void EmRegs328::SetSubBankHandlers (void)
{
	// Install base handlers.

	EmRegs::SetSubBankHandlers ();

	// Now add standard/specialized handers for the defined registers.

	INSTALL_HANDLER (StdRead,			StdWrite,				scr);

	INSTALL_HANDLER (StdRead,			NullWrite,				chipID);
	INSTALL_HANDLER (StdRead,			NullWrite,				maskID);
	INSTALL_HANDLER (StdRead,			NullWrite,				swID);

	INSTALL_HANDLER (StdRead,			StdWrite,				csAGroupBase);
	INSTALL_HANDLER (StdRead,			StdWrite,				csBGroupBase);
	INSTALL_HANDLER (StdRead,			StdWrite,				csCGroupBase);
	INSTALL_HANDLER (StdRead,			StdWrite,				csDGroupBase);

	INSTALL_HANDLER (StdRead,			StdWrite,				csAGroupMask);
	INSTALL_HANDLER (StdRead,			StdWrite,				csBGroupMask);
	INSTALL_HANDLER (StdRead,			StdWrite,				csCGroupMask);
	INSTALL_HANDLER (StdRead,			StdWrite,				csDGroupMask);

	INSTALL_HANDLER (StdRead,			StdWrite,				csASelect0);
	INSTALL_HANDLER (StdRead,			csASelect1Write,		csASelect1);
	INSTALL_HANDLER (StdRead,			StdWrite,				csASelect2);
	INSTALL_HANDLER (StdRead,			StdWrite,				csASelect3);

	INSTALL_HANDLER (StdRead,			StdWrite,				csBSelect0);
	INSTALL_HANDLER (StdRead,			StdWrite,				csBSelect1);
	INSTALL_HANDLER (StdRead,			StdWrite,				csBSelect2);
	INSTALL_HANDLER (StdRead,			StdWrite,				csBSelect3);

	INSTALL_HANDLER (StdRead,			csCSelect0Write,		csCSelect0);
	INSTALL_HANDLER (StdRead,			csCSelect1Write,		csCSelect1);
	INSTALL_HANDLER (StdRead,			StdWrite,				csCSelect2);
	INSTALL_HANDLER (StdRead,			StdWrite,				csCSelect3);

	INSTALL_HANDLER (StdRead,			StdWrite,				csDSelect0);
	INSTALL_HANDLER (StdRead,			StdWrite,				csDSelect1);
	INSTALL_HANDLER (StdRead,			StdWrite,				csDSelect2);
	INSTALL_HANDLER (StdRead,			StdWrite,				csDSelect3);
	INSTALL_HANDLER (StdRead,			StdWrite,				csDebug);

	INSTALL_HANDLER (StdRead,			StdWrite,				pllControl);
	INSTALL_HANDLER (pllFreqSelRead,	StdWrite,				pllFreqSel);
	INSTALL_HANDLER (StdRead,			StdWrite,				pllTest);
	INSTALL_HANDLER (StdRead,			StdWrite,				pwrControl);

	INSTALL_HANDLER (StdRead,			StdWrite,				intVector);
	INSTALL_HANDLER (StdRead,			StdWrite,				intControl);
	INSTALL_HANDLER (StdRead,			intMaskHiWrite,			intMaskHi);
	INSTALL_HANDLER (StdRead,			intMaskLoWrite,			intMaskLo);
	INSTALL_HANDLER (StdRead,			StdWrite,				intWakeupEnHi);
	INSTALL_HANDLER (StdRead,			StdWrite,				intWakeupEnLo);
	INSTALL_HANDLER (StdRead,			intStatusHiWrite,		intStatusHi);
	INSTALL_HANDLER (StdRead,			NullWrite,				intStatusLo);
	INSTALL_HANDLER (StdRead,			NullWrite,				intPendingHi);
	INSTALL_HANDLER (StdRead,			NullWrite,				intPendingLo);

	INSTALL_HANDLER (StdRead,			StdWrite,				portADir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portAData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portASelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portBDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portBData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portBSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portCDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portCData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portCSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portDDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portDData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portDPullupEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portDPolarity);
	INSTALL_HANDLER (StdRead,			portDIntReqEnWrite,		portDIntReqEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portDIntEdge);

	INSTALL_HANDLER (StdRead,			StdWrite,				portEDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portEData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portEPullupEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portESelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portFDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portFData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portFPullupEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portFSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portGDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portGData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portGPullupEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portGSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portJDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portJData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portJSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portKDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portKData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portKPullupEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portKSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				portMDir);
	INSTALL_HANDLER (portXDataRead,		portXDataWrite,			portMData);
	INSTALL_HANDLER (StdRead,			StdWrite,				portMPullupEn);
	INSTALL_HANDLER (StdRead,			StdWrite,				portMSelect);

	INSTALL_HANDLER (StdRead,			StdWrite,				pwmControl);
	INSTALL_HANDLER (StdRead,			StdWrite,				pwmPeriod);
	INSTALL_HANDLER (StdRead,			StdWrite,				pwmWidth);
	INSTALL_HANDLER (StdRead,			NullWrite,				pwmCounter);

	INSTALL_HANDLER (StdRead,			StdWrite,				tmr1Control);
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr1Prescaler);
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr1Compare);
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr1Capture);
	INSTALL_HANDLER (StdRead,			NullWrite,				tmr1Counter);
	INSTALL_HANDLER (tmr1StatusRead,	tmr1StatusWrite,		tmr1Status);

	INSTALL_HANDLER (StdRead,			StdWrite,				tmr2Control);
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr2Prescaler);
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr2Compare);
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr2Capture);
	INSTALL_HANDLER (StdRead,			NullWrite,				tmr2Counter);
	INSTALL_HANDLER (tmr2StatusRead,	tmr2StatusWrite,		tmr2Status);

	INSTALL_HANDLER (StdRead,			StdWrite,				wdControl);
	INSTALL_HANDLER (StdRead,			StdWrite,				wdReference);
	INSTALL_HANDLER (StdRead,			wdCounterWrite,			wdCounter);

	INSTALL_HANDLER (StdRead,			StdWrite,				spiSlave);
	INSTALL_HANDLER (StdRead,			StdWrite,				spiMasterData);
	INSTALL_HANDLER (StdRead,			spiMasterControlWrite,	spiMasterControl);

	INSTALL_HANDLER (uartRead,			uartWrite,				uControl);
	INSTALL_HANDLER (uartRead,			uartWrite,				uBaud);
	INSTALL_HANDLER (uartRead,			uartWrite,				uReceive);
	INSTALL_HANDLER (uartRead,			uartWrite,				uTransmit);
	INSTALL_HANDLER (uartRead,			uartWrite,				uMisc);

	INSTALL_HANDLER (StdRead,			lcdRegisterWrite,		lcdStartAddr);
	INSTALL_HANDLER (StdRead,			lcdRegisterWrite,		lcdPageWidth);
	INSTALL_HANDLER (StdRead,			lcdRegisterWrite,		lcdScreenWidth);
	INSTALL_HANDLER (StdRead,			lcdRegisterWrite,		lcdScreenHeight);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdCursorXPos);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdCursorYPos);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdCursorWidthHeight);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdBlinkControl);
	INSTALL_HANDLER (StdRead,			lcdRegisterWrite,		lcdPanelControl);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdPolarity);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdACDRate);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdPixelClock);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdClockControl);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdLastBufferAddr);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdOctetTermCount);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdPanningOffset);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdFrameRate);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdGrayPalette);
	INSTALL_HANDLER (StdRead,			StdWrite,				lcdReserved);

	INSTALL_HANDLER (rtcHourMinSecRead,	StdWrite,				rtcHourMinSec);
	INSTALL_HANDLER (StdRead,			StdWrite,				rtcAlarm);
	INSTALL_HANDLER (StdRead,			StdWrite,				rtcReserved);
	INSTALL_HANDLER (StdRead,			rtcControlWrite,		rtcControl);
	INSTALL_HANDLER (StdRead,			rtcIntStatusWrite,		rtcIntStatus);
	INSTALL_HANDLER (StdRead,			rtcIntEnableWrite,		rtcIntEnable);
	INSTALL_HANDLER (StdRead,			StdWrite,				stopWatch);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetRealAddress
// ---------------------------------------------------------------------------

uint8* EmRegs328::GetRealAddress (emuptr address)
{
	uint8*	loc = ((uint8*) &f68328Regs) + (address - kMemoryStart);

	return loc;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetAddressStart
// ---------------------------------------------------------------------------

emuptr EmRegs328::GetAddressStart (void)
{
	return kMemoryStart;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetAddressRange
// ---------------------------------------------------------------------------

uint32 EmRegs328::GetAddressRange (void)
{
	COMPILE_TIME_ASSERT (kMemorySize == 0x0B14);

	return kMemorySize;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::Cycle
// ---------------------------------------------------------------------------
// Handles periodic events that need to occur when the processor cycles (like
// updating timer registers).  This function is called in two places from
// Emulator::Execute.  Interestingly, the loop runs 3% FASTER if this function
// is in its own separate function instead of being inline.

#if 0
static int		calibrated;
static int		increment;
static int		timesCalled;
static uint32	startingTime;

static void PrvCalibrate (uint16 tmrCompare)
{
	// Calibrate the the value by which we increment the counter.
	// The counter is set up so that it times out after 10 milliseconds
	// so that it can increment the Palm OS's tick counter 100 times
	// a second.  We would like tmrCounter to surpass tmrCompare
	// after 10 milliseconds.  So figure out by how much we need to
	// increment it in order for that to happen.

	// If timer is disabled; reset calibration.

	if (tmrCompare == 0xFFFF)
	{
		startingTime = 0;
	}

	// If timer is enabled, restart calibration.

	else if (startingTime == 0)
	{
		startingTime = Platform::GetMilliseconds();
		timesCalled = 0;
		increment = 1;
	}

	// If calibration is started, continue it.

	else
	{
		timesCalled++;

		uint32	now = Platform::GetMilliseconds();
		if (now - startingTime > 100)
		{
			calibrated = true;
			increment = tmrCompare / (timesCalled / 10);
		}
	}
}
#endif

void EmRegs328::Cycle (Bool sleeping)
{
#if 0
	// Cycle is *very* sensitive to timing issue.  With this section
	// of code, a Gremlins run can slow down by 5%.
	if (!calibrated)
	{
		::PrvCalibrate (READ_REGISTER (tmr2Compare));
	}
#else
	#if _DEBUG
		#define increment	20
	#else
		#define increment	4
	#endif
#endif

	// Determine whether timer 2 is enabled.

	if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnable) != 0)
	{
		// If so, increment the timer.

		WRITE_REGISTER (tmr2Counter, READ_REGISTER (tmr2Counter) + (sleeping ? 1 : increment));

		// Determine whether the timer has reached the specified count.

		if (sleeping || READ_REGISTER (tmr2Counter) > READ_REGISTER (tmr2Compare))
		{
			// Flag the occurrence of the successful comparison.

			WRITE_REGISTER (tmr2Status, READ_REGISTER (tmr2Status) | hwr328TmrStatusCompare);

			// If the Free Run/Restart flag is not set, clear the counter.

			if ((READ_REGISTER (tmr2Control) & hwr328TmrControlFreeRun) == 0)
			{
				WRITE_REGISTER (tmr2Counter, 0);
			}

			// If the timer interrupt is enabled, post an interrupt.

			if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnInterrupt) != 0)
			{
				WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwr328IntLoTimer2);
				EmRegs328::UpdateInterrupts ();
			}
		}
	}

	if ((fCycle += increment) > READ_REGISTER (tmr2Compare))
	{
		fCycle = 0;

		if (++fTick >= 100)
		{
			fTick = 0;

			if (++fSec >= 60)
			{
				fSec = 0;

				if (++fMin >= 60)
				{
					fMin = 0;

					if (++fHour >= 24)
					{
						fHour = 0;
					}
				}
			}
		}
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::CycleSlowly
// ---------------------------------------------------------------------------
// Handles periodic events that need to occur when the processor cycles (like
// updating timer registers).  This function is called in two places from
// Emulator::Execute.  Interestingly, the loop runs 3% FASTER if this function
// is in its own separate function instead of being inline.

void EmRegs328::CycleSlowly (Bool sleeping)
{
	UNUSED_PARAM(sleeping)

	// See if a hard button is pressed.

	EmAssert (gSession);
	if (gSession->HasButtonEvent ())
	{
		EmButtonEvent	event = gSession->GetButtonEvent ();
		if (event.fButton == kElement_CradleButton)
		{
			EmRegs328::HotSyncEvent (event.fButtonIsDown);
		}
		else
		{
			EmRegs328::ButtonEvent (event.fButton, event.fButtonIsDown);
		}
	}

	// See if there's anything new ("Put the data on the bus")

	EmRegs328::UpdateUARTState (false);

	// Check to see if the RTC alarm is ready to go off.  First see
	// if the RTC is enabled, and that the alarm event isn't already
	// registered (the latter check is just an optimization).

	if ((READ_REGISTER (rtcIntEnable) & hwr328RTCIntEnableAlarm) != 0 &&
		(READ_REGISTER (rtcIntStatus) & hwr328RTCIntStatusAlarm) == 0)
	{
		uint32	rtcAlarm = READ_REGISTER (rtcAlarm);

		long	almHour	 = (rtcAlarm & hwr328RTCAlarmHoursMask) >> hwr328RTCAlarmHoursOffset;
		long	almMin	 = (rtcAlarm & hwr328RTCAlarmMinutesMask) >> hwr328RTCAlarmMinutesOffset;
		long	almSec	 = (rtcAlarm & hwr328RTCAlarmSecondsMask) >> hwr328RTCAlarmSecondsOffset;
		long	almInSeconds = (almHour * 60 * 60) + (almMin * 60) + almSec;

		long	nowHour;
		long	nowMin;
		long	nowSec;
		::GetHostTime (&nowHour, &nowMin, &nowSec);
		long	nowInSeconds = (nowHour * 60 * 60) + (nowMin * 60) + nowSec;

		if (almInSeconds <= nowInSeconds)
		{
			WRITE_REGISTER (rtcIntStatus, READ_REGISTER (rtcIntStatus) | hwr328RTCIntStatusAlarm);
			EmRegs328::UpdateRTCInterrupts ();
		}
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::TurnSoundOff
// ---------------------------------------------------------------------------

void EmRegs328::TurnSoundOff (void)
{
	uint16	pwmControl = READ_REGISTER (pwmControl);
	WRITE_REGISTER (pwmControl, pwmControl & ~hwr328PWMControlEnable);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::ResetTimer
// ---------------------------------------------------------------------------

void EmRegs328::ResetTimer (void)
{
	WRITE_REGISTER (tmr2Counter, 0);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::ResetRTC
// ---------------------------------------------------------------------------

void EmRegs328::ResetRTC (void)
{
	fHour = 15;
	fMin = 0;
	fSec = 0;
	fTick = 0;
	fCycle = 0;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetInterruptLevel
// ---------------------------------------------------------------------------

int32 EmRegs328::GetInterruptLevel (void)
{
	uint16	intStatusHi = READ_REGISTER (intStatusHi);
	uint16	intStatusLo = READ_REGISTER (intStatusLo);

	// Level 7 = IRQ7.

	if ((intStatusHi & hwr328IntHiNMI) != 0)
		return 7;

	// Level 6 = SPIS, TMR1, IRQ6.

	if ((intStatusHi & (hwr328IntHiTimer1 | hwr328IntHiSPIS | hwr328IntHiIRQ6)) != 0)
		return 6;

	// Level 5 = PEN.

	if ((intStatusHi & hwr328IntHiPen) != 0)
		return 5;

	// Level 4 = SPIM, TMR2, UART, WDT, RTC, KB, PWM, INT0 - INT7.

	if ((intStatusLo & (	hwr328IntLoAllKeys |
							hwr328IntLoPWM |
							hwr328IntLoKbd |
						//	hwr328IntLoLCDC |
							hwr328IntLoRTC |
							hwr328IntLoWDT |
							hwr328IntLoTimer2 |
							hwr328IntLoSPIM)) != 0)
		return 4;

	// Level 3 = IRQ3.

	if ((intStatusHi & hwr328IntHiIRQ3) != 0)
		return 3;

	// Level 2 = IRQ2.

	if ((intStatusHi & hwr328IntHiIRQ2) != 0)
		return 2;

	// Level 1 = IRQ1.

	if ((intStatusHi & hwr328IntHiIRQ1) != 0)
		return 1;

	// Level 0.

	return -1;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetInterruptBase
// ---------------------------------------------------------------------------

int32 EmRegs328::GetInterruptBase (void)
{
	return READ_REGISTER (intVector) & 0xF8;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetLCDHasFrame
// ---------------------------------------------------------------------------

Bool EmRegs328::GetLCDHasFrame (void)
{
	return false;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetLCDBeginEnd
// ---------------------------------------------------------------------------

void EmRegs328::GetLCDBeginEnd (emuptr& begin, emuptr& end)
{
	emuptr	baseAddr	= READ_REGISTER (lcdStartAddr);
	int		rowBytes	= READ_REGISTER (lcdPageWidth) * 2;
	int		height		= READ_REGISTER (lcdScreenHeight) + 1;

	begin = baseAddr;
	end = baseAddr + rowBytes * height;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetLCDScanlines
// ---------------------------------------------------------------------------

void EmRegs328::GetLCDScanlines (EmScreenUpdateInfo& info)
{
	// Get the screen metrics.

	int32	bpp			= 1 << (READ_REGISTER (lcdPanelControl) & 0x01);
	int32	width		= READ_REGISTER (lcdScreenWidth) + 1;
	int32	height		= READ_REGISTER (lcdScreenHeight) + 1;
	int32	rowBytes	= READ_REGISTER (lcdPageWidth) * 2;
	emuptr	baseAddr	= READ_REGISTER (lcdStartAddr);

	info.fLeftMargin	= READ_REGISTER (lcdPanningOffset) & 0x0F;

	EmPixMapFormat	format	=	bpp == 1 ? kPixMapFormat1 :
								bpp == 2 ? kPixMapFormat2 :
								bpp == 4 ? kPixMapFormat4 :
								kPixMapFormat8;

	RGBList	colorTable;
	this->PrvGetPalette (colorTable);

	// Set format, size, and color table of EmPixMap.

	info.fImage.SetSize			(EmPoint (width, height));
	info.fImage.SetFormat		(format);
	info.fImage.SetRowBytes		(rowBytes);
	info.fImage.SetColorTable	(colorTable);

	// Determine first and last scanlines to fetch, and fetch them.

	info.fFirstLine		= (info.fScreenLow - baseAddr) / rowBytes;
	info.fLastLine		= (info.fScreenHigh - baseAddr - 1) / rowBytes + 1;

	long	firstLineOffset	= info.fFirstLine * rowBytes;
	long	lastLineOffset	= info.fLastLine * rowBytes;

	EmMem_memcpy (
		(void*) ((uint8*) info.fImage.GetBits () + firstLineOffset),
		baseAddr + firstLineOffset,
		lastLineOffset - firstLineOffset);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetUARTDevice
// ---------------------------------------------------------------------------
// Return what sort of device is hooked up to the given UART.

EmUARTDeviceType EmRegs328::GetUARTDevice (int /*uartNum*/)
{
	Bool	serEnabled	= this->GetLineDriverState (kUARTSerial);
	Bool	irEnabled	= this->GetLineDriverState (kUARTIR);

	// It's probably an error to have them both enabled at the same
	// time.  !!! TBD: make this an error message.

	EmAssert (!(serEnabled && irEnabled));

	if (serEnabled)
		return kUARTSerial;

	if (irEnabled)
		return kUARTIR;

	return kUARTNone;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetDynamicHeapSize
// ---------------------------------------------------------------------------

int32 EmRegs328::GetDynamicHeapSize (void)
{
	uint32	result = 0;

	uint32	csCSelect0 = READ_REGISTER (csCSelect0) & ADDRESS_MASK;
	uint32	csCSelect1 = READ_REGISTER (csCSelect1) & ADDRESS_MASK;

	if (csCSelect0 == 0x0000 && csCSelect1 == 0x0000)
	{
		result = 0 * 1024L;
	}
	else if (csCSelect0 == 0x0070 && csCSelect1 == 0x0000)
	{
		result = 32 * 1024L;
	}
	else if (csCSelect0 == 0x00F0 && csCSelect1 == 0x0000)
	{
		result = 64 * 1024L;
	}
	else if (csCSelect0 == 0x0070 && csCSelect1 == 0x0070)
	{
		// This one's odd, but the Symbol seems to (temporarily)
		// set up this configuration when running with 2Meg of RAM.
		result = 96 * 1024L;
	}
	else if (csCSelect0 == 0x00F0 && csCSelect1 == 0x0070)
	{
		result = 96 * 1024L;
	}
	else if (csCSelect0 == 0x01F0 && csCSelect1 == 0x0000)
	{
		result = 128 * 1024L;
	}
	else if (csCSelect0 == 0x03F0 && csCSelect1 == 0x0000)
	{
		result = 256 * 1024L;
	}
	else
	{
		EmAssert (false);
	}

	if (!ChipSelectsConfigured())
	{
		result = 16 * 1024L * 1024L;
	}

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetROMSize
// ---------------------------------------------------------------------------

int32 EmRegs328::GetROMSize (void)
{
	uint32	result = 2 * 1024L * 1024L;

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetROMBaseAddress
// ---------------------------------------------------------------------------

emuptr EmRegs328::GetROMBaseAddress (void)
{
	if (!this->ChipSelectsConfigured())
	{
		return 0xFFFFFFFF;
	}
	
	return 0x10C00000;  // use known value
}


// ---------------------------------------------------------------------------
//		� EmRegs328::ChipSelectsConfigured
// ---------------------------------------------------------------------------

Bool EmRegs328::ChipSelectsConfigured (void)
{
	return READ_REGISTER (csAGroupBase) & 0x0001;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetSystemClockFrequency
// ---------------------------------------------------------------------------

int32 EmRegs328::GetSystemClockFrequency (void)
{
	uint16	pllControl	= READ_REGISTER (pllControl);
	uint16	pllFreqSel	= READ_REGISTER (pllFreqSel);

	uint16	PC			= (pllFreqSel & 0x00FF);
	uint16	QC			= (pllFreqSel & 0x0F00) >> 8;

	uint32	result = 32768L * (14 * (PC + 1) + QC + 1);

	// Divide by the system clock scaler, if needed.

	switch (pllControl & 0x0F00)
	{
		case hwr328PLLControlSysVCODiv2:
			result /= 2;
			break;

		case hwr328PLLControlSysVCODiv4:
			result /= 4;
			break;

		case hwr328PLLControlSysVCODiv8:
			result /= 8;
			break;

		case hwr328PLLControlSysVCODiv16:
			result /= 16;
			break;
	}

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetCanStop
// ---------------------------------------------------------------------------

Bool EmRegs328::GetCanStop (void)
{
	// Make sure Timer 2 is enabled or the RTC interrupt is enabled.

	if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnable) != 0)
		return true;

	if ((READ_REGISTER (rtcIntEnable) & hwr328RTCIntEnableAlarm) != 0)
		return true;

	return false;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetAsleep
// ---------------------------------------------------------------------------

Bool EmRegs328::GetAsleep (void)
{
	return ((READ_REGISTER (pllControl) & hwr328PLLControlDisable) != 0);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetPortInputValue
// ---------------------------------------------------------------------------
// Return the GPIO values for the pins on the port.  These values are used
// if the select pins are high.

uint8 EmRegs328::GetPortInputValue (int port)
{
	uint8	result = 0;

	if (port == 'D')
	{
		result = this->GetPortInternalValue (port);
	}

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetPortInternalValue
// ---------------------------------------------------------------------------
// Return the dedicated values for the pins on the port.  These values are
// used if the select pins are low.

uint8 EmRegs328::GetPortInternalValue (int port)
{
	uint8	result = 0;

	if (port == 'C')
	{
		// This makes the power on key work. If the signal is not asserted, the
		// unit will not transition between asleep and awake (cf. HwrSleep, HwrWake).

		result |= hwr328PortCNMI;
	}

	else if (port == 'D')
	{
		// If the ID_DETECT pin is asserted, load the data lines with the
		// hardware ID.

		if (EmRegs328::IDDetectAsserted ())
		{
			result |= EmRegs328::GetHardwareID ();
		}

		// Otherwise, load the lines with keyboard information.

		else
		{
			// Get the INT bits that need to be set.

			result |= this->GetKeyBits ();
		}
	}

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::PortDataChanged
// ---------------------------------------------------------------------------

void EmRegs328::PortDataChanged (int port, uint8, uint8 newValue)
{
	if (port == 'D')
	{
		// Clear the interrupt bits that are having a 1 written to them.
		// Only clear them if they're configured as edge-senstive.

		uint8	portDIntEdge = READ_REGISTER (portDIntEdge);

		PRINTF ("EmRegs328::PortDataChanged (D): fPortDEdge  = 0x%02lX", (uint32) (uint8) fPortDEdge);
		PRINTF ("EmRegs328::PortDataChanged (D): portDIntEdge = 0x%02lX", (uint32) (uint8) portDIntEdge);
		PRINTF ("EmRegs328::PortDataChanged (D): newValue     = 0x%02lX", (uint32) (uint8) newValue);

		fPortDEdge &= ~(newValue & portDIntEdge);

		PRINTF ("EmRegs328::PortDataChanged (D): fPortDEdge  = 0x%02lX", (uint32) (uint8) fPortDEdge);

		// Set the new interrupt state.

		EmRegs328::UpdatePortDInterrupts ();
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::pllFreqSelRead
// ---------------------------------------------------------------------------

uint32 EmRegs328::pllFreqSelRead (emuptr address, int size)
{
	// Simulate the rising and falling of the CLK32 signal so that functions
	// like HwrPreRAMInit, HwrShutDownPLL, PrvSetPLL, and PrvShutDownPLL
	// won't hang.

	uint16	pllFreqSel = READ_REGISTER (pllFreqSel) ^ 0x8000;
	WRITE_REGISTER (pllFreqSel, pllFreqSel);

	// Finish up by doing a standard read.

	return EmRegs328::StdRead (address, size);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::portXDataRead
// ---------------------------------------------------------------------------

uint32 EmRegs328::portXDataRead (emuptr address, int)
{
	// The value read can come from three different places:
	//
	//	- the value what was written to the data register
	//	- any dedicated inputs
	//	- any GPIO inputs
	//
	// The value returned depends on the settings of the SEL and DIR
	// registers.  So let's get those settings, the values from the three
	// input sources, and build up a return value based on those.

	int		port	= GetPort (address);

	uint8	sel		= StdRead (address + 2, 1);
	uint8	dir		= StdRead (address - 1, 1);
	uint8	output	= StdRead (address + 0, 1);
	uint8	input	= EmHAL::GetPortInputValue (port);
	uint8	intFn	= EmHAL::GetPortInternalValue (port);

	uint8	xsel	= sel;
	uint8	xdir	= dir;
	uint8	xoutput	= output;
	uint8	xinput	= input;
	uint8	xintFn	= intFn;

	if (port == 'D')
	{
		sel = 0xFF;		// No "select" bit in low nybble, so set for IO values.

		// The system will poll portD twice in KeyBootKeys to see
		// if any keys are down.  Wait at least that long before
		// letting up any boot keys maintained by the session.  When we
		// do call ReleaseBootKeys, set our counter to -1 as a flag not
		// to call it any more.

		if (fPortDDataCount != 0xFFFFFFFF && ++fPortDDataCount >= 2 * 2)
		{
			fPortDDataCount = 0xFFFFFFFF;
			gSession->ReleaseBootKeys ();
		}
	}

	// Use the internal chip function bits if the "sel" bits are zero.

	intFn &= ~sel;

	// Otherwise, use the I/O bits.

	output &= sel & dir;	// Use the output bits if the "dir" is one.
	input &= sel & ~dir;	// Use the input bits if the "dir" is zero.

	// Assert that there are no overlaps.

	EmAssert ((output & input) == 0);
	EmAssert ((output & intFn) == 0);
	EmAssert ((input & intFn) == 0);

	// Mush everything together.

	uint8	result = output | input | intFn;

	// If this is port D, flip the bits if the POLARITY register says to.
	// (!!! Does this inversion apply only to input bits?  That is, the
	// bits where the "dir" register has 0 bits?)

	if (0 && port == 'D')
	{
		uint8	polarity = READ_REGISTER (portDPolarity);
		PRINTF ("EmRegs328::portXDataRead: polarity = 0x%02lX", (uint32) polarity);
		result ^= polarity;
	}

	PRINTF ("EmRegs328::port%cDataRead: sel    dir    output input  intFn  result", (char) port);
	PRINTF ("EmRegs328::port%cDataRead: 0x%02lX   0x%02lX   0x%02lX   0x%02lX   0x%02lX   0x%02lX",
		(char) port, (uint32) xsel, (uint32) xdir, (uint32) xoutput, (uint32) xinput, (uint32) xintFn, (uint32) result);

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::tmr1StatusRead
// ---------------------------------------------------------------------------

uint32 EmRegs328::tmr1StatusRead (emuptr address, int size)
{
	uint16	tmr1Counter = READ_REGISTER (tmr1Counter) + 16;
	uint16	tmr1Compare = READ_REGISTER (tmr1Compare);
	uint16	tmr1Control = READ_REGISTER (tmr1Control);

	// Increment the timer.

	WRITE_REGISTER (tmr1Counter, tmr1Counter);

	// If the timer has passed the specified value...

	if ( (tmr1Counter - tmr1Compare) < 16 )
	{
		// Set the flag saying the timer timed out.

		uint16	tmr1Status = READ_REGISTER (tmr1Status) | hwr328TmrStatusCompare;
		WRITE_REGISTER (tmr1Status, tmr1Status);

		// If it's not a free-running timer, reset it to zero.

		if ((tmr1Control & hwr328TmrControlFreeRun) == 0)
		{
			WRITE_REGISTER (tmr1Counter, 0);
		}
	}

	// Remember this guy for later (see EmRegs328::tmr1StatusWrite())

	fLastTmr1Status |= READ_REGISTER (tmr1Status);

	// Finish up by doing a standard read.

	return EmRegs328::StdRead (address, size);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::tmr2StatusRead
// ---------------------------------------------------------------------------

uint32 EmRegs328::tmr2StatusRead (emuptr address, int size)
{
#if 0	// (Greg doesn't do this for Timer 2...I wonder why)

	/*
		ram_rom.cpp: DBReg::tmr2StatusRead: Hmm, I don't update the TMR2 count
		value. As with everything else that's missing in my DragonBall
		emulation, it's probably because I never found anything that needed it.
		If you know otherwise, by all means implement it. Also, the magic value
		of 16 counts per status read seemed to be about right for the programmed
		timer 1 frequency. It isn't likely to be right for timer 2.

						-- Greg
	*/

	uint16	tmr2Counter = READ_REGISTER (tmr2Counter) + 16;
	uint16	tmr2Compare = READ_REGISTER (tmr2Compare);
	uint16	tmr2Control = READ_REGISTER (tmr2Control);

	// Increment the timer.

	WRITE_REGISTER (tmr2Counter, tmr2Counter);

	// If the timer has passed the specified value...

	if ( (tmr2Counter - tmr2Compare) < 16 )
	{
		// Set the flag saying the timer timed out.

		uint16	tmr2Status = READ_REGISTER (tmr1Status) | hwr328TmrStatusCompare;
		WRITE_REGISTER (tmr2Status, tmr2Status);

		// If it's not a free-running timer, reset it to zero.

		if ( (tmr2Control & hwr328TmrControlFreeRun) == 0 )
			WRITE_REGISTER (tmr2Counter, 0);
	}
#endif

	fLastTmr2Status |= READ_REGISTER (tmr2Status);	// remember this guy for later (see EmRegs328::tmr2StatusWrite())

	// Finish up by doing a standard read.

	return EmRegs328::StdRead (address, size);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::uartRead
// ---------------------------------------------------------------------------

uint32 EmRegs328::uartRead (emuptr address, int size)
{
	// If this is a full read, get the next byte from the FIFO.

	Bool	refreshRxData = (address == addressof (uReceive)) && (size == 2);

	// See if there's anything new ("Put the data on the bus")

	EmRegs328::UpdateUARTState (refreshRxData);

	// Finish up by doing a standard read.

	return EmRegs328::StdRead (address, size);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::rtcHourMinSecRead
// ---------------------------------------------------------------------------

uint32 EmRegs328::rtcHourMinSecRead (emuptr address, int size)
{
	// Get the desktop machine's time.

	long	hour, min, sec;

	if (Hordes::IsOn ())
	{
		hour = fHour;
		min = fMin;
		sec = fSec;
	}
	else
	{
		::GetHostTime (&hour, &min, &sec);
	}

	// Update the register.

	WRITE_REGISTER (rtcHourMinSec, (hour << hwr328RTCHourMinSecHoursOffset)
								| (min << hwr328RTCHourMinSecMinutesOffset)
								| (sec << hwr328RTCHourMinSecSecondsOffset));

	// Finish up by doing a standard read.

	return EmRegs328::StdRead (address, size);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::csASelect1Write
// ---------------------------------------------------------------------------

void EmRegs328::csASelect1Write (emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Check its new state and update our ram-protect flag.

	gMemAccessFlags.fProtect_SRAMSet = (READ_REGISTER (csASelect1) & 0x0008) != 0;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::csCSelect0Write
// ---------------------------------------------------------------------------

void EmRegs328::csCSelect0Write (emuptr address, int size, uint32 value)
{
	uint32	csCSelect0 = READ_REGISTER (csCSelect0);

	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Check to see if the unprotected memory range changed.

	if ((csCSelect0 & ADDRESS_MASK) != (READ_REGISTER (csCSelect0) & ADDRESS_MASK))
	{
		EmAssert (gSession);
		gSession->ScheduleResetBanks ();
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::csCSelect1Write
// ---------------------------------------------------------------------------

void EmRegs328::csCSelect1Write (emuptr address, int size, uint32 value)
{
	uint32	csCSelect1 = READ_REGISTER (csCSelect1);

	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Check to see if the unprotected memory range changed.

	if ((csCSelect1 & ADDRESS_MASK) != (READ_REGISTER (csCSelect1) & ADDRESS_MASK))
	{
		EmAssert (gSession);
		gSession->ScheduleResetBanks ();
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::intMaskHiWrite
// ---------------------------------------------------------------------------

void EmRegs328::intMaskHiWrite (emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Respond to the new interrupt state.

	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::intMaskLoWrite
// ---------------------------------------------------------------------------

void EmRegs328::intMaskLoWrite (emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Respond to the new interrupt state.

	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::intStatusHiWrite
// ---------------------------------------------------------------------------

void EmRegs328::intStatusHiWrite (emuptr address, int size, uint32 value)
{
	// IRQ1, IRQ2, IRQ3, IRQ6 and IRQ7 are cleared by writing to their
	// respective status bits.  We handle those there. Since there are
	// no interrupt status bits like this in intStatusLo, we don't need
	// a handler for that register; we only handle intStatusHi.

	// Even though this is a 16-bit register as defined by the Palm headers,
	// it's a 32-bit register according to Dragonball docs, and is in fact
	// accessed that way in the kernal files (cf. HwrIRQ4Handler). In those
	// cases, we're still only interested in access to the IRQ# bits, so we
	// can turn 4-byte accesses into 2-byte accesses.

	if (size == 4)
		value >>= 16;

	// Take into account the possibility of 1-byte accesses, too. If we're
	// accessing the upper byte, just return. If we're accessing the lower
	// byte, we can treat it as a 2-byte access.

	else if (size == 1 && address == addressof (intStatusHi))
		return;

	// Now we can treat the rest of this function as a word-write to intStatusHi.

	uint16	intPendingHi = READ_REGISTER (intPendingHi);

	//	For each interrupt:
	//		If we're writing to that interrupt's status bit and its edge bit is set:
	//			- clear the interrupt's pending bit
	//			- respond to the new interrupt state.

	#undef CLEAR_PENDING_INTERRUPT
	#define CLEAR_PENDING_INTERRUPT(edge, irq)							\
		if ( (READ_REGISTER (intControl) & edge) && (value & (irq)) )	\
		{																\
			intPendingHi &= ~(irq);										\
		}

	CLEAR_PENDING_INTERRUPT (hwr328IntCtlEdge1, hwr328IntHiIRQ1);
	CLEAR_PENDING_INTERRUPT (hwr328IntCtlEdge2, hwr328IntHiIRQ2);
	CLEAR_PENDING_INTERRUPT (hwr328IntCtlEdge3, hwr328IntHiIRQ3);
	CLEAR_PENDING_INTERRUPT (hwr328IntCtlEdge6, hwr328IntHiIRQ6);

	// IRQ7 is not edge-programmable, so clear it if we're merely writing to it.

	if (value & hwr328IntHiNMI)
	{
		intPendingHi &= ~(hwr328IntHiNMI);
	}

	// If we're emulating the user pressing the hotsync button, make sure the
	// interrupt stays asserted.  (!!! Should we use the same technique for
	// other buttons, too?  It doesn't seem to be needed right now, but doing
	// that may more closely mirror the hardware.)

	if (fHotSyncButtonDown)
	{
		intPendingHi |= hwr328IntHiIRQ1;
	}
	else
	{
		intPendingHi &= ~hwr328IntHiIRQ1;
	}

	WRITE_REGISTER (intPendingHi, intPendingHi);
	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::portXDataWrite
// ---------------------------------------------------------------------------

void EmRegs328::portXDataWrite (emuptr address, int size, uint32 value)
{
	// Get the old value before updating it.

	uint8	oldValue	= StdRead (address, size);

	// Take a snapshot of the line driver states.

	Bool	driverStates[kUARTEnd];
	EmHAL::GetLineDriverStates (driverStates);

	// Now update the value with a standard write.

	StdWrite (address, size, value);

	// Let anyone know that it's changed.

	int		port = GetPort (address);
	PRINTF ("EmRegs328::port%cDataWrite: oldValue = 0x%02lX", (char) port, (uint32) (uint8) oldValue);
	PRINTF ("EmRegs328::port%cDataWrite: newValue = 0x%02lX", (char) port, (uint32) (uint8) value);

	EmHAL::PortDataChanged (port, oldValue, value);

	// Respond to any changes in the line driver states.

	EmHAL::CompareLineDriverStates (driverStates);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::portDIntReqEnWrite
// ---------------------------------------------------------------------------

void EmRegs328::portDIntReqEnWrite (emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Set the new interrupt state.

	EmRegs328::UpdatePortDInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::tmr1StatusWrite
// ---------------------------------------------------------------------------

void EmRegs328::tmr1StatusWrite (emuptr address, int size, uint32 value)
{
	UNUSED_PARAM(address)
	UNUSED_PARAM(size)

	EmAssert (size == 2);	// This function's a hell of a lot easier to write if
						// we assume only full-register access.

	// Get the current value.

	uint16	tmr1Status = READ_REGISTER (tmr1Status);

	// If the user had previously read the status bits while they
	// were set, then it's OK for them to be clear now.  Otherwise,
	// we have to merge any set status bits back in.

	tmr1Status &= value | ~fLastTmr1Status;	// fLastTmr1Status was set in EmRegs328::tmr1StatusRead()

	WRITE_REGISTER (tmr1Status, tmr1Status);

	fLastTmr1Status = 0;
	if ((tmr1Status & hwr328TmrStatusCompare) == 0)
	{
		uint16	intPendingHi = READ_REGISTER (intPendingHi) & ~hwr328IntHiTimer1;
		WRITE_REGISTER (intPendingHi, intPendingHi);

		// Respond to the new interrupt state.

		EmRegs328::UpdateInterrupts ();
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::tmr2StatusWrite
// ---------------------------------------------------------------------------

void EmRegs328::tmr2StatusWrite (emuptr address, int size, uint32 value)
{
	UNUSED_PARAM(address)
	UNUSED_PARAM(size)

	EmAssert (size == 2);	// This function's a hell of a lot easier to write if
						// we assume only full-register access.

	// Get the current value.

	uint16	tmr2Status = READ_REGISTER (tmr2Status);

	// If the user had previously read the status bits while they
	// were set, then it's OK for them to be clear now.  Otherwise,
	// we have to merge any set status bits back in.

	tmr2Status &= value | ~fLastTmr2Status;	// fLastTmr2Status was set in EmRegs328::tmr2StatusRead()

	WRITE_REGISTER (tmr2Status, tmr2Status);

	fLastTmr2Status = 0;
	if ( (tmr2Status & hwr328TmrStatusCompare) == 0 )
	{
		uint16	intPendingLo = READ_REGISTER (intPendingLo) & ~hwr328IntLoTimer2;
		WRITE_REGISTER (intPendingLo, intPendingLo);

		// Respond to the new interrupt state.

		EmRegs328::UpdateInterrupts ();
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::wdCounterWrite
// ---------------------------------------------------------------------------

void EmRegs328::wdCounterWrite (emuptr address, int size, uint32 value)
{
	UNUSED_PARAM(address)
	UNUSED_PARAM(size)
	UNUSED_PARAM(value)

	// Always set it to zero (a write to this register always resets it).

	WRITE_REGISTER (wdCounter, 0);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::spiMasterControlWrite
// ---------------------------------------------------------------------------

void EmRegs328::spiMasterControlWrite (emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Get the current value.

	uint16	spiMasterControl = READ_REGISTER (spiMasterControl);

	// Check to see if data exchange and interrupts are enabled.

	#define BIT_MASK (hwr328SPIMControlExchange | hwr328SPIMControlIntEnable)
	if ((spiMasterControl & BIT_MASK) != 0)
	{
		// If so, assert the interrupt and clear the exchange bit.

		spiMasterControl |= hwr328SPIMControlIntStatus;
		spiMasterControl &= ~hwr328SPIMControlExchange;

		WRITE_REGISTER (spiMasterControl, spiMasterControl);

/*
		// If we wanted digitizer data, load it into the SPIM data register.

		switch (READ_REGISTER (portFData) & hwrTD1PortFPanelMask)
		{
			case (hwrTD1PortFPanelCfgXMeas):
				WRITE_REGISTER (spiMasterData, (0xFF - Hardware::fgPen_HorzLocation) * 2);
				break;

			case (hwrTD1PortFPanelCfgYMeas):
				WRITE_REGISTER (spiMasterData, (0xFF - Hardware::fgPen_VertLocation) * 2);
				break;
		}
*/
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::uartWrite
// ---------------------------------------------------------------------------

void EmRegs328::uartWrite(emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// If this write included the TX_DATA field, signal that it needs to
	// be transmitted.

	Bool	sendTxData =
				((address == addressof (uTransmit)) && (size == 2)) ||
				((address == addressof (uTransmit) + 1) && (size == 1));

	// React to any changes.

	EmRegs328::UARTStateChanged (sendTxData);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::lcdRegisterWrite
// ---------------------------------------------------------------------------

void EmRegs328::lcdRegisterWrite(emuptr address, int size, uint32 value)
{
	// First, get the old value in case we need to see what changed.

	uint32	oldValue = EmRegs328::StdRead (address, size);

	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Note what changed.

	if (address == addressof (lcdScreenWidth))
	{
		EmScreen::InvalidateAll ();
	}
	else if (address == addressof (lcdScreenHeight))
	{
		EmScreen::InvalidateAll ();
	}
	else if (address == addressof (lcdPanelControl))
	{
		if (((value ^ oldValue) & hwr328LcdPanelControlGrayScale) != 0)
		{
			EmScreen::InvalidateAll ();
		}
	}
	else if (address == addressof (lcdStartAddr))
	{
		// Make sure the low-bit is always zero.

		uint32	lcdStartAddr = READ_REGISTER (lcdStartAddr) & 0xFFFFFFFE;
		WRITE_REGISTER (lcdStartAddr, lcdStartAddr);

		EmScreen::InvalidateAll ();
	}
	else if (address == addressof (lcdPageWidth))
	{
		if (value != oldValue)
		{
			EmScreen::InvalidateAll ();
		}
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::rtcControlWrite
// ---------------------------------------------------------------------------

void EmRegs328::rtcControlWrite(emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Respond to the new interrupt state.

	EmRegs328::UpdateRTCInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::rtcIntStatusWrite
// ---------------------------------------------------------------------------

void EmRegs328::rtcIntStatusWrite(emuptr address, int size, uint32 value)
{
	// Status bits are cleared by writing ones to them.

	// If we're doing a byte-write to the upper byte, shift the byte
	// so that we can treat the operation as a word write.  If we're
	// doing a byte-write to the lower byte, this extension will happen
	// automatically.

	if (address == addressof (rtcIntStatus) && size == 1)
		value <<= 8;

	// Get the current value.

	uint16	rtcIntStatus = READ_REGISTER (rtcIntStatus);

	// Clear the requested bits.

	rtcIntStatus &= ~value;

	// Update the register.

	WRITE_REGISTER (rtcIntStatus, rtcIntStatus);

	// Respond to the new interrupt state.

	EmRegs328::UpdateRTCInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::rtcIntEnableWrite
// ---------------------------------------------------------------------------

void EmRegs328::rtcIntEnableWrite(emuptr address, int size, uint32 value)
{
	// Do a standard update of the register.

	EmRegs328::StdWrite (address, size, value);

	// Respond to the new interrupt state.

	EmRegs328::UpdateRTCInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::ButtonEvent
// ---------------------------------------------------------------------------
// Handles a Palm device button event by updating the appropriate registers.

void EmRegs328::ButtonEvent (SkinElementType button, Bool buttonIsDown)
{
	uint16	bitNumber = this->ButtonToBits (button);

	// Get the bits that should have been set with the previous set
	// of pressed keys.  We use this old value to update the port D interrupts.

	uint8	oldBits = this->GetKeyBits ();

	// Update the set of keys that are currently pressed.

	if (buttonIsDown)
	{
		fKeyBits |= bitNumber;	// Remember the key bit
	}
	else
	{
		fKeyBits &= ~bitNumber;	// Forget the key bit
	}

	// Now get the new set of bits that should be set.

	uint8	newBits = this->GetKeyBits ();

	PRINTF ("EmRegs328::ButtonEvent: fKeyBits = 0x%04lX", (uint32) fKeyBits);
	PRINTF ("EmRegs328::ButtonEvent: oldBits   = 0x%02lX", (uint32) oldBits);
	PRINTF ("EmRegs328::ButtonEvent: newBits   = 0x%02lX", (uint32) newBits);

	// Set the interrupt bits for the bits that went from off to on.
	// These get cleared when portDData is written to.

	fPortDEdge |= newBits & ~oldBits;

	PRINTF ("EmRegs328::ButtonEvent: fPortDEdge = 0x%02lX", (uint32) fPortDEdge);

	// Set the new interrupt state.

	EmRegs328::UpdatePortDInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::HotSyncEvent
// ---------------------------------------------------------------------------
// Handles a HotSync button event by updating the appropriate registers.

void EmRegs328::HotSyncEvent (Bool iButton_IsDown)
{
	// If the button changes state, set or clear the HotSync interrupt.

	uint16	intPendingHi = READ_REGISTER (intPendingHi);

	if (iButton_IsDown)
	{
		intPendingHi |= hwr328IntHiIRQ1;
		fHotSyncButtonDown = true;
	}
	else
	{
		intPendingHi &= ~hwr328IntHiIRQ1;
		fHotSyncButtonDown = false;
	}

	WRITE_REGISTER (intPendingHi, intPendingHi);

	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetKeyBits
// ---------------------------------------------------------------------------

uint8 EmRegs328::GetKeyBits (void)
{
	// Return the key bits

	uint8	portDPullupEn	= READ_REGISTER (portDPullupEn);	// Interested where bits are one
	uint8	keyBits			= portDPullupEn & fKeyBits;

	PRINTF ("EmRegs328::GetKeyBits: keyBits = 0x%02lX", (uint32) keyBits);

	return keyBits;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::ButtonToBits
// ---------------------------------------------------------------------------

uint16 EmRegs328::ButtonToBits (SkinElementType button)
{
	uint16 bitNumber = 0;
	switch (button)
	{
		case kElement_None:				break;

		case kElement_PowerButton:		bitNumber = keyBitPower;	break;
		case kElement_UpButton: 		bitNumber = keyBitPageUp;	break;
		case kElement_DownButton:		bitNumber = keyBitPageDown; break;
		case kElement_App1Button:		bitNumber = keyBitHard1;	break;
		case kElement_App2Button:		bitNumber = keyBitHard2;	break;
		case kElement_App3Button:		bitNumber = keyBitHard3;	break;
		case kElement_App4Button:		bitNumber = keyBitHard4;	break;
		case kElement_CradleButton: 	bitNumber = keyBitCradle;	break;
		case kElement_Antenna:			bitNumber = keyBitAntenna;	break;
		case kElement_ContrastButton:	bitNumber = keyBitContrast; break;

/*
		// Symbol-specific
		case kElement_TriggerLeft:		bitNumber = keyBitTrigLeft;			break;
		case kElement_TriggerCenter:	bitNumber = keyBitTrigCenter;		break;
		case kElement_TriggerRight:		bitNumber = keyBitTrigRight;		break;
		case kElement_UpButtonLeft:		bitNumber = keyBitPageUpLeft;		break;
		case kElement_UpButtonRight:	bitNumber = keyBitPageUpRight;		break;
		case kElement_DownButtonLeft:	bitNumber = keyBitPageDownLeft;		break;
		case kElement_DownButtonRight:	bitNumber = keyBitPageDownRight;	break;
*/
		default:						EmAssert (false);
	}

	return bitNumber;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UpdateInterrupts
// ---------------------------------------------------------------------------
// Determines whether an interrupt has occurred by copying the Interrupt
// Pending Register to the Interrupt Status Register.

void EmRegs328::UpdateInterrupts (void)
{
	// Copy the Interrupt Pending Register to the Interrupt Status
	// Register, but ignore interrupts that are being masked.

	// Note: this function is not sensitive to the byte ordering of the registers,
	// so their contents don't need to be accessed via READ_REGISTER or WRITE_REGISTER.

	f68328Regs.intStatusHi = f68328Regs.intPendingHi & ~f68328Regs.intMaskHi;
	f68328Regs.intStatusLo = f68328Regs.intPendingLo & ~f68328Regs.intMaskLo;

	PRINTF ("EmRegs328::UpdateInterrupts: intMask    = 0x%04lX %04lX",
		(uint32) f68328Regs.intMaskHi, (uint32) f68328Regs.intMaskLo);

	PRINTF ("EmRegs328::UpdateInterrupts: intPending = 0x%04lX %04lX",
		(uint32) f68328Regs.intPendingHi, (uint32) f68328Regs.intPendingLo);

	// If the Interrupt Status Register isn't clear, flag an interrupt.

	if (f68328Regs.intStatusHi || f68328Regs.intStatusLo)
	{
		regs.spcflags |= SPCFLAG_INT;

		PRINTF ("EmRegs328::UpdateInterrupts: intStatus  = 0x%04lX %04lX",
			(uint32) f68328Regs.intStatusHi, (uint32) f68328Regs.intStatusLo);
	}
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UpdatePortDInterrupts
// ---------------------------------------------------------------------------
// Determine what interrupts need to be generated based on the current
// settings in portDData and fPortDEdge.

void EmRegs328::UpdatePortDInterrupts (void)
{
	// Update INT0-INT7 of the Interrupt-Pending register (bits 8-15 of the low word).

	// First, get those bits and clear them out.

	uint16	intPendingLo	= READ_REGISTER (intPendingLo) & ~hwr328IntLoAllKeys;


	// Initialize the variable to hold the new interrupt settings.

	uint8	newBits			= 0;


	// Get some other values we're going to need:

	uint8	portDDir		= READ_REGISTER (portDDir);	// Interrupt on inputs only (when pin is low)
	uint8	portDData		= EmHAL::GetPortInputValue ('D');
	uint8	portDPolarity	= READ_REGISTER (portDPolarity);
	uint8	portDIntReqEn	= READ_REGISTER (portDIntReqEn);
	uint8	portDIntEdge	= READ_REGISTER (portDIntEdge);


	// We have a line-level interrupt if:
	//
	//	- line-level interrupts are requested
	//	- the GPIO bit matches the polarity bit

	newBits |= ~portDIntEdge & portDData & portDPolarity;
	newBits |= ~portDIntEdge & ~portDData & ~portDPolarity;


	// We have an edge interrupt if:
	//
	//	- edge interrupts are requested
	//	- an edge has been recorded
	//
	// Note that we should distinguish between rising and falling edges.
	// For historical reasons, that's not done, and the Palm OS doesn't
	// look for them, so it's OK for now.

	newBits |= portDIntEdge & fPortDEdge & portDPolarity;
	newBits |= portDIntEdge & 0 & ~portDPolarity;


	// Only have interrupts if they're enabled and the pin is configured for input.

	newBits &= portDIntReqEn & ~portDDir;

	PRINTF ("EmRegs328::UpdatePortDInterrupts: Dir  Data Pol  Req  Edg  PDE  bits");
	PRINTF ("EmRegs328::UpdatePortDInterrupts: 0x%02lX 0x%02lX 0x%02lX 0x%02lX 0x%02lX 0x%02lX 0x%02lX",
		(uint32) portDDir, (uint32) portDData, (uint32) portDPolarity, (uint32) portDIntReqEn, (uint32) portDIntEdge,
		(uint32) fPortDEdge, (uint32) newBits);


	// Merge in the new values and write out the result.

	intPendingLo |=	(((uint16) newBits) << 8) & hwr328IntLoAllKeys;
	WRITE_REGISTER (intPendingLo, intPendingLo);


	// Respond to the new interrupt state.

	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UpdateRTCInterrupts
// ---------------------------------------------------------------------------
// Determine whether to set or clear the RTC bit in the interrupt pending
// register based on the current RTC register values.

void EmRegs328::UpdateRTCInterrupts (void)
{
	// See if the RTC is enabled.

	Bool	rtcEnabled = (READ_REGISTER (rtcControl) & hwr328RTCControlRTCEnable) != 0;

	// See if there are any RTC events that need to trigger an interrupt.

#define BITS_TO_CHECK				( \
	hwr328RTCIntEnableSec			| \
	hwr328RTCIntEnable24Hr			| \
	hwr328RTCIntEnableAlarm			| \
	hwr328RTCIntEnableMinute		| \
	hwr328RTCIntEnableStopWatch		)

	uint16	rtcIntStatus = READ_REGISTER (rtcIntStatus);
	uint16	rtcIntEnable = READ_REGISTER (rtcIntEnable);
	uint16	rtcIntPending = rtcIntStatus & rtcIntEnable & BITS_TO_CHECK;

	Bool	havePendingEvents = rtcIntPending != 0;

	// If the RTC is enabled and there are pending events, set the interrupt.
	// Otherwise, clear the interrupt.

	uint16	intPendingLo = READ_REGISTER (intPendingLo);

	if (rtcEnabled && havePendingEvents)
	{
		intPendingLo |= hwr328IntLoRTC;		// have events, so set interrupt
	}
	else
	{
		intPendingLo &= ~hwr328IntLoRTC;	// no events, so clear interrupt
	}

	// Update the interrupt pending register.

	WRITE_REGISTER (intPendingLo, intPendingLo);

	// Respond to the new interrupt state.

	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::IDDetectAsserted
// ---------------------------------------------------------------------------
// cf. HwrIdentifyFeatures and HwrPreRAMInit.

Bool EmRegs328::IDDetectAsserted (void)
{
			uint8	portEDir		= READ_REGISTER (portEDir);
			uint8	portEData		= READ_REGISTER (portEData);
			uint8	portEPullupEn	= READ_REGISTER (portEPullupEn);
	const	uint8	kMask			= hwrTD1PortENoBacklight;

	return	(portEDir & kMask) == kMask &&
			(portEData & kMask) == 0 &&
			(portEPullupEn & kMask) == 0;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetHardwareID
// ---------------------------------------------------------------------------

UInt8 EmRegs328::GetHardwareID (void)
{
	// Determine the hardware ID.

	// Note: Because of a Poser bug, I don't think any of the following actually
	// gets executed.  328 ROMs first check to see if they're on a PalmPilot
	// by executing the following:
	//
	//			if (!(baseP->portEData & hwrTD1PortENoBacklight)) {
	//				...on PalmPilot...
	//			} else {
	//				...execute ID DETECT...
	//			}
	//
	// In Poser, hwrTD1PortENoBacklight is always zero (that's the bug), and
	// so we always think we're on a PalmPilot, regardless of what 328 device
	// we selected.

	EmAssert (gSession);

	EmDevice	device		= gSession->GetDevice ();
	long		miscFlags	= device.HardwareID ();

	// Reverse map the following:
//	GHwrMiscFlags = 0;
//	if ( (keyState & keyBitHard1) == 0) GHwrMiscFlags |= hwrMiscFlagID1;
//	if ( (keyState & keyBitHard2) == 0) GHwrMiscFlags |= hwrMiscFlagID2;
//	if ( (keyState & keyBitHard3) == 0) GHwrMiscFlags |= hwrMiscFlagID3;
//	if ( (keyState & keyBitHard4) == 0) GHwrMiscFlags |= hwrMiscFlagID4;

	uint8	keyState = ~0;

	if ((miscFlags & hwrMiscFlagID1) != 0)	keyState &= ~keyBitHard1;
	if ((miscFlags & hwrMiscFlagID2) != 0)	keyState &= ~keyBitHard2;
	if ((miscFlags & hwrMiscFlagID3) != 0)	keyState &= ~keyBitHard3;
	if ((miscFlags & hwrMiscFlagID4) != 0)	keyState &= ~keyBitHard4;

	return keyState;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UARTStateChanged
// ---------------------------------------------------------------------------

void EmRegs328::UARTStateChanged (Bool sendTxData)
{
	EmUARTDragonball::State	state (EmUARTDragonball::kUART_Dragonball);

	EmRegs328::MarshalUARTState (state);
	fUART->StateChanged (state, sendTxData);
	EmRegs328::UnmarshalUARTState (state);

	EmRegs328::UpdateUARTInterrupts (state);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UpdateUARTState
// ---------------------------------------------------------------------------

void EmRegs328::UpdateUARTState (Bool refreshRxData)
{
	EmUARTDragonball::State	state (EmUARTDragonball::kUART_Dragonball);

	EmRegs328::MarshalUARTState (state);
	fUART->UpdateState (state, refreshRxData);
	UnmarshalUARTState (state);

	EmRegs328::UpdateUARTInterrupts (state);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UpdateUARTInterrupts
// ---------------------------------------------------------------------------

void EmRegs328::UpdateUARTInterrupts (const EmUARTDragonball::State& state)
{
	// Generate the appropriate interrupts.

	if (state.RX_FULL_ENABLE	&& state.RX_FIFO_FULL	||
		state.RX_HALF_ENABLE	&& state.RX_FIFO_HALF	||
		state.RX_RDY_ENABLE		&& state.DATA_READY		||
		state.TX_EMPTY_ENABLE	&& state.TX_FIFO_EMPTY	||
		state.TX_HALF_ENABLE	&& state.TX_FIFO_HALF	||
		state.TX_AVAIL_ENABLE	&& state.TX_AVAIL)
	{
		// Set the UART interrupt.

		WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwr328IntLoUART);
	}
	else
	{
		// Clear the UART interrupt.

		WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) & ~hwr328IntLoUART);
	}

	// Respond to the new interrupt state.

	EmRegs328::UpdateInterrupts ();
}


// ---------------------------------------------------------------------------
//		� EmRegs328::MarshalUARTState
// ---------------------------------------------------------------------------

void EmRegs328::MarshalUARTState (EmUARTDragonball::State& state)
{
	uint16	uControl		= READ_REGISTER (uControl);
	uint16	uBaud			= READ_REGISTER (uBaud);
	uint16	uReceive		= READ_REGISTER (uReceive);
	uint16	uTransmit		= READ_REGISTER (uTransmit);
	uint16	uMisc			= READ_REGISTER (uMisc);

	state.UART_ENABLE		= (uControl & hwr328UControlUARTEnable) != 0;
	state.RX_ENABLE			= (uControl & hwr328UControlRxEnable) != 0;
	state.TX_ENABLE			= (uControl & hwr328UControlTxEnable) != 0;
	state.RX_CLK_CONT		= (uControl & hwr328UControlRxClock1x) != 0;
	state.PARITY_EN			= (uControl & hwr328UControlParityEn) != 0;
	state.ODD_EVEN			= (uControl & hwr328UControlParityOdd) != 0;
	state.STOP_BITS			= (uControl & hwr328UControlStopBits2) != 0;
	state.CHAR8_7			= (uControl & hwr328UControlDataBits8) != 0;
	state.GPIO_DELTA_ENABLE	= (uControl & hwr328UControlGPIODeltaEn) != 0;	// 68328 only
//	state.OLD_ENABLE		= (uControl & hwrEZ328UControlOldDataEn) != 0;	// 68EZ328 only
	state.CTS_DELTA_ENABLE	= (uControl & hwr328UControlCTSDeltaEn) != 0;
	state.RX_FULL_ENABLE	= (uControl & hwr328UControlRxFullEn) != 0;
	state.RX_HALF_ENABLE	= (uControl & hwr328UControlRxHalfEn) != 0;
	state.RX_RDY_ENABLE		= (uControl & hwr328UControlRxRdyEn) != 0;
	state.TX_EMPTY_ENABLE	= (uControl & hwr328UControlTxEmptyEn) != 0;
	state.TX_HALF_ENABLE	= (uControl & hwr328UControlTxHalfEn) != 0;
	state.TX_AVAIL_ENABLE	= (uControl & hwr328UControlTxAvailEn) != 0;

	// Baud control register bits
	// These are all values the user sets; we just look at them.

	state.GPIO_DELTA		= (uBaud & hwr328UBaudGPIODelta) != 0;			// 68328 only
	state.GPIO				= (uBaud & hwr328UBaudGPIOData) != 0;			// 68328 only
	state.GPIO_DIR			= (uBaud & hwr328UBaudGPIODirOut) != 0;			// 68328 only
	state.GPIO_SRC			= (uBaud & hwr328UBaudGPIOSrcBaudGen) != 0;		// 68328 only
//	state.UCLK_DIR			= (uBaud & hwrEZ328UBaudUCLKDirOut) != 0;		// 68EZ328 only
	state.BAUD_SRC			= (uBaud & hwr328UBaudBaudSrcGPIO) != 0;
	state.DIVIDE			= (uBaud & hwr328UBaudDivider) >> hwr328UBaudDivideBitOffset;
	state.PRESCALER			= (uBaud & hwr328UBaudPrescaler);

	// Receive register bits
	// These are all input bits; we set them, not the user.

	state.RX_FIFO_FULL		= (uReceive & hwr328UReceiveFIFOFull) != 0;
	state.RX_FIFO_HALF		= (uReceive & hwr328UReceiveFIFOHalf) != 0;
	state.DATA_READY		= (uReceive & hwr328UReceiveDataRdy) != 0;
//	state.OLD_DATA			= (uReceive & hwrEZ328UReceiveOldData) != 0;	// 68EZ328 only
	state.OVRUN				= (uReceive & hwr328UReceiveOverrunErr) != 0;
	state.FRAME_ERROR		= (uReceive & hwr328UReceiveFrameErr) != 0;
	state.BREAK				= (uReceive & hwr328UReceiveBreakErr) != 0;
	state.PARITY_ERROR		= (uReceive & hwr328UReceiveParityErr) != 0;
	state.RX_DATA			= (uReceive & hwr328UReceiveData);

	// Transmitter register bits
	// We set everything except TX_DATA; the user sets that
	// value and ONLY that value.

	state.TX_FIFO_EMPTY		= (uTransmit & hwr328UTransmitFIFOEmpty) != 0;
	state.TX_FIFO_HALF		= (uTransmit & hwr328UTransmitFIFOHalf) != 0;
	state.TX_AVAIL			= (uTransmit & hwr328UTransmitTxAvail) != 0;
	state.SEND_BREAK		= (uTransmit & hwr328UTransmitSendBreak) != 0;
	state.IGNORE_CTS		= (uTransmit & hwr328UTransmitIgnoreCTS) != 0;
//	state.BUSY				= (uTransmit & hwrEZ328UTransmitBusy) != 0;		// 68EZ328 only
	state.CTS_STATUS		= (uTransmit & hwr328UTransmitCTSStatus) != 0;
	state.CTS_DELTA			= (uTransmit & hwr328UTransmitCTSDelta) != 0;
	state.TX_DATA			= (uTransmit & hwr328UTransmitData);

	// Misc register bits
	// These are all values the user sets; we just look at them.

//	state.BAUD_TEST			= (uMisc & hwrEZ328UMiscBaudTest) != 0;			// 68EZ328 only
	state.CLK_SRC			= (uMisc & hwr328UMiscClkSrcGPIO) != 0;
	state.FORCE_PERR		= (uMisc & hwr328UMiscForceParityErr) != 0;
	state.LOOP				= (uMisc & hwr328UMiscLoopback) != 0;
//	state.BAUD_RESET		= (uMisc & hwrEZ328UMiscBaudReset) != 0;		// 68EZ328 only
//	state.IR_TEST			= (uMisc & hwrEZ328UMiscIRTestEn) != 0;			// 68EZ328 only
	state.RTS_CONT			= (uMisc & hwr328UMiscRTSThruFIFO) != 0;
	state.RTS				= (uMisc & hwr328UMiscRTSOut) != 0;
	state.IRDA_ENABLE		= (uMisc & hwr328UMiscIRDAEn) != 0;
	state.IRDA_LOOP			= (uMisc & hwr328UMiscLoopIRDA) != 0;
//	state.RX_POL			= (uMisc & hwrEZ328UMiscRXPolarityInv) != 0;	// 68EZ328 only
//	state.TX_POL			= (uMisc & hwrEZ328UMiscTXPolarityInv) != 0;	// 68EZ328 only
}


// ---------------------------------------------------------------------------
//		� EmRegs328::UnmarshalUARTState
// ---------------------------------------------------------------------------

void EmRegs328::UnmarshalUARTState (const EmUARTDragonball::State& state)
{
	uint16	uControl	= 0;
	uint16	uBaud		= 0;
	uint16	uReceive	= 0;
	uint16	uTransmit	= 0;
	uint16	uMisc		= 0;

	if (state.UART_ENABLE)		uControl |= hwr328UControlUARTEnable;
	if (state.RX_ENABLE)		uControl |= hwr328UControlRxEnable;
	if (state.TX_ENABLE)		uControl |= hwr328UControlTxEnable;
	if (state.RX_CLK_CONT)		uControl |= hwr328UControlRxClock1x;
	if (state.PARITY_EN)		uControl |= hwr328UControlParityEn;
	if (state.ODD_EVEN)			uControl |= hwr328UControlParityOdd;
	if (state.STOP_BITS)		uControl |= hwr328UControlStopBits2;
	if (state.CHAR8_7)			uControl |= hwr328UControlDataBits8;
	if (state.GPIO_DELTA_ENABLE)uControl |= hwr328UControlGPIODeltaEn;	// 68328 only
//	if (state.OLD_ENABLE)		uControl |= hwrEZ328UControlOldDataEn;	// 68EZ328 only
	if (state.CTS_DELTA_ENABLE)	uControl |= hwr328UControlCTSDeltaEn;
	if (state.RX_FULL_ENABLE)	uControl |= hwr328UControlRxFullEn;
	if (state.RX_HALF_ENABLE)	uControl |= hwr328UControlRxHalfEn;
	if (state.RX_RDY_ENABLE)	uControl |= hwr328UControlRxRdyEn;
	if (state.TX_EMPTY_ENABLE)	uControl |= hwr328UControlTxEmptyEn;
	if (state.TX_HALF_ENABLE)	uControl |= hwr328UControlTxHalfEn;
	if (state.TX_AVAIL_ENABLE)	uControl |= hwr328UControlTxAvailEn;

	// Baud control register bits
	// These are all values the user sets; we just look at them.

	if (state.GPIO_DELTA)		uBaud |= hwr328UBaudGPIODelta;		// 68328 only
	if (state.GPIO)				uBaud |= hwr328UBaudGPIOData;		// 68328 only
	if (state.GPIO_DIR)			uBaud |= hwr328UBaudGPIODirOut;		// 68328 only
	if (state.GPIO_SRC)			uBaud |= hwr328UBaudGPIOSrcBaudGen;	// 68328 only
//	if (state.UCLK_DIR)			uBaud |= hwrEZ328UBaudUCLKDirOut;	// 68EZ328 only
	if (state.BAUD_SRC)			uBaud |= hwr328UBaudBaudSrcGPIO;

	uBaud |= (state.DIVIDE << hwr328UBaudDivideBitOffset) & hwr328UBaudDivider;
	uBaud |= (state.PRESCALER) & hwr328UBaudPrescaler;

	// Receive register bits
	// These are all input bits; we set them, not the user.

	if (state.RX_FIFO_FULL)		uReceive |= hwr328UReceiveFIFOFull;
	if (state.RX_FIFO_HALF)		uReceive |= hwr328UReceiveFIFOHalf;
	if (state.DATA_READY)		uReceive |= hwr328UReceiveDataRdy;
//	if (state.OLD_DATA)			uReceive |= hwrEZ328UReceiveOldData;	// 68EZ328 only
	if (state.OVRUN)			uReceive |= hwr328UReceiveOverrunErr;
	if (state.FRAME_ERROR)		uReceive |= hwr328UReceiveFrameErr;
	if (state.BREAK)			uReceive |= hwr328UReceiveBreakErr;
	if (state.PARITY_ERROR)		uReceive |= hwr328UReceiveParityErr;

	uReceive |= (state.RX_DATA) & hwr328UReceiveData;

	// Transmitter register bits
	// We set everything except TX_DATA; the user sets that
	// value and ONLY that value.

	if (state.TX_FIFO_EMPTY)	uTransmit |= hwr328UTransmitFIFOEmpty;
	if (state.TX_FIFO_HALF)		uTransmit |= hwr328UTransmitFIFOHalf;
	if (state.TX_AVAIL)			uTransmit |= hwr328UTransmitTxAvail;
	if (state.SEND_BREAK)		uTransmit |= hwr328UTransmitSendBreak;
	if (state.IGNORE_CTS)		uTransmit |= hwr328UTransmitIgnoreCTS;
//	if (state.BUSY)				uTransmit |= hwrEZ328UTransmitBusy;		// 68EZ328 only
	if (state.CTS_STATUS)		uTransmit |= hwr328UTransmitCTSStatus;
	if (state.CTS_DELTA)		uTransmit |= hwr328UTransmitCTSDelta;

	uTransmit |= (state.TX_DATA) & hwr328UTransmitData;

	// Misc register bits
	// These are all values the user sets; we just look at them.

//	if (state.BAUD_TEST)		uMisc |= hwrEZ328UMiscBaudTest;			// 68EZ328 only
	if (state.CLK_SRC)			uMisc |= hwr328UMiscClkSrcGPIO;
	if (state.FORCE_PERR)		uMisc |= hwr328UMiscForceParityErr;
	if (state.LOOP)				uMisc |= hwr328UMiscLoopback;
//	if (state.BAUD_RESET)		uMisc |= hwrEZ328UMiscBaudReset;		// 68EZ328 only
//	if (state.IR_TEST)			uMisc |= hwrEZ328UMiscIRTestEn;			// 68EZ328 only
	if (state.RTS_CONT)			uMisc |= hwr328UMiscRTSThruFIFO;
	if (state.RTS)				uMisc |= hwr328UMiscRTSOut;
	if (state.IRDA_ENABLE)		uMisc |= hwr328UMiscIRDAEn;
	if (state.IRDA_LOOP)		uMisc |= hwr328UMiscLoopIRDA;
//	if (state.RX_POL)			uMisc |= hwrEZ328UMiscRXPolarityInv;	// 68EZ328 only
//	if (state.TX_POL)			uMisc |= hwrEZ328UMiscTXPolarityInv;	// 68EZ328 only

	WRITE_REGISTER (uControl, uControl);
	WRITE_REGISTER (uBaud, uBaud);
	WRITE_REGISTER (uReceive, uReceive);
	WRITE_REGISTER (uTransmit, uTransmit);
	WRITE_REGISTER (uMisc, uMisc);
}


// ---------------------------------------------------------------------------
//		� EmRegs328::GetPort
// ---------------------------------------------------------------------------
// Given an address, return a value indicating what port it is associated with.

int EmRegs328::GetPort (emuptr address)
{
	const long	MASK = 0x00000FF8;

	switch (address & MASK)
	{
		case 0x0400:	return 'A';
		case 0x0408:	return 'B';
		case 0x0410:	return 'C';
		case 0x0418:	return 'D';
		case 0x0420:	return 'E';
		case 0x0428:	return 'F';
		case 0x0430:	return 'G';
		case 0x0438:	return 'J';
		case 0x0440:	return 'K';
		case 0x0448:	return 'M';
	}

	EmAssert (false);
	return 0;
}


// ---------------------------------------------------------------------------
//		� EmRegs328::PrvGetPalette
// ---------------------------------------------------------------------------

void EmRegs328::PrvGetPalette (RGBList& thePalette)
{
	// !!! TBD
	Preference<RGBType> pref1 (kPrefKeyBackgroundColor);
	Preference<RGBType> pref2 (kPrefKeyHighlightColor);

	RGBType foreground (0, 0, 0);
	RGBType background;

	if (this->GetLCDBacklightOn ())
	{
		if (pref2.Loaded ())
			background = *pref2;
		else
			background = ::SkinGetHighlightColor ();
	}
	else
	{
		if (pref1.Loaded ())
			background = *pref1;
		else
			background = ::SkinGetBackgroundColor ();
	}

	long	br = ((long) background.fRed);
	long	bg = ((long) background.fGreen);
	long	bb = ((long) background.fBlue);

	long	dr = ((long) foreground.fRed) - ((long) background.fRed);
	long	dg = ((long) foreground.fGreen) - ((long) background.fGreen);
	long	db = ((long) foreground.fBlue) - ((long) background.fBlue);

	int32	bpp			= 1 << (READ_REGISTER (lcdPanelControl) & 0x01);
	int32	numColors	= 1 << bpp;
	thePalette.resize (numColors);

	for (int color = 0; color < numColors; ++color)
	{
		thePalette[color].fRed		= (UInt8) (br + dr * color / (numColors - 1));
		thePalette[color].fGreen	= (UInt8) (bg + dg * color / (numColors - 1));
		thePalette[color].fBlue 	= (UInt8) (bb + db * color / (numColors - 1));
	}
}


