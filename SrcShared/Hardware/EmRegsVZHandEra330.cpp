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
#include "EmRegsVZTemp.h"
#include "EmRegsVZPrv.h"
#include "EmTRGSD.h"
#include "EmRegsVZHandEra330.h"
#include "EmRegs330CPLD.h"
#include "EmBankRegs.h"			// EmBankRegs::DisableSubBank
#include "EmSPISlaveADS784x.h"	// EmSPISlaveADS784x
#include "EmSPISlave330Current.h"
#include "EmScreen.h"			// EmScreenUpdateInfo
#include "EmDlg.h"				// EmDlg::DoCommonDialog


#pragma mark -

const int		kNumButtonRows = 4;
const int		kNumButtonCols = 4;

const uint16	kButtonMap[kNumButtonRows][kNumButtonCols] =
{
	{ keyBitHard1,	keyBitHard2,	keyBitHard3,	keyBitHard4 },
	{ keyBitPageUp,	keyBitPageDown,	0,	            keyBitThumbDown},
	{ keyBitPower,	0,              keyBitContrast, keyBitThumbPush},
	{ 0,            0,              0,              keyBitThumbUp}, 
};


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::EmRegsVZHandEra330
// ---------------------------------------------------------------------------

EmRegsVZHandEra330::EmRegsVZHandEra330 (HandEra330PortManager ** fPortManager) :
	EmRegsVZ (),
	fSPISlaveADC (new EmSPISlaveADS784x (kChannelSet2)),
    fSPISlaveCurrent (new EmSPISlave330Current ())
{
    PortD = PortD_DOCK_BTN |
            PortD_CD_IRQ |
            PortD_CF_IRQ |
            PortD_POWER_FAIL;
    PortF = PortF_PEN_IO |
            PortF_CPLD_CS_F;
    PortG = PortG_DTACK |
            PortG_A0 |
            PortG_Unused |
            // PortG_LION |
            PortG_Unused2;
    PortJ = PortJ_AD_CS;
    PortK = PortK_LED_GREEN |
            PortK_LED_RED |
            PortK_CPLD_TDO |
            PortK_CPLD_TCK;
    PortM = PortM_CPLD_TDI;
    *fPortManager = &PortMgr;
	PortMgr.Keys.Row[0] = 1;
	PortMgr.Keys.Row[1] = 1;
	PortMgr.Keys.Row[2] = 1;
	PortMgr.Keys.Row[3] = 1;
	PortMgr.LCDOn = false;
	PortMgr.BacklightOn = false;
	PortMgr.IRPortOn = false;
	PortMgr.CFBus.bEnabled  = false;
	PortMgr.CFBus.Width     = kCFBusWidth16;
	PortMgr.CFBus.bSwapped  = false;
    PortMgr.CFInserted = true;
    PortMgr.SDInserted = true;
    PortMgr.pendingIRQ2 = false;
    PortMgr.SDChipSelect = false;
    PortMgr.PowerConnected = false;

    // make sure SPI1 fifos are empty
    rxHead = rxTail = txHead = txTail = 0;
    txFifoEmpty = true;
    rxFifoEmpty = true;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::~EmRegsVZHandEra330
// ---------------------------------------------------------------------------

EmRegsVZHandEra330::~EmRegsVZHandEra330 (void)
{
	delete fSPISlaveADC;
}

void EmRegsVZHandEra330::Initialize(void)
{
    EmRegsVZ::Initialize();

    SD.Initialize();
}

void EmRegsVZHandEra330::Dispose (void)
{
    EmRegsVZ::Dispose();

    SD.Dispose();
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetLCDScreenOn
// ---------------------------------------------------------------------------

Bool EmRegsVZHandEra330::GetLCDScreenOn (void)
{
	// TRG LCD on is determined by LCD contrast on in the CPLD
	return PortMgr.LCDOn;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetLCDBacklightOn
// ---------------------------------------------------------------------------

Bool EmRegsVZHandEra330::GetLCDBacklightOn (void)
{
	// TRG CPLD controls the backlight
	return PortMgr.BacklightOn;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetLineDriverState
// ---------------------------------------------------------------------------
// Return whether or not the line drivers for the given object are open or
// closed.

Bool EmRegsVZHandEra330::GetLineDriverState (EmUARTDeviceType type)
{
	if (type == kUARTSerial)
	   return (READ_REGISTER (portBData) & PortB_RS232_ON) != 0;

	if (type == kUARTIR)
		return PortMgr.IRPortOn;

	return false;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetUARTDevice
// ---------------------------------------------------------------------------
// Return what sort of device is hooked up to the given UART.

EmUARTDeviceType EmRegsVZHandEra330::GetUARTDevice (int /*uartNum*/)
{
	Bool	serEnabled	= this->GetLineDriverState (kUARTSerial);
	Bool	irEnabled	= this->GetLineDriverState (kUARTIR);

	// It's probably an error to have them both enabled at the same
	// time.  !!! TBD: make this an error message.

	EmAssert (!(serEnabled && irEnabled));

	// !!! Which UART are they using?

//	if (uartNum == ???)
	{
		if (serEnabled)
			return kUARTSerial;

		if (irEnabled)
			return kUARTIR;
	}

	return kUARTNone;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetVibrateOn
// ---------------------------------------------------------------------------

Bool EmRegsVZHandEra330::GetVibrateOn (void)
{
	return false;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetInterruptLevel
// ---------------------------------------------------------------------------

int32 EmRegsVZHandEra330::GetInterruptLevel (void)
{
    int32 retval;

    retval = EmRegsVZ::GetInterruptLevel ();

    if (PortMgr.pendingIRQ2 && (retval < 2))
        retval = 2;

    return retval;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetLEDState
// ---------------------------------------------------------------------------

uint16 EmRegsVZHandEra330::GetLEDState (void)
{
	uint16	result		= kLEDOff;
	uint8	portKData	= READ_REGISTER (portKData);

    if ((portKData & PortK_LED_GREEN) == 0)
		result |= kLEDGreen;

	if ((portKData & PortK_LED_RED) == 0)
		result |= kLEDRed;

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetPortD
// ---------------------------------------------------------------------------

uint8 EmRegsVZHandEra330::GetPortD (uint8 result)
{
     return result |
            PortD_DOCK_BTN |
            PortD_CD_IRQ |
            PortD_CF_IRQ |
            PortD_POWER_FAIL;
}

// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetPortInputValue
// ---------------------------------------------------------------------------
// Return the GPIO values for the pins on the port.  These values are used
// if the select pins are high.

uint8 EmRegsVZHandEra330::GetPortInputValue (int port)
{
    uint8	result = EmRegsVZ::GetPortInputValue (port);

    switch (port)
    {
       case 'D' :
          result = GetPortD (result);
          break;
       case 'F' :
          result = PortF;
          break;
       case 'G' :
          result = PortG;
          break;
       case 'J' :
          result = PortJ;
          break;
       case 'K' :
          result = PortK;
          break;
       case 'M' :
          result = PortM;
	      break;

      }

      return result;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetPortInternalValue
// ---------------------------------------------------------------------------
// Return the dedicated values for the pins on the port.  These values are
// used if the select pins are low.

uint8 EmRegsVZHandEra330::GetPortInternalValue (int port)
{
	uint8	result = EmRegsVZ::GetPortInternalValue (port);

	switch (port)
	{
        case 'D' :
            result = GetPortD(result);
            break;
        case 'F' :
            result = PortF;
            break;
        case 'G' :
            result = PortG;
            break;
        case 'J' :
            result = PortJ;
            break;
        case 'K' :
            result = PortK;
            break;
        case 'M' :
            result = PortM;
            break;
        }

	return result;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetKeyInfo
// ---------------------------------------------------------------------------

void EmRegsVZHandEra330::GetKeyInfo (int* numRows, int* numCols,
								uint16* keyMap, Bool* rows)
{
	*numRows = kNumButtonRows;
	*numCols = kNumButtonCols;

	memcpy (keyMap, kButtonMap, sizeof (kButtonMap));

	// Determine what row is being asked for.
	rows[0]	=  PortMgr.Keys.Row[0];
	rows[1]	=  PortMgr.Keys.Row[1]; 
	rows[2]	=  PortMgr.Keys.Row[2];
    rows[3] =  PortMgr.Keys.Row[3];
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetSPISlave
// ---------------------------------------------------------------------------

EmSPISlave* EmRegsVZHandEra330::GetSPISlave (void)
{
	if ((READ_REGISTER (portJData) & PortJ_AD_CS) == 0)
    {
        if (PortMgr.SenseCurrent)
            return fSPISlaveCurrent;
        else
    		return fSPISlaveADC;
	}

	return NULL;
}


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::GetROMSize
// ---------------------------------------------------------------------------

int32 EmRegsVZHandEra330::GetROMSize (void)
{
	return (2 * 1024 * 1024);
}	


// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::ButtonToBits
// ---------------------------------------------------------------------------

uint16 EmRegsVZHandEra330::ButtonToBits (SkinElementType button)
{
	uint16 bitNumber = 0;
    static Bool CF_button_pushed = false;
    static Bool SD_button_pushed = false;
    static Bool power_button_pushed = false;

	switch (button)
	{
		default:
			bitNumber = EmRegsVZ::ButtonToBits (button);
			break;

		// Borrow some skin elements from Symbol for our Thumb wheel
	    // NOTE: Borrowed Palm's contrast for our aux, so above case already handles it.
		case kElement_TriggerLeft :	 
			bitNumber = keyBitThumbUp;          break;
		case kElement_TriggerCenter : 
			bitNumber = keyBitThumbPush;		break;
		case kElement_TriggerRight :	 
			bitNumber = keyBitThumbDown;		break;

        // Borrow some additional skin elements to trigger a CF removal or insert.
        case kElement_DownButtonLeft :
            bitNumber = 0;
            // we get called twice here, don't handle the event on the button release
            if(!CF_button_pushed)
            {
                PortMgr.CFInserted = !PortMgr.CFInserted;

                if (PortMgr.CFInserted)
					EmDlg::DoCommonDialog("The CF card has been installed.", kDlgFlags_OK);
                else
                    EmDlg::DoCommonDialog("The CF card has been removed.", kDlgFlags_OK);

                PortMgr.pendingIRQ2 = true;
                CF_button_pushed = true;
            }
            else
                CF_button_pushed = false;
            break;

        // SD
        case kElement_DownButtonRight :
            bitNumber = 0;
            if (!SD_button_pushed)
            {
                PortMgr.SDInserted = !PortMgr.SDInserted;

                if (PortMgr.SDInserted)
                    EmDlg::DoCommonDialog("The SD card has been installed.\n\nIt will take a second for the OS to mount it.",
                               kDlgFlags_OK);
                else
                    EmDlg::DoCommonDialog("The SD card has been removed.", kDlgFlags_OK);

                PortMgr.pendingIRQ2 = true;
                SD_button_pushed = true;
            }
            else
                SD_button_pushed = false;
            break;

        // power
        case kElement_UpButtonLeft :
            bitNumber = 0;
            if (!power_button_pushed)
            {
                if (!PortMgr.PowerConnected)
                {
                    if (PortG & PortG_LION)
                        PortG &= ~PortG_LION;
                    else
                        PortG |= PortG_LION;
                }
                PortMgr.PowerConnected = !PortMgr.PowerConnected;
                PortMgr.pendingIRQ2 = true;
                ((EmSPISlave330Current *)fSPISlaveCurrent)->SetMode(PortMgr.PowerConnected);

                power_button_pushed = true;
            }
            else
                power_button_pushed = false;
            break;
	}

	return bitNumber;
}


/**********************************************************************************
 * SD support:
 * HandEra 330 SD is attached to the DragonballVZ SPI1 which is otherwise unused.
 **********************************************************************************/
uint32 EmRegsVZHandEra330::spiRxDRead(emuptr /* address */, int /* size */)
{
    uint32 retval;

    // there is an 8 word fifo here, read back the first in.

    if ((rxHead == rxTail) && rxFifoEmpty)
    {
        // invalid read, fifo empty
        return 0;
    }

    retval = rxFifo[rxTail++];
    if (rxTail == 8)
        rxTail = 0;
    if (rxTail == rxHead)
        txFifoEmpty = true;
    return retval;
}

void EmRegsVZHandEra330::spiTxDWrite(emuptr address, int size, uint32 value)
{
	// Do a standard update of the register. (so reading the last value back works)
	EmRegsVZ::StdWrite (address, size, value);

    if (!txFifoEmpty && (txHead == txTail))
    {
        // fifo full, do nothing
        return;
    }

    txFifoEmpty = false;

    // there is an 8 word fifo here.
    txFifo[txHead++] = value;
    if (txHead == 8)
        txHead = 0;
}

void EmRegsVZHandEra330::spiCont1Write(emuptr address, int size, uint32 value)
{
    // if we were not enabled before, flush fifos
    if ((value & hwrVZ328SPIMControlEnable)==0)
    {
        txTail = txHead;
        rxTail = rxHead;
        txFifoEmpty = rxFifoEmpty = true;
    }

	// Do a standard update of the register.
	EmRegsVZ::StdWrite (address, size, value);

	// Get the current value.
	uint16	spiCont1	= READ_REGISTER (spiCont1);

	// Check to see if data exchange and enable are enabled.
	#define BIT_MASK (hwrVZ328SPIMControlExchange | hwrVZ328SPIMControlEnable)
	if ((spiCont1 & BIT_MASK) == BIT_MASK)
	{
        // do the exchange
        if (!txFifoEmpty)
        {
            // is SD chip selected?
            if (PortMgr.SDChipSelect)
            {
                uint16 rxData, txData;
                
                do
                {
                    txData = txFifo[txTail++];
                    if (txTail == 8)
                        txTail = 0;
                    SD.ExchangeBits(txData, &rxData, (spiCont1 & 0x000f)+1);
                    rxFifo[rxHead++] = rxData;
                    if (rxHead == 8)
                        rxHead = 0;
                } while (txTail != txHead);
                txFifoEmpty = true;
                rxFifoEmpty = false;
            }
            else
            {
                // nothing else is connected here, just stuff the rx fifo and flush the tx fifo
                do
                {
                    txTail++;
                    if (txTail == 8)
                        txTail = 0;
                    rxFifo[rxHead++] = 0xff;
                    if (rxHead == 8)
                        rxHead = 0;
                } while (txTail != txHead);
                rxFifoEmpty = false;
                txFifoEmpty = true;
            }
        }

		// Clear the exchange bit.
		spiCont1 &= ~hwrVZ328SPIMControlExchange;
		WRITE_REGISTER (spiCont1, spiCont1);
	}
}

uint32 EmRegsVZHandEra330::spiCont1Read(emuptr /* address */, int /* size */)
{
	return 0;
}

void EmRegsVZHandEra330::spiIntCSWrite(emuptr /* address */, int /* size */, uint32 /* value */)
{
}

uint32 EmRegsVZHandEra330::spiIntCSRead(emuptr /* address */, int /* size */)
{
	return 0;
}

// ---------------------------------------------------------------------------
//		� EmRegsVZHandEra330::SetSubBankHandlers
// ---------------------------------------------------------------------------

void EmRegsVZHandEra330::SetSubBankHandlers(void)
{
	//HwrM68VZ328Type   regs;

	EmRegsVZ::SetSubBankHandlers();

    // SD support
	this->SetHandler((ReadFunction)&EmRegsVZHandEra330::spiRxDRead,
                         (WriteFunction)&EmRegsVZ::StdWrite,
	                 addressof(spiRxD),
	                 sizeof(UInt16));
	this->SetHandler((ReadFunction)&EmRegsVZ::StdRead,
                         (WriteFunction)&EmRegsVZHandEra330::spiTxDWrite,
	                 addressof(spiTxD),
	                 sizeof(UInt16));
	this->SetHandler((ReadFunction)&EmRegsVZ::StdRead,
                         (WriteFunction)&EmRegsVZHandEra330::spiCont1Write,
	                 addressof(spiCont1),
	                 sizeof(UInt16));
/*
	this->SetHandler((ReadFunction)&EmRegsVZHandEra330::spiIntCSRead,
                     (WriteFunction)&EmRegsVZHandEra330::spiIntCSWrite,
	                 addressof(spiIntCS),
	                 sizeof(regs.spiIntCS));
*/
}
