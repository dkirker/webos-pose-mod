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
#include "EmRegs330CPLD.h"

#include "EmHAL.h"				// EmHAL::LineDriverChanged
#include "EmHandEra330Defs.h"
#include "EmMemory.h"
#include "EmScreen.h"			// EmScreenUpdateInfo

/*****************************************************************************
 * The HandEra 330 uses a CPLD for additional GPIO
 * This includes:
 * CF & SD bus control, keyboard row drivers, LCD control, Backlight, IRDA,
 * RS232 DTR signal, current vs voltage sense, and audio control.
 *
 * POSE needs to know about LCD, backlight, keyboard, and current sense so these
 * are stored in an external HandEra330PortManager class that the EmRegsVZHandEra330
 * class can get the values from.
 ****************************************************************************/

// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::EmRegs330CPLD
// ---------------------------------------------------------------------------
EmRegs330CPLD::EmRegs330CPLD (HandEra330PortManager * fPortManager)
{
    Reg0 = Cpld0Edo;
    Reg2 = Cpld2CfDetect |
           Cpld2NoSdDetect |
           Cpld2NoExPwrDetect |
           Cpld2SdUnwriteProt |
           Cpld2SdPowerOff |
           Cpld2Kbd3Inactive |
           Cpld2Kbd2Inactive |
           Cpld2Kbd1Inactive |
           Cpld2Kbd0Inactive |
           Cpld2NoReset |
           Cpld2CfBufsOff |
           Cpld2SwapOff |
           Cpld2CfPowerOff |
           Cpld2BusWidth16;
     Reg4 = Cpld4LcdBiasOff |
            Cpld4LcdVccOn |
            Cpld4LcdOff |
            Cpld4BlPdOff |
            Cpld4DtrOff |
            Cpld4MmcCsOn |
            Cpld4FiltSdOff |
            Cpld4Rs232Off |
            Cpld4IrdaOff |
            Cpld4MicOff |
            Cpld4SenseVoltage;
     fPortMgr = fPortManager;
}


// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::~EmRegs330CPLD
// ---------------------------------------------------------------------------

EmRegs330CPLD::~EmRegs330CPLD (void)
{
}

void EmRegs330CPLD::Reset(Bool /*hardwareReset*/)
{
	fPortMgr->Keys.Row[0] = 1;
	fPortMgr->Keys.Row[1] = 1;
	fPortMgr->Keys.Row[2] = 1;
	fPortMgr->Keys.Row[3] = 1;
	fPortMgr->LCDOn = false;
	fPortMgr->BacklightOn = false;
	fPortMgr->IRPortOn = false;
	fPortMgr->CFBus.bEnabled  = false;
	fPortMgr->CFBus.Width     = kCFBusWidth16;
	fPortMgr->CFBus.bSwapped  = false;
    fPortMgr->pendingIRQ2 = false;
    fPortMgr->SDChipSelect = false;
    fPortMgr->PowerConnected = false;
}

// -------------------------------------------------------------------------
//		� EmRegs330CPLD::SetSubBankHandlers
// ---------------------------------------------------------------------------
void EmRegs330CPLD::SetSubBankHandlers (void)
{
	// Install base handlers.

	EmRegs::SetSubBankHandlers();
	// Now add standard/specialized handers for the defined registers.
	this->SetHandler((ReadFunction)&EmRegs330CPLD::Read,
                     (WriteFunction)&EmRegs330CPLD::Write,
                      kMemoryStartCPLD,
                      kMemorySizeCPLD);
}

// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::GetRealAddress
// ---------------------------------------------------------------------------
uint8 * EmRegs330CPLD::GetRealAddress (emuptr address)
{
	return (address-kMemoryStartCPLD) + Buffer;
}

// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::GetWord
// ---------------------------------------------------------------------------
uint32 EmRegs330CPLD::GetWord (emuptr address)
{
   uint32 offset = address - kMemoryStartCPLD;

   switch (offset)
   {
       case CpldReg00 :
           return Cpld0Edo;

       case CpldReg02 :
           if (fPortMgr->CFInserted)
               Reg2 &= ~Cpld2NoCfDetect;
           else
               Reg2 |= Cpld2NoCfDetect;
           if (fPortMgr->SDInserted)
               Reg2 &= ~Cpld2NoSdDetect;
           else
               Reg2 |= Cpld2NoSdDetect;
           if (fPortMgr->PowerConnected)
               Reg2 &= ~Cpld2NoExPwrDetect;
           else
               Reg2 |= Cpld2NoExPwrDetect;
           return (Reg2);

       case CpldReg04 :
           return (Reg4);

       default :
           return 0;
    }
}


// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::SetWord
// ---------------------------------------------------------------------------
void EmRegs330CPLD::SetWord (emuptr address, uint32 val)
{
    uint32 offset = address - kMemoryStartCPLD;

    switch (offset)
    {
        case CpldReg00 :
            fPortMgr->pendingIRQ2 = false;
            break;

        case CpldReg02 :
            Reg2 = (val & 0x0fff) | (Reg2 & 0xf000); // top 4 bits are read only.
            
            // keys:
            fPortMgr->Keys.Row[3] = (Reg2 & Cpld2Kbd3Inactive) ? 0 : 1;
            fPortMgr->Keys.Row[2] = (Reg2 & Cpld2Kbd2Inactive) ? 0 : 1;
	        fPortMgr->Keys.Row[1] = (Reg2 & Cpld2Kbd1Inactive) ? 0 : 1;
            fPortMgr->Keys.Row[0] = (Reg2 & Cpld2Kbd0Inactive) ? 0 : 1;

            // CF:
            fPortMgr->CFBus.bEnabled = (Reg2 & Cpld2CfBufsOn);
            if (Reg2 & Cpld2BusWidth8)
            {
                fPortMgr->CFBus.Width = kCFBusWidth8;

                // HandEra330 byte swapping is not the same as the TRGpro when
                // in byte mode.  On the TRGpro it must be enabled to access bytes
                // correctly, on the 330 it will work either way...
                // Fudge it here to be the same as TRGpro so code can be shared.
                fPortMgr->CFBus.bSwapped = true;
            }
            else
            {
                fPortMgr->CFBus.Width = kCFBusWidth16;
                fPortMgr->CFBus.bSwapped = (Reg2 & Cpld2SwapOn);
            }

            break;

        case CpldReg04 :
            Bool	backlightWasOn	= fPortMgr->BacklightOn;
            Bool	lcdWasOn		= fPortMgr->LCDOn;
            Bool	irWasOn			= fPortMgr->IRPortOn;

            Reg4 = val;

            fPortMgr->LCDOn = (Reg4 & Cpld4LcdBiasOn);
	        fPortMgr->BacklightOn = (Reg4 & Cpld4BlPdOn);
	        fPortMgr->IRPortOn = (Reg4 & Cpld4IrdaOff) == 0;
	        fPortMgr->SenseCurrent = (Reg4 & Cpld4SenseCurrent);
            fPortMgr->SDChipSelect = ((Reg4 & Cpld4MmcCsOn) == 0);

        	if ((fPortMgr->LCDOn != lcdWasOn) || (fPortMgr->BacklightOn != backlightWasOn))
		        EmScreen::InvalidateAll ();

			if (irWasOn != fPortMgr->IRPortOn)
				EmHAL::LineDriverChanged (kUARTIR);

            break;
    }
}


// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::GetAddressStart
// ---------------------------------------------------------------------------
emuptr EmRegs330CPLD::GetAddressStart (void)
{
	return (kMemoryStartCPLD);
}

// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::GetAddressRange
// ---------------------------------------------------------------------------
uint32 EmRegs330CPLD::GetAddressRange (void)
{
	return kMemorySizeCPLD;
}

// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::Read
// ---------------------------------------------------------------------------
uint32 EmRegs330CPLD::Read(emuptr address, int size)
{
	switch(size)
	{
//		case sizeof(uint8) :
//			return(GetByte(address));
		case sizeof(uint16) :
			return(GetWord(address));
	}
	return 0;
}

// ---------------------------------------------------------------------------
//		� EmRegs330CPLD::Write
// ---------------------------------------------------------------------------
void EmRegs330CPLD::Write(emuptr address, int size, uint32 val)
{
	switch(size)
	{
//		case sizeof(uint8) :
//			SetByte(address, val);
//			break;
		case sizeof(uint16) :
			SetWord(address, val);
			break;
	}
}

