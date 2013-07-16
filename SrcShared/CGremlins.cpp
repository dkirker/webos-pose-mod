/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 1998-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#include "EmCommon.h"

#include <stdio.h>		// needed for sprintf.
#include <stdlib.h>		// needed for rand and srand
#include <string.h>		// needed for strcpy and friends


#ifdef forSimulator

#define PILOT_PRECOMPILED_HEADERS_OFF

// Palm Includes 
#include <BuildDefines.h>
#ifdef HAS_LOCAL_BUILD_DEFAULTS
#include "LocalBuildDefaults.h"
#endif
#include <PalmTypes.h>

#include <Chars.h>
#include <DebugMgr.h>
#include <ErrorBase.h>
#include <FeatureMgr.h>
#include <Field.h>
#include <Form.h>
#include <TextMgr.h>
#include <PalmLocale.h>

#include "CGremlinsStubs.h"
#include "CGremlins.h"
#include "Hardware.h"
#include <EmuStubs.h>

#define	NON_PORTABLE
#include "SystemPrv.h"
#include "DataPrv.h"
#include "SysEvtPrv.h"
#include <SystemPkt.h>

#include "ShellCmd.h"

#else	// !forSimulator

#include "EmBankRegs.h"			// RegsBank
#include "EmEventPlayback.h"	// RecordPenEvent, etc.
#include "EmMemory.h"			// EmMemPut16, EmMemPut32
#include "EmPalmStructs.h"		// EmAliasPenBtnInfoType
#include "EmPatchState.h"		// GetCurrentAppInfo
#include "EmSession.h"			// gSession, ScheduleAutoSaveState
#include "ErrorHandling.h"		// Errors::ThrowIfPalmError
#include "Hordes.h"				// Hordes::IsOn, TurnOn
#include "Logging.h"
#include "PreferenceMgr.h"		// Preference<GremlinInfo>
#include "ROMStubs.h"			// FtrGet, TxtGetNextChar, TxtCharBounds, TxtByteAddr, FrmGetActiveForm...
#include "SessionFile.h"		// SessionFile
#include "Strings.r.h"			// kStr_ values
#include "EmLowMem.h"			// EmLowMem_SetGlobal for setting battery level



///////////////////////////////////////////////////////////////////////////////////
// Private function declarations
// (actually just some operator override declarations we'll need)

static EmStream&	operator >> (EmStream&, DatabaseInfo&);
static EmStream&	operator << (EmStream&, const DatabaseInfo&);

static EmStream&	operator >> (EmStream&, GremlinInfo&);
static EmStream&	operator << (EmStream&, const GremlinInfo&);


///////////////////////////////////////////////////////////////////////////////////
// Private globals

static int	gIntlMgrExists = -1;





///////////////////////////////////////////////////////////////////////////////////
// Private functions

static Bool IntlMgrExists (void)
{
	if (gIntlMgrExists < 0)
	{
		// Note that we need to check by calling the feature manager rather than
		// checking to see if the trap is implemented. sysTrapIntlDispatch is
		// sysTrapPsrInit on 1.0 systems and sysUnused2 on intermediate systems.
		// That means that the trap IS implemented, but just not the one we want.

		UInt32	data;
		Err		err = FtrGet (sysFtrCreator, sysFtrNumIntlMgr, &data);

		gIntlMgrExists = !err && (data & intlMgrExists) != 0;
	}

	return gIntlMgrExists != 0;
}

static UInt16 _TxtGetNextChar (const Char *inText, UInt32 inOffset, WChar *outChar)
{
	if (IntlMgrExists ())
	{
		return TxtGetNextChar (inText, inOffset, outChar);
	}

	if (outChar)
		*outChar = (UInt8) inText[inOffset];

	return sizeof (Char);
}

static WChar _TxtCharBounds (const Char *inText, UInt32 inOffset, UInt32 *outStart, UInt32 *outEnd)
{
	if (IntlMgrExists ())
	{
		return TxtCharBounds (inText, inOffset, outStart, outEnd);
	}

	if (outStart)
		*outStart = inOffset;

	if (outEnd)
		*outEnd = inOffset + 1;

	return inText[inOffset];
}

static UInt8 _TxtByteAttr (UInt8 inByte)
{
	if (IntlMgrExists ())
	{
		return TxtByteAttr (inByte);
	}

	return byteAttrSingle;
}

#define TxtGetNextChar	_TxtGetNextChar
#define TxtCharBounds	_TxtCharBounds
#define TxtByteAttr		_TxtByteAttr

#include "CGremlins.h"
#include "CGremlinsStubs.h"

#define PRINTF	if (!LogGremlins ()) ; else LogAppendMsg


// Use our own versions of rand() and srand() so that we generate the
// same numbers on both platforms.

#undef RAND_MAX
#define RAND_MAX 0x7fff

#define rand	Gremlin_rand
#define srand	Gremlin_srand

unsigned long int gGremlinNext = 1;

static int rand(void)
{
//	gGremlinNext = gGremlinNext * 1103515245 + 12345;	// MSL numbers

	gGremlinNext = gGremlinNext * 214013L + 2531011L;	// VC++ numbers
	PRINTF ("--- gGremlinNext == 0x%08X", (long) gGremlinNext);

	return ((gGremlinNext >> 16) & 0x7FFF);
}

static void srand(unsigned int seed)
{
	gGremlinNext = seed;
}


#endif


//#define randN(N) ((N) ? rand() / (RAND_MAX / (N)) : (0))
#define randN(N) ((int) (((long) rand() * (N)) / ((long) RAND_MAX + 1)))
#define randPercent (randN(100))

#ifndef forSimulator
#undef randN
inline int randN (long N)
{
	int	result = ((int) (((long) rand() * (N)) / ((long) RAND_MAX + 1)));
	PRINTF ("--- randN(%ld) == 0x%08X", N, (long) result);
	return result;
}
#endif

#define PEN_MOVE_CHANCE							50			// 50% move pen else pen up
#define PEN_BIG_MOVE_CHANCE						5			// 5% move pen really far

#define KEY_DOWN_EVENT_WITHOUT_FOCUS_CHANCE		10
#define KEY_DOWN_EVENT_WITH_FOCUS_CHANCE		40
#define PEN_DOWN_EVENT_CHANCE					(70 + KEY_DOWN_EVENT_WITHOUT_FOCUS_CHANCE)
#define MENU_EVENT_CHANCE						(PEN_DOWN_EVENT_CHANCE + 4)
#define FIND_EVENT_CHANCE						(MENU_EVENT_CHANCE + 2)
#define KEYBOARD_EVENT_CHANCE					(FIND_EVENT_CHANCE + 1)
#define LOW_BATTERY_EVENT_CHANCE				(KEYBOARD_EVENT_CHANCE + 2)
#define APP_SWITCH_EVENT_CHANCE					(LOW_BATTERY_EVENT_CHANCE + 4)
// #define POWER_OFF_CHANCE						(APP_SWITCH_EVENT_CHANCE + 1)

#define LAUNCHER_EVENT_CHANCE					0	// percent of APP_SWITCH_EVENT_CHANCE


#define commandKeyMask							0x0008


#define TYPE_QUOTE_CHANCE						10

#define MAX_SEED_VALUE							1000	// Max. # of seed values allowed.
#define INITIAL_SEED							1

#define LETTER_PROB								60

// Chars less often than a letter
#define SYMBOL_PROB								(LETTER_PROB / 10)
#define EXT_LTTR_PROB							(LETTER_PROB / 3)
#define EXTENDED_PROB							(LETTER_PROB / 5)
#define CONTROL_PROB							(LETTER_PROB / 2)
#define MENU_PROB								(LETTER_PROB / 10)
#define KBRD_PROB								1 	// The formula results in 0 
	//												((LETTER_PROB / 30) / 3)	// three chars to activate keyboard
#define NXTFLD_PROB								(LETTER_PROB / 10)
#define SEND_DATA_PROB							(LETTER_PROB / 60)

// Chars more often than a letter
#define SPACE_PROB								(LETTER_PROB * 5)
#define TAB_PROB								(LETTER_PROB * 2)
#define BACKSPACE_PROB							(LETTER_PROB * 3) 
#define RETURN_PROB								((LETTER_PROB * 10) * 1)	// extra exercise


//Global variables
Gremlins*	TheGremlinsP;					// Pointer to the Gremlins class.
long		IdleTimeCheck;					// Tick count for the next idle query

// Array of probabilities of a key being pressed for gremlin mode.
#define NUM_OF_KEYS		0x110
static const int chanceForKey[NUM_OF_KEYS] = {
	0, 0, 0, 0, 0, 0, 0, 0,														// 0x00 - 0x07
	BACKSPACE_PROB, TAB_PROB, RETURN_PROB, CONTROL_PROB, CONTROL_PROB, 0, 0, 0,	// 0x08 - 0x0F
	0, 0, 0, 0, 0, 0, 0, 0,														// 0x10 - 0x17
	0, 0, 0, 0, CONTROL_PROB, CONTROL_PROB, CONTROL_PROB, CONTROL_PROB,			// 0x18 - 0x1F

	// Symbols
	SPACE_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x20 - 0x23
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x24 - 0x27
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x28 - 0x2B
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x2C - 0x2F
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x30 - 0x33
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x34 - 0x37
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x38 - 0x3B
	SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB, SYMBOL_PROB,	// 0x3C - 0x3F

	// Uppercase
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x40 - 0x43
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x44 - 0x47
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x48 - 0x4B
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x4C - 0x4F
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x50 - 0x53
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x54 - 0x57
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x58 - 0x5B
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x5C - 0x5F

	// Lowercase
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x60 - 0x63
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x64 - 0x67
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x68 - 0x6B
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x6C - 0x6F
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x70 - 0x73
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x74 - 0x77
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x78 - 0x8B
	LETTER_PROB, LETTER_PROB, LETTER_PROB, LETTER_PROB,	// 0x7C - 0x7F

	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x80 - 0x83
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x84 - 0x87
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x88 - 0x8B
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x8C - 0x8F
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x90 - 0x93
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x94 - 0x97
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x98 - 0x9B
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0x9C - 0x9F

	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xA0 - 0xA3
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xA4 - 0xA7
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xA8 - 0xAB
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xAC - 0xAF
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xB0 - 0xB3
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xB4 - 0xB7
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xB8 - 0xBB
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xBC - 0xBF

	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xC0 - 0xC3
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xC4 - 0xC7
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xC8 - 0xCB
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xCC - 0xCF
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xD0 - 0xD3
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xD4 - 0xD7
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xD8 - 0xDB
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xDC - 0xDF

	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xE0 - 0xE3
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xE4 - 0xE7
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xE8 - 0xEB
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xEC - 0xEF
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xF0 - 0xF3
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xF4 - 0xF7
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xF8 - 0xFB
	EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB, EXTENDED_PROB,	// 0xFC - 0xFF
	
	// Virtual events
	// DOLATER kwk - Why not generate keyboardAlphaChr (0x110) & keyboardNumericChr (0x111)?
	0, 0, 0, NXTFLD_PROB, 0, MENU_PROB, CONTROL_PROB, 0,				// 0x100 - 0x107
	CONTROL_PROB, KBRD_PROB, CONTROL_PROB, 0, NXTFLD_PROB, 0, 0, 0,		// 0x108 - 0x10f

};

#define NUM_OF_QUOTES	18

// Shakespearean quotes used by Gremlins for English text
static const char * kAsciiQuotes[NUM_OF_QUOTES] = {
	"Out out damn spot!",
	
	"Et tu, Brute?",
	
	"When in disgrace with fortune and mens' eyes "
		"I all alone beweep my outcast state.  "
		"And trouble deaf heaven with my bootless cries and "
		"look upon myself and curse my fate. "
		"Wishing me like to one more rich in hope, "
		"featured like him, like him with friends possest, "
		"desiring this man's art and that man's scope, "
		"with what I most enjoy contented least;"
		"\n"
		"Yet in these thoughts myself almost despising- "
		"haply I think on thee: and then my state, "
		"like to the lark at break of day arising "
		"from sullen earth, sings hymns at Heaven's gate; "
		"for thy sweet love rememb'red such wealth brings "
		"that then I scorn to change my state with kings.",
		
	"I think my wife is honest, and think she is not; "
		"I think that thou art just, and think thou art not.",
		
	"O that this too too sullied flesh would melt, thaw, "
		"and resolve itself into a dew, "
		"or that the Everlasting had not fixed "
		"His canon 'gainst self-slaughter.",
		
	"Come, you spirits that tend on mortal thoughts, unsex me here, "
		"and fill me from the crown to the toe top-full "
		"of direst cruelty.",
		
	"I do not think but Desdemona's honest.",
	
	"That I did love the Moor to live with him",
	
	"What a piece of work is a man",
	
	"Fair is foul, and foul is fair.",
	
	"All hail, Macbeth, that shalt be King hereafter!",
	
	"What's Montague?",
	
	"To a nunnery, go, and quickly too.",
	
	"I'll have some proof.",
	
	"Now are we well resolved, and by God's help and yours, "
		"the noble sinews of our power, France being ours, "
		"we'll bend it to our awe or break it all to pieces.",
		
	"Tennis balls, my liege.",
	
	"De Sin: le col de Nick, le menton de Sin.",
	
	"But swords I smile at, weapons laugh to scorn, "
		"brandished by man that's of a woman born."
	
};

static const char * kShiftJISQuotes[NUM_OF_QUOTES] = {
	"�l�́A���ĐX�̐_���E����",
	
	"���̂̂��P",
	
	"�l�ʂƏb�̐g�́A���؂̊p�����X�̐_�E�V�V�_��"
		"�l�͉��̎E���˂΂Ȃ�Ȃ������̂��|"
		"���̎���A�l�Ԃ��ӂ��A�����̌����т��񂩂ꂽ�Ƃ͂����A"
		"�܂��l���񂹂��ʑ��Â̐X�����������Ɏc���Ă���"
		"\n"
		"���ꂼ��̐X�́A����R���Ȃ�"
		"����Ō��������b�������K���ɂȂ��Ď���Ă���"
		"�����āA�����N���l�ԒB���P��"
		"�r�Ԃ�_�X�Ƌ�����Ă���"
		"���̏b�B���]���Ă����̂��A�V�V�_�ł���"
		"�r�Ԃ�_�X���ł�����������Ă����̂�"
		"�^�^���҂ƌĂ΂�鐻�S�W�c������",
		
	"���̐g�Ń^�^���W�c�𗦂���G�{�V��O"
		"�ޏ��͌Ȃ��M�O�ŁA�X��؂�񂢂Ă���",
		
	"���̔z���ŁA��O���h���炤�A�S���U�ɂ��g�L�ƍb�Z"
		"�V�V�_���˂炤���̕s���̖V��E�W�R�V"
		"�k�̒n�̉ʂẲB�ꗢ�ɏZ�ޘV�ޏ��E�q�C����"
		"������A�i�S�̐_�A�����ȂǐX�����_�b����",
		
	"����ɐX�̐���E�R�_�}�����c�����T���͐l�Ԃ�"
		"�q�ł���Ȃ���R�������Ɉ�Ă�ꂽ�u���̂̂��P�v",
		
	"�������T���́A�X��N���l�Ԃ���������",
	
	"��ł��������āA�l�Ԃƍr�Ԃ�_�X�̍Ō�",
	
	"�̑匈��Ɋ������܂�鏭�N",
	
	"�A�V�^�J�ނ́A���̎􂢂�",
	
	"������ꂽ���䂦���q�����߂���@��T���ɁA",
	
	"���ɏo�����N������",
	
	"���N�Ə����͎S���̒��ŏo��A",
	
	"����ɐS��ʂ킹",
	
	"�Ă䂭�ӂ��肪�����ƎE�C�̉ʂĂɌ���������"
		"��]�Ƃ͉��������̂����N�Ə����̈�"
		"�������ɃV�V�_���߂���l�ԂƏb����",
		
	"�̐킢���c����",
	
	"�g�p����̈�取�������A�W�J����Ă����c",
	
	"����E�r�{�@�@�F�{��@�x���@��@�@�@�@�F����ꎈ�Y�E���c�@�L���쑍�w���@�@"
		"�F���ԍN���v���f���[�T�[�F��ؕq�v"
};

static const char * kBig5Quotes[NUM_OF_QUOTES] = {
	"���}���Ĥ@�^�]�C�@",
	
	"�̦۶��G�]��",
	
	"��L�@�f�ڤۤ���A�G�N�u�����h�A�ӭ�\"�q�F\""
	"�����A�����@�Ѥ]�C�G��\"�¤h��\"�����C"
	"���Ѥ��ҰO��Ʀ�H�H�ۤS���G�����иL�L�A�@�ƵL���A",
		
	"�����η��Ҧ����k�l�A�@�@�ӦҸ��h�A"
	"ı����ѡA�ҥX��ڤ��W�C��ڰ��Ž�ܡA"
	"�ۤ��Y���ȳ��v�H��\\�h���l�A���S�L�q���j�L�i�p��"
	"��]�I���A�h�۱��N�w���ҿ�Ѯ����w�A�A���K��"
	"�ɡAܮ����Τ���A�I���S�Ш|�����A�t�v "
	"\n"
	"�ͳW�ͤ��w�A�H�ܤ���@�޵L���A�b�ͼ�ˤ��o�A�s "
	"�z�@���A�H�i�ѤU�H�G�ڤ��o�T���K�A�M�ӻդ�"
	"���۾������H�A�U���i�]�ڤ����v�A���@�v�u "
	"�A�@�֨Ϩ�{���]�C�����餧�T�ܽ����A��_÷�ɡA�� "
	"��i���S�A���h�x��A�祼�����ڤ����h�����̡C���� "
	"���ǡA�U���L��A�S�󧫥ΰ��y�����A�źt�X�@�q�G��"
	"�ӡA��i�ϻӻլL�ǡA�ƥi���@���ءA�}�H�T�e�A���� "
	"�y�G�H�G��\"��B��\"�����C���^���Z��\"��\"��\"��\"",
		
	"���r�A�O�����\\�̲��ءA��O���ѥ߷N�����C�C��ݩx�G�A"
	"�D���ѱq��ӨӡH���_�ڥ������ "
	"��A�ӫ��h�`������C�ݦb�U�N���Ӿ��`��"
	"��Ӥk�E��ҥ۸ɤѤ��ɡA��j��s",
		
	"�L�]�V�m�����g�Q�G�V�A��g�G�Q�|�V�x�ۤT�U���d���ʹs�@���C�E��"
	"��u�ΤF�T�U���d���ʶ��A�u���ѤF�@�����ΡA�K"
	"��b���s�C�G�p�U�C",
		
	"�֪����ۦ۸g�Ҥ���A�F�ʤw�q�A�]��_��",
	
	"�ѱo�ɤѡA�W�ۤv�L�������J��A�E�۫�ۼ�",
	
	"�A��]�d���F�\\�C�@��A�����",
	
	"�����ڡA�X���@���@�D�����ӨӡA",
	
	"�ͱo���椣�Z�A�ׯ��~���A���������Ӧܮp�U�A���_��",
	
	"�䰪�ֽͧסC���O",
	
	"���Ƕ��s�������P�Ȥۤ��ơA��K����",
	
	"���Ф��a�شI�Q�C����",
	
	"ť�F�A��ı���ʤZ�ߡA�]�Q�n��H���h�ɤ@�ɳo�a�شI�Q�A��"
	"�۫�����A���o�w�A�K�f�R�H���A�V�����D���D�G�j�v�A"
	"�̤l�����A���ਣ§�F�C�A�D�G��ͨ��H�@���aģ�c�ءA",
		
	"�ߤ��}���C�̤l��������",
	
	"�A�ʫo�y�q�A�p���G�v�P�ιD��A�w�D�Z�~�A��",
	
	"���ɤ��٥@�����A�Q���٤H���w�C�p�X�o�@�I�O�ߡA"
	"��a�̤l�o�J���СA�b���I�Q�����A�ŬX�m��"
};

static const char * kGB2312Quotes[NUM_OF_QUOTES] = {
	"�˿����һ��Ҳ����",
	
	"�����ƣ�����",
	
	"����һ���λ�֮�󣬹ʽ�������ȥ�����衰ͨ�顱"
	"֮˵��׫��һ��Ҳ����Ի����ʿ�������ơ�"
	"���������Ǻ��º��ˣ������ƣ���糾µµ��һ���޳ɣ�",

	"�����������֮Ů�ӣ�һһϸ����ȥ��"
	"������ֹ��ʶ���Գ�����֮�ϡ�����������ü��"
	"�ϲ�����ȹ���գ�ʵ�������࣬��������֮���޿����֮"
	"��Ҳ�����ˣ����������������������£�������֮"
	"ʱ�������з�֮�գ������ֽ���֮������ʦ"
	"\n"
	"�ѹ�̸֮�£���������һ���޳ɣ������ʵ�֮���"
	"��һ�����Ը������ˣ���֮��̲��⣬Ȼ�����"
	"�����������ˣ��򲻿�����֮��Ф���Ի�����"
	"��һ��ʹ������Ҳ�������֮é����뻣�������������"
	"��Ϧ��¶������ͥ������δ�з���֮�󻳱�ī�ߡ�����"
	"δѧ���±����ģ��ֺη��ü�����ԣ����ݳ�һ�ι���"
	"�������ʹ����Ѵ�����������֮Ŀ�����˳��ƣ�����"
	"�˺�����Ի������塱���ơ��˻��з��á��Ρ��á��á�",
		
	"���֣�������������Ŀ�����Ǵ������Ȿּ����λ���٣���"
	"������Ӻζ�����˵����������"
	"�ƣ�ϸ��������Ȥζ�������½�������ע��"
	"ԭ��Ů�����ʯ����֮ʱ���ڴ��ɽ",
		
	"�޻������ɸ߾�ʮ���ɣ�������ʮ������ʯ������ǧ�����һ�顣活�"
	"��ֻ����������ǧ��ٿ飬ֻ����ʣ��һ��δ�ã���"
	"���ڴ�ɽ�๡���¡�",
		
	"˭֪��ʯ�Ծ���֮��������ͨ�������ʯ",
	
	"��ò��죬���Լ��޲Ĳ�����ѡ������Թ��̾",
	
	"����ҹ���Ų�����һ�գ������",
	
	"��֮�ʣ����һɮһ��ԶԶ������",
	
	"���ùǸ񲻷����������죬˵˵ЦЦ�������£�����ʯ",
	
	"�߸�̸���ۡ�����",
	
	"˵Щ��ɽ����������֮�£����˵��",
	
	"�쳾���ٻ����󡣴�ʯ",
	
	"���ˣ������򶯷��ģ�Ҳ��Ҫ���˼�ȥ��һ�����ٻ����󣬵�"
	"�Ժ޴ִ��������ѣ���������ԣ�����ɮ��˵������ʦ��"
	"���Ӵ�����ܼ����ˡ����Ŷ�λ̸����������ҫ������",

	"����Ľ֮����������ִ�",
	
	"����ȴ��ͨ��������ʦ���ε��壬���Ƿ�Ʒ����",
	
	"�в������֮�ģ��������֮�¡����ɷ�һ����ģ�"
	"Я�����ӵ���쳾�����Ǹ����У���������"
};

typedef struct
{
	UInt16 charEncoding;
	const char** strings;
} QuotesInfoType;

static const QuotesInfoType kQuotesInfo[] =
{
	{ charEncodingPalmSJIS, kShiftJISQuotes },
	
	// All of the possible Traditional Chinese encodings.
	{ charEncodingBig5, kBig5Quotes },
	{ charEncodingBig5_HKSCS, kBig5Quotes },
	{ charEncodingBig5Plus, kBig5Quotes },
	{ charEncodingPalmBig5, kBig5Quotes },
	
	// All of the possible Simplified Chinese encodings.
	{ charEncodingGB2312, kGB2312Quotes },
	{ charEncodingGBK, kGB2312Quotes },
	{ charEncodingPalmGB, kGB2312Quotes }
};

/***********************************************************************
 *
 * FUNCTION:	GetFocusObject
 *
 * DESCRIPTION: Return whether the current form has the focus.
 *
 * CALLED BY:  here
 *
 * PARAMETERS:	none
 *
 * RETURNED:	TRUE if the form has a focus set and FALSE if not
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			roger	8/25/95	Initial Revision
 *			roger	11/27/95	Ignored not editable fields.
 *
 ***********************************************************************/
static FieldPtr GetFocusObject()
{
	FormPtr frm;
	UInt16 focusObj;
	FieldPtr textFieldP;


	// Pick a point within one of the current form's objects
	frm = FrmGetActiveForm ();

	// The active window will not be the active form
	// if a popup list of a menu is displayed.
	if ((! frm) || (FrmGetWindowHandle (frm) != WinGetActiveWindow ()) ||
		((focusObj = FrmGetFocus(frm)) == noFocus))
	{
		if (!frm)
			PRINTF ("--- GetFocusObject == NULL (FrmGetActiveForm () == NULL)");
		else if (FrmGetWindowHandle (frm) != WinGetActiveWindow ())
			PRINTF ("--- GetFocusObject == NULL (FrmGetWindowHandle () != WinGetActiveWindow ())");
		else
			PRINTF ("--- GetFocusObject == NULL (FrmGetFocus () == noFocus)");

		return NULL;
	}

	// Get the field.  If it's a table get it's field.
	if (FrmGetObjectType(frm, focusObj) == frmTableObj)
	{
		textFieldP = TblGetCurrentField((TablePtr) FrmGetObjectPtr(frm, focusObj));
		if (textFieldP == NULL)
		{
			PRINTF ("--- GetFocusObject == NULL (TblGetCurrentField () == NULL)");
			return NULL;
		}
	}
	else
	{
		textFieldP = (FieldPtr) FrmGetObjectPtr(frm, focusObj);

		if (textFieldP == NULL)
		{
			PRINTF ("--- GetFocusObject == NULL (FrmGetObjectPtr () == NULL)");
		}
	}

	return textFieldP;
}


/***********************************************************************
 *
 * FUNCTION:	IsFocus
 *
 * DESCRIPTION: Return whether the current form has the focus.
 *
 * CALLED BY:  EmGremlins.cp
 *
 * PARAMETERS:	none
 *
 * RETURNED:	TRUE if the form has a focus set and FALSE if not
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			roger	8/25/95	Initial Revision
 *			roger	11/27/95	Ignored not editable fields, broke out GetFocusObject
 *
 ***********************************************************************/
static int IsFocus()
{
	FieldPtr textFieldP;
	FieldAttrType attr;



	textFieldP = GetFocusObject();
	if (textFieldP == NULL)
	{
		PRINTF ("--- IsFocus == false (textFieldP == NULL)");
		return false;
	}

	// Now make sure that the field is editable.
	FldGetAttributes(textFieldP, &attr);
	if (!attr.editable)
	{
		PRINTF ("--- IsFocus == false (!attr.editable 0x%04X)", (uint32) *(uint16*) &attr);
		return false;
	}

	PRINTF ("--- IsFocus == true");
	return true;
}


/***********************************************************************
 *
 * FUNCTION:	SpaceLeftInFocus
 *
 * DESCRIPTION: Return the number of characters which can be added to 
 *	the object with the focus.
 *
 * CALLED BY:  EmGremlins.cp
 *
 * PARAMETERS:	none
 *
 * RETURNED:	The number of characters which can be added to 
 *	the object with the focus.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			roger	11/27/95	Initial Revision
 *
 ***********************************************************************/
static int SpaceLeftInFocus()
{
	FieldPtr textFieldP;
	FieldAttrType attr;



	textFieldP = GetFocusObject();
	if (textFieldP == NULL)
		return 0;

	// Now make sure that the field is editable.
	FldGetAttributes(textFieldP, &attr);
	if (!attr.editable)
		return 0;



	return FldGetMaxChars(textFieldP) - FldGetTextLength(textFieldP);
}


/***********************************************************************
 *
 * FUNCTION:	FakeLocalMovement
 *
 * DESCRIPTION: Generate a random point within the vicinity of the last
 *						point.
 *
 * CALLED BY:  EmGremlins.cp
 *
 * PARAMETERS:	currentX -	the new x-coordinate of a pen movement.
 *					currentY -	the new y-coordinate of a pen movement.
 * 				lastX -		the last x-coordinate of a pen movement.
 *					lastY -		the last y-coordinate of a pen movement.					
 *
 * RETURNED:	Nothing.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/15/95	Initial Revision
 *
 ***********************************************************************/
static void FakeLocalMovement(Int16* currentX, Int16* currentY, Int16 lastX, Int16 lastY)
{
	Int16 winWidth, winHeight;

	*currentX = lastX + (randN(FntLineHeight() * 2) - FntLineHeight());
	*currentY = lastY + (randN(FntLineHeight() * 2) - FntLineHeight());	// FntLineHeight

	// Note: This code was incorrectly using Hwr Display constants to determine screen size.
	//			The approved of method is to use the size of the current window, which may also be
	//			the screen, however, this may not be correct for what gremilns needs to do.
	//			Something needs to be done for now just to get it to work. BRM 6/30/99
	WinGetDisplayExtent(&winWidth, &winHeight);
	
	// Clip to screen bounds
	//
	// KAAR: In original Gremlins, the point was pinned to [-1...winWidth/Height].
	// That doesn't seem right, especially since -1 is used as a pen up indicator.
	// So now I clip to [0...winWidth/Height).

	if (*currentX < 0) *currentX = 0;
	if (*currentX >= winWidth) 
		*currentX = winWidth - 1;

	if (*currentY < 0) *currentY = 0;
	if (*currentY >= winHeight) 
		*currentY = winHeight = 1;
}


/***********************************************************************
 *
 * FUNCTION:	RandomScreenXY
 *
 * DESCRIPTION: Generate a random point.
 *
 * CALLED BY:  EmGremlins.cp
 *
 * PARAMETERS:	x -	the x-coordinate of a pen movement.
 *					y -	the y-coordinate of a pen movement.
 *
 * RETURNED:	Nothing.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/15/95	Initial Revision
 *
 ***********************************************************************/
static void RandomScreenXY(Int16* x, Int16* y)
{
#ifdef __DEBUGGER_APPLICATION__

	// Since the WinGetDisplayExtent() trap doesn't exist in all versions
	// of the Palm OS, the debugger can't rely on it being around.  So,
	// for the debugger version of this build, we explicitely set the
	// old screen width.
	//
	// DOLATER:  Figure out a way to determine if the WinGetDisplayExtent()
	// is around.  If it is, then call it.  Otherwise, revert to the
	// old constants.
	//
	#define hwrDisplayWidth 	160
	#define hwrDisplayHeight	160

	*x = randN(hwrDisplayWidth);
	*y = randN(hwrDisplayHeight);
	
#else

	Int16 winWidth, winHeight;

	WinGetDisplayExtent(&winWidth, &winHeight);

	*x = randN(winWidth);
	*y = randN(winHeight);

#endif
}


/***********************************************************************
 *
 * FUNCTION:	RandomWindowXY
 *
 * DESCRIPTION: Generate a random point.
 *
 * CALLED BY:  EmGremlins.cp
 *
 * PARAMETERS:	x -	the x-coordinate of a pen movement.
 *					y -	the y-coordinate of a pen movement.
 *
 * RETURNED:	Nothing.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			Keith	11/11/99	Initial Revision
 *
 ***********************************************************************/
static void RandomWindowXY(Int16* x, Int16* y)
{
	// Every so often tap anywhere on the screen (10%)
	if ((randN(10) == 1) || (WinGetActiveWindow () == NULL))
	{
		RandomScreenXY(x, y);
	}
	else
	{
		// We want to tap in the active window.  However, WinGetWindowBounds
		// works against the draw window, which is not necessarily the active
		// window.  Make it so.

		WinHandle	oldDraw = WinSetDrawWindow (WinGetActiveWindow());

		RectangleType	bounds;
		WinGetWindowBounds (&bounds);

		*x = bounds.topLeft.x + randN(bounds.extent.x);
		*y = bounds.topLeft.y + randN(bounds.extent.y);

		WinSetDrawWindow (oldDraw);
	}
}


/***********************************************************************
 *
 * FUNCTION:	FakeEventXY
 *
 * DESCRIPTION: Generate random (x,y) coordindates to produce an event.
 *
 * CALLED BY:  EmGremlins.cp
 *
 * PARAMETERS:	x -	x-coordinate of a pen down.
 *					y -	y-coordinate of a pen down.
 *
 * RETURNED:	
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	08/15/95	Initial Revision
 *			kwk	07/17/98	10% of the time, generate tap in silkscreen btn.
 *			kwk	08/04/99	Cranked percentage down to 5%, since otherwise
 *								we're always just bringing up the keyboard or
 *								the Find form.
 *
 ***********************************************************************/
static void FakeEventXY(Int16* x, Int16* y)
{
	FormPtr frm;
	Int16 objIndex;
	RectangleType bounds;

#ifndef forSimulator
	// Every so often tap anywhere on the screen (2%)
	if (randN(100) < 2)
		{
		RandomScreenXY(x, y);
		return;
		}
#endif

	// Pick a point within one of the current form's objects
	frm = FrmGetActiveForm ();

	// First see if we want to generate a tap in a silkscreen button. If not, then
	// generate a point in the draw window if there no active form, or the active form 
	// is not the the active window.. The active window will not be the active form
	// if a popup list of a menu is displayed.
	//
	// Also do this if there aren't any objects in the form.
	
	if (randN(20) == 1) {
		UInt16 numButtons;
		const PenBtnInfoType* buttonListP = EvtGetPenBtnList(&numButtons);

		const size_t	size = EmAliasPenBtnInfoType<PAS>::GetSize ();
		emuptr			addr = ((emuptr) buttonListP) + randN(numButtons) * size;

		EmAliasPenBtnInfoType<PAS>	button (addr);
		RectangleType randButtonRect;
		randButtonRect.topLeft.x = button.boundsR.topLeft.x;
		randButtonRect.topLeft.y = button.boundsR.topLeft.y;
		randButtonRect.extent.x = button.boundsR.extent.x;
		randButtonRect.extent.y = button.boundsR.extent.y;
		
		*x = randButtonRect.topLeft.x + (randButtonRect.extent.x / 2);
		*y = randButtonRect.topLeft.y + (randButtonRect.extent.y / 2);
	} else if ((frm == NULL) || 
		(FrmGetWindowHandle (frm) != WinGetActiveWindow ()))
	{
		RandomWindowXY (x, y);
	}
	else
	{
		// Generate a point in an one of the form's objects that we expect
		// can do something with the point (i.e. labels are ignored).

#ifdef forSimulator
		do 
		{
			objIndex = randN(numObjects);
			switch (FrmGetObjectType (frm, objIndex))
			{
				case frmBitmapObj:
				case frmLineObj:
				case frmFrameObj:
				case frmRectangleObj:
				case frmLabelObj:
				case frmTitleObj:
				case frmPopupObj:
					// do nothing for these
					objIndex = -1;
					break;
				
				default:
					FrmGetObjectBounds (frm, objIndex, &bounds);
					*x = bounds.topLeft.x + randN(bounds.extent.x);
					*y = bounds.topLeft.y + randN(bounds.extent.y);
					WinWindowToDisplayPt(x, y);

					if (	*x < -1 || *x > 1000 || 
							*y < -1 || *y > 1000)
						ErrDisplay("Invalid point made");

					break;
			}	// end switch
		} while (objIndex == -1);	// don't leave until we found a useful object

#else
		// Get the list of objects we can click on.

		vector<UInt16>	okObjects;
		::CollectOKObjects (frm, okObjects);

		// If there are no such objects, just generate a random point.

		if (okObjects.size () == 0)
		{
			RandomWindowXY (x, y);
		}

		// If there are such objects, pick one and click on it.

		else
		{
			objIndex = okObjects[randN(okObjects.size ())];

			FrmGetObjectBounds (frm, objIndex, &bounds);

			Int16 winWidth, winHeight;
			::WinGetDisplayExtent(&winWidth, &winHeight);

			if (bounds.topLeft.x < 0)
				bounds.topLeft.x = 0;

			if (bounds.topLeft.y < 0)
				bounds.topLeft.y = 0;

			if (bounds.extent.x > winWidth - bounds.topLeft.x - 1)
				bounds.extent.x = winWidth - bounds.topLeft.x - 1;

			if (bounds.extent.y > winHeight - bounds.topLeft.y - 1)
				bounds.extent.y = winHeight - bounds.topLeft.y - 1;

			*x = bounds.topLeft.x + randN(bounds.extent.x);
			*y = bounds.topLeft.y + randN(bounds.extent.y);

			WinWindowToDisplayPt(x, y);
		}
#endif
	}	// end else
}


/************************************************************
 *
 * FUNCTION: 	 GremlinsSendEvent
 *
 * DESCRIPTION: Send a synthesized event to the device if it's 
 *              idle.
 *
 * PARAMETERS:  nothing
 *
 * RETURNS:     nothing
 * 
 * CALLED BY:   the debugger's console object
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			art	11/2/95	Created.
 *			dia	8/26/98	Added try/catch block.
 *
 *************************************************************/
#ifdef forSimulator

void GremlinsSendEvent (void)
{
//	long					tick;
//	Boolean					idle;
//	LowMemType*				lowMemP = (LowMemType*)PilotGlobalsP;
//	SysEvtMgrGlobalsPtr		sysEvtMgrGlobalsP;
	
	if (!TheGremlinsP->IsInitialized() || !StubGremlinsIsOn())
		return;

	ErrTry
		{
#if EMULATION_LEVEL == EMULATION_WINDOWS
		TheGremlinsP->GetFakeEvent();
#else
//		This makes it go faster, but it is much less careful (not as reproducable).
//		The code was left here for future reference / fixing.

//		// If accessing remote device, low memory is at 0...
//		#if MEMORY_TYPE == MEMORY_REMOTE 
//		lowMemP = (LowMemType*)0;
//		#endif
//		
//		// Find out if the device is idle.
//		tick = StubTimGetTicks();
//		if ((tick - IdleTimeCheck) >= 0)
//			{
//			sysEvtMgrGlobalsP = (SysEvtMgrGlobalsPtr)ShlDWord(&lowMemP->fixed.globals.sysEvtMgrGlobalsP);
//			idle = ShlByte(&sysEvtMgrGlobalsP->idle);
//			if (!idle) 
//				{
//				IdleTimeCheck = tick + 12;  // 10 times a second
//				return;
//				}
//			else 
//				// Clear the idle bit so the the device will not send us another idle packet.
//				// Send an event
//				IdleTimeCheck = 0;
//
			TheGremlinsP->GetFakeEvent();
//			}
#endif
		}
	ErrCatch (inErr)
		{
		if (inErr != -1)
			{
			char text[256];
			UInt32 step;

			// Print error & stop...
			TheGremlinsP->SStatus (NULL, &step, NULL);
			sprintf(text, "Error #%lx occurred while sending.  Gremlins at %ld.  Stopping.\n", inErr, step);
			DbgMessage(text);
			StubAppGremlinsOff();
			}
		}
	ErrEndCatch
}

#endif


/************************************************************
 *
 * FUNCTION: 	 GremlinsProcessPacket
 *
 * DESCRIPTION: Send a synthesized event to the device if it's 
 *              idle.
 *
 * PARAMETERS:  bodyP - pointer to Gremlins packet from device.
 *
 * RETURNS:     nothing
 * 
 * CALLED BY:   the debugger's console object
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			art	11/2/95	Created.
 *			dia	8/26/98	Added try/catch block.
 *
 *************************************************************/
#ifdef forSimulator

void GremlinsProcessPacket (void* bodyParamP)
{
	UInt8					flags;
	SysPktGremlinsCmdType*	bodyP = (SysPktGremlinsCmdType*)bodyParamP;
	LowMemType*				lowMemP = (LowMemType*)PilotGlobalsP;
	SysEvtMgrGlobalsPtr		sysEvtMgrGlobalsP;

	if (!TheGremlinsP->IsInitialized())
		return;
		
	ErrTry
		{
		// See which action code got sent us
		if (bodyP->action == sysPktGremlinsIdle) {

			// If accessing remote device, low memory is at 0...
			#if MEMORY_TYPE == MEMORY_REMOTE 
			lowMemP = (LowMemType*)0;
			#endif

			// Clear the idle bit so the the device will not send us another idle packet.
			// Send an event
			TheGremlinsP->GetFakeEvent();
			
			// Turn the idle bit back on.
			sysEvtMgrGlobalsP = (SysEvtMgrGlobalsPtr)ShlDWord((void *)&lowMemP->fixed.globals.sysEvtMgrGlobalsP);
			flags = ShlByte((void *)&sysEvtMgrGlobalsP->gremlinsFlags);
			flags |= grmGremlinsIdle;
			ShlWriteMem (&flags, (UInt32)&sysEvtMgrGlobalsP->gremlinsFlags, sizeof(UInt8));

//			flags = ShlByte(&sysEvtMgrGlobalsP->gremlinsFlags);
//			ErrFatalDisplayIf (!(flags & grmGremlinsIdle), "Invalid flags");
			}
			
		else
			ErrDisplay("Invalid action code");
		}
	ErrCatch (inErr)
		{
		if (inErr != -1)
			{
			char text[256];
			UInt32 step;
			
			// Print error & stop...
			TheGremlinsP->SStatus (NULL, &step, NULL);
			sprintf(text, "Error #%lx occurred while processing.  Gremlins at %ld.  Stopping.\n", inErr, step);
			DbgMessage(text);
			StubAppGremlinsOff();
			}
		}
	ErrEndCatch
}
#endif


// ---------------------------------------------------------------------------
//		� operator >> (EmStream&, DatabaseInfo&)
// ---------------------------------------------------------------------------

EmStream& operator >> (EmStream& inStream, DatabaseInfo& outInfo)
{
	inStream >> outInfo.creator;
	inStream >> outInfo.type;
	inStream >> outInfo.version;

	inStream >> outInfo.dbID;
	inStream >> outInfo.cardNo;
	inStream >> outInfo.modDate;
	inStream >> outInfo.dbAttrs;
	inStream >> outInfo.name;

	outInfo.dbName[0] = 0;

	return inStream;
}


// ---------------------------------------------------------------------------
//		� operator << (EmStream&, const DatabaseInfo&)
// ---------------------------------------------------------------------------

EmStream& operator << (EmStream& inStream, const DatabaseInfo& inInfo)
{
	LocalID		dbID = 0;
	UInt16 		cardNo = 0;
	UInt32		modDate = 0;
	UInt16		dbAttrs = 0;
	char		name[dmDBNameLength] = {0};

	inStream << inInfo.creator;
	inStream << inInfo.type;
	inStream << inInfo.version;

	// I have no idea why dummy values are written out for these fields.
	// But it sure causes us problems later when we need to access them!
	// See the code in Hordes::GetAppList that needs to patch up the missing
	// information.

	inStream << dbID;
	inStream << cardNo;
	inStream << modDate;
	inStream << dbAttrs;
	inStream << name;

	return inStream;
}


// ---------------------------------------------------------------------------
//		� operator >> (EmStream&, AppPreferences::GremlinInfo&)
// ---------------------------------------------------------------------------

EmStream& operator >> (EmStream& inStream, GremlinInfo& outInfo)
{
	bool	dummy;

	inStream >> outInfo.fNumber;
	inStream >> outInfo.fSteps;
	inStream >> outInfo.fAppList;

	inStream >> dummy;				// forward compatibility: this field was
									// fContinuePastWarnings

	inStream >> dummy;
	inStream >> dummy;

	return inStream;
}


// ---------------------------------------------------------------------------
//		� operator << (EmStream&, const AppPreferences::GremlinInfo&)
// ---------------------------------------------------------------------------

EmStream& operator << (EmStream& inStream, const GremlinInfo& inInfo)
{
	bool	dummy = false;

	inStream << inInfo.fNumber;
	inStream << inInfo.fSteps;
	inStream << inInfo.fAppList;

	inStream << dummy;				// backward compatibility: this field was
									// fContinuePastWarnings

	inStream << dummy;
	inStream << dummy;

	return inStream;
}


/************************************************************
 *
 * FUNCTION: Default Constructor
 *
 * DESCRIPTION: Finds the key probablilities sum.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: main() of EmEmulatorApp.cp
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	08/01/95	Created.
 *			kwk	07/17/98	Moved key probability init into run-time
 *								section.
 *
 *************************************************************/
Gremlins::Gremlins()
{
	keyProbabilitiesSum = 0;
	inited = false;
#ifdef forSimulator
	number = -1;
#else
	number = ~0;
#endif
}

/************************************************************
 *
 * FUNCTION: Destructor
 *
 * DESCRIPTION: Any necessary deallocation or cleanup.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: main() of EmEmulatorApp.cp
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/1/95	Created.
 *
 *************************************************************/
Gremlins::~Gremlins()
{
}

/************************************************************
 *
 * FUNCTION: IsInitialized
 *
 * DESCRIPTION: Returns whether or not Gremlins has be initialized.
 *
 * PARAMETERS: None.
 *
 * RETURNS: TRUE - has been initialized, FALSE - has not been initialized.
 * 
 * CALLED BY: FindCommandStatus() in EmEmulatorApp.cp.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/1/95	Created.
 *
 *************************************************************/
Boolean Gremlins::IsInitialized() const
{
	return inited;
}


/************************************************************
 *
 * FUNCTION: Initialize
 *
 * DESCRIPTION: Initialize the gremlins class.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: ObeyCommand() in EmEmulatorApp.cp.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/1/95	Created.
 *
 *************************************************************/
void Gremlins::Initialize(UInt16 newNumber, UInt32 untilStep, UInt32 finalVal)
{
#ifndef forSimulator
	gIntlMgrExists = -1;
	::ResetCalibrationInfo();
	::ResetClocks ();
	EmLowMem_SetGlobal (hwrBatteryLevel, 255);
	EmLowMem_SetGlobal (hwrBatteryPercent, 100);
#endif

	counter = 0;
	until = untilStep;
	finalUntil = finalVal;
#ifndef forSimulator
	saveUntil = until;
#endif
	catchUp = false;
	needPenUp = false;
	charsToType[0] = '\0';
	inited = true;
#ifdef forSimulator
	// removed...test will always fail because newNumber is unsigned...
//	if (newNumber == -1)
//		newNumber = INITIAL_SEED;
		//newNumber = clock() % MAX_SEED_VALUE + 1;	
#endif
	number = newNumber;
	srand(number);

	IdleTimeCheck = 0;

	// Update menus (needed when init. called from console)
	StubAppGremlinsOn ();
}



/************************************************************
 *
 * FUNCTION: Reset
 *
 * DESCRIPTION: Un-initialize the gremlins class.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 *************************************************************/
void Gremlins::Reset(void)
{
	inited = false;
}


/************************************************************
 *
 * FUNCTION: New
 *
 * DESCRIPTION: Start new Gremlins
 *
 * PARAMETERS: GremlinInfo info
 *
 * RETURNS: Nothing.
 * 
 *************************************************************/
void
Gremlins::New (const GremlinInfo& info)
{
	if (LogGremlins ())
	{
		string	templ = Platform::GetString (kStr_GremlinStarted);
		LogAppendMsg (templ.c_str (), (int) info.fNumber, info.fSteps);
	}

	// If needed, switch to an "approved" application.
	// This code roughly follows that in AppsViewSwitchApp in Launcher.

	if (info.fAppList.size () > 0)
	{
		// Switch to the first on the list.
		
		DatabaseInfo	dbInfo = *(info.fAppList.begin ());

		//---------------------------------------------------------------------
		// If this is an executable, call SysUIAppSwitch
		//---------------------------------------------------------------------
		if (::IsExecutable (dbInfo.type, dbInfo.creator, dbInfo.dbAttrs))
		{
			Err err = ::SysUIAppSwitch (dbInfo.cardNo, dbInfo.dbID,
							sysAppLaunchCmdNormalLaunch, NULL);
			Errors::ThrowIfPalmError (err);
		}

		//---------------------------------------------------------------------
		// else, this must be a launchable data database. Find it's owner app
		//  and launch it with a pointer to the data database name.
		//---------------------------------------------------------------------
		else
		{
			DmSearchStateType	searchState;
			UInt16				cardNo;
			LocalID				dbID;
			Err err = ::DmGetNextDatabaseByTypeCreator (true, &searchState, 
						sysFileTApplication, dbInfo.creator, 
						true, &cardNo, &dbID);
			Errors::ThrowIfPalmError (err);

			// Create the param block
			emuptr	cmdPBP = (emuptr) ::MemPtrNew (sizeof (SysAppLaunchCmdOpenDBType));
			Errors::ThrowIfNULL ((void*) cmdPBP);

			// Fill it in
			::MemPtrSetOwner ((MemPtr) cmdPBP, 0);
			EmMemPut16 (cmdPBP + offsetof (SysAppLaunchCmdOpenDBType, cardNo), dbInfo.cardNo);
			EmMemPut32 (cmdPBP + offsetof (SysAppLaunchCmdOpenDBType, dbID), dbInfo.dbID);

			// Switch now
			err = ::SysUIAppSwitch (cardNo, dbID, sysAppLaunchCmdOpenDB, (MemPtr) cmdPBP);
			Errors::ThrowIfPalmError (err);
		}
	}

	this->Initialize (info.fNumber, info.fSteps, info.fFinal);

	gremlinStartTime = Platform::GetMilliseconds ();

	// Make sure the app's awake.  Normally, we post events on a patch to
	// SysEvGroupWait.  However, if the Palm device is already waiting,
	// then that trap will never get called.  By calling EvtWakeup now,
	// we'll wake up the Palm device from its nap.

	Errors::ThrowIfPalmError (EvtWakeup ());

	Hordes::TurnOn(true);

	if (info.fSaveFrequency != 0)
	{
		EmAssert (gSession);
		gSession->ScheduleAutoSaveState ();
	}
}



/************************************************************
 *
 * FUNCTION: Save
 *
 * DESCRIPTION: Saves Gremlin Info
 *
 * PARAMETERS: SessionFile to write to.
 *
 * RETURNS: Nothing.
 *
 *************************************************************/
void
Gremlins::Save (SessionFile& f)
{
	gremlinStopTime = Platform::GetMilliseconds ();

	const long	kCurrentVersion = 2;

	Chunk			chunk;
	EmStreamChunk	s (chunk);

	Bool hordesIsOn = Hordes::IsOn ();

	s << kCurrentVersion;

	s << keyProbabilitiesSum;
	s << lastPointY;
	s << lastPointX;
	s << lastPenDown;
	s << number;
	s << counter;
	s << finalUntil;
	s << saveUntil;
	s << inited;
	s << catchUp;
	s << needPenUp;
	s << charsToType;

	s << (hordesIsOn != false);
	s << gremlinStartTime;
	s << gremlinStopTime;

	s << gGremlinNext;

	GremlinInfo info;

	info.fAppList = gGremlinAppList;
	info.fNumber = number;
	info.fSaveFrequency = gGremlinSaveFrequency;
	info.fSteps = until;
	info.fFinal = finalUntil;

	s << info;

	f.WriteGremlinInfo (chunk);
}


/************************************************************
 *
 * FUNCTION: Load
 *
 * DESCRIPTION: Loads Gremlin Info
 *
 * PARAMETERS: SessionFile to read from.
 *
 * RETURNS: TRUE if a Gremlin state have been loaded and it
 *			is ON.
 *			FALSE otherwise.
 *
 *************************************************************/
Boolean
Gremlins::Load (SessionFile& f)
{
	Chunk	chunk;
	bool	fHordesOn;

	if (f.ReadGremlinInfo (chunk))
	{
		long			version;
		EmStreamChunk	s (chunk);

		s >> version;

		if (version >= 1)
		{
			s >> keyProbabilitiesSum;
			s >> lastPointY;
			s >> lastPointX;
			s >> lastPenDown;
			s >> number;
			s >> counter;
			s >> finalUntil;
			s >> saveUntil;
			s >> inited;
			s >> catchUp;
			s >> needPenUp;
			s >> charsToType;

			s >> fHordesOn;

			s >> gremlinStartTime;
			s >> gremlinStopTime;

			s >> gGremlinNext;

			// sync until to finalUntil

			until = finalUntil;

			// Patch up the start and stop times.

			int32	delta = gremlinStopTime - gremlinStartTime;
			gremlinStopTime = Platform::GetMilliseconds ();
			gremlinStartTime = gremlinStopTime - delta;

			// Reset keyProbabilitiesSum to zero so that it gets
			// recalculated.  Writing it out to the session file
			// was a bad idea.  The value written out may not be
			// appropriate for the version of Poser reading it in.

			keyProbabilitiesSum = 0;
		}

		if (version >= 2)
		{
			GremlinInfo	info;

			s >> info;

			Preference<GremlinInfo>	pref (kPrefKeyGremlinInfo);
			pref = info;
		}
	}

	return fHordesOn;
}


/************************************************************
 *
 * FUNCTION: SStatus
 *
 * DESCRIPTION: Return the gremlin number and counter.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: DoGremlins() in ShellCmdSys.cpp.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			roger	8/4/95	Created.
 *			dia	9/1/98	Allows for NULL parameters.
 *
 *************************************************************/
void Gremlins::SStatus(UInt16 *currentNumber, UInt32 *currentStep, 
	UInt32 *currentUntil)
{
	if (currentNumber) *currentNumber = number;
	if (currentStep) *currentStep = counter;
	if (currentUntil) *currentUntil = until;
}


/************************************************************
 *
 * FUNCTION: SetSeed
 *
 * DESCRIPTION: Allows the user to set the seed to be used.
 *
 * PARAMETERS: newSeed -		the new value of the seed.
 *
 * RETURNS: TRUE - seed value set to new seed, FALSE - value not set.
 * 
 * CALLED BY: Uncalled. (to be called from Debug Console)
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/2/95	Created.
 *
 *************************************************************/
Boolean Gremlins::SetSeed(UInt32 newSeed)
{
	if (newSeed > MAX_SEED_VALUE)
		return false;
	else
	{
		number = (UInt16) newSeed;
		srand(number);
		return true;
	}
}

/************************************************************
 *
 * FUNCTION: SetUntil
 *
 * DESCRIPTION: Allows the user to set the until value to be used.
 *
 * PARAMETERS: newUntil -		the new value of until.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: Hordes::Step
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/2/95	Created.
 *
 *************************************************************/
void Gremlins::SetUntil(UInt32 newUntil)
{
	until = newUntil;
#ifndef forSimulator
	saveUntil = until;
#endif
}

/************************************************************
 *
 * FUNCTION: RestoreFinalUntil
 *
 * DESCRIPTION: Restores the original max gremlins limit.
 *
 * CALLED BY: Hordes::Resume
 *
 *************************************************************/

void Gremlins::RestoreFinalUntil (void)
{
	until = finalUntil;
}

/************************************************************
 *
 * FUNCTION: Step
 *
 * DESCRIPTION: Allows Gremlins to go on step further then its
 *						set maximum.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: ObeyCommand() in EmEmulatorApp.cp.
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			David	8/1/95	Created.
 *
 *************************************************************/
void Gremlins::Step()
{
#ifndef forSimulator
	saveUntil = until;
#endif
	until = counter + 1;
}




/************************************************************
 *
 * FUNCTION: Resume
 *
 * DESCRIPTION: Resumes Gremlin
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 *************************************************************/
void
Gremlins::Resume (void)
{
	gremlinStartTime = Platform::GetMilliseconds () - (gremlinStopTime - gremlinStartTime);

	// Make sure we're all on the same page...
	::ResetCalibrationInfo ();

	// Make sure the app's awake.  Normally, we post events on a patch to
	// SysEvGroupWait.  However, if the Palm device is already waiting,
	// then that trap will never get called.  By calling EvtWakeup now,
	// we'll wake up the Palm device from its nap.

	Errors::ThrowIfPalmError (EvtWakeup ());
}




/************************************************************
 *
 * FUNCTION: Stop
 *
 * DESCRIPTION: Stops Gremlin
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 *************************************************************/
void
Gremlins::Stop (void)
{
	if (Hordes::IsOn())
	{
		Hordes::TurnOn(false);
		gremlinStopTime = Platform::GetMilliseconds ();

		unsigned short	number;
		unsigned long	step;
		unsigned long	until;
		this->SStatus (&number, &step, &until);

		if (LogGremlins ())
		{
			string	templ = Platform::GetString (kStr_GremlinEnded);
			LogAppendMsg (templ.c_str (),
				(int) number, step, until, (gremlinStopTime - gremlinStartTime));
		}

		LogDump ();
	}
}


/************************************************************
 *
 * FUNCTION: SendCharsToType
 *
 * DESCRIPTION: Send a char to the emulator if any are pending.
 *
 * PARAMETERS: None.
 *
 * RETURNS: 	true if a char was sent.
 * 
 * CALLED BY: 	GetFakeEvent
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			roger	10/04/95	Created.
 *			kwk	07/28/98	Queue double-byte characters correctly.
 *
 *************************************************************/
Boolean Gremlins::SendCharsToType()
{
	if (charsToType[0] != '\0')
	{
		WChar theChar;
		UInt16 charSize = TxtGetNextChar(charsToType, 0, &theChar);
		EmEventPlayback::RecordKeyEvent (theChar, 0, 0);
		StubAppEnqueueKey(theChar, 0, 0);
		PRINTF ("--- Gremlin #%ld Gremlins::SendCharsToType: key = %ld", (long) number, (long) theChar);
		strcpy(&charsToType[0], &charsToType[charSize]);
		return true;
	}

	return false;
}


/************************************************************
 *
 * FUNCTION: GetFakeEvent
 *
 * DESCRIPTION: Make a phony event for gremlin mode.
 *
 * PARAMETERS:  None
 *
 * RETURNS: TRUE if a key or point was enqueued, FALSE otherwise.
 * 
 * CALLED BY: TDEProcessMacEvents in EmEmulatorEvents.cp.
 *
 * REVISION HISTORY:
 *	06/01/95	rsf	Created by Roger Flores.
 *	07/31/95	David Moved to emulator level.
 *	08/28/98	kwk	Removed usage of TxtCharIsVirtual macro.
 *	07/03/99	kwk	Set command bit for page up/page down keydown
 *					events, since these are virtual (to match Graffiti behavior).
 *	06/04/01	kwk	Add support for Big-5 char encoding (Trad. Chinese).
 *
 *************************************************************/
Boolean Gremlins::GetFakeEvent()
{
	int chance;
	int i;
	int spaceLeft;
	PointType pen;
	const char *quote;

	PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: Entering", (long) number);

	if (! inited)
	{
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: not initialized; leaving", (long) number);
		return false;
	}

	// check to see if Gremlins has produced its max. # of "events."
	if (counter > until)
	{
		StubAppGremlinsOff ();
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: End of Days; leaving", (long) number);
		return false;
	}

	// Added - during Gremlin runs, we found that the timeout
	// could get set to 30 seconds and that a Gremlin may type
	// characters for more than 30 seconds at a time.  EvtEnqueueKey
	// doesn't reset the event timer, so it was possible for the
	// device to go to sleep, even when typing was occuring.

	::EvtResetAutoOffTimer ();

	// check to see if the event loop needs time to catch up.
	if (catchUp)
	{
		EmEventPlayback::RecordNullEvent ();
		catchUp = false;
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: playing catchup; leaving", (long) number);
		return false;
	}
#ifdef forSimulator
	counter++;
#endif

	// if no control object was entered, return a pen up.
	if (needPenUp)
	{
		pen.x = -1;
		pen.y = -1;
		lastPointX = pen.x;
		lastPointY = pen.y;
		lastPenDown = false;
		needPenUp = false;
		EmEventPlayback::RecordPenEvent (pen);
		StubAppEnqueuePt(&pen);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted pen up; leaving", (long) number);
		return true;
	}

	// If we've queued up a quote string, and there are still characters to
	// send, do so now.

	if (SendCharsToType())
	{
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: sent chars to type (1); leaving", (long) number);
		return true;
	}

	chance = randPercent;

	// Now fake an input
	if ((chance < KEY_DOWN_EVENT_WITHOUT_FOCUS_CHANCE)
	 || (chance < KEY_DOWN_EVENT_WITH_FOCUS_CHANCE && IsFocus()))
	{
		if ((randPercent < TYPE_QUOTE_CHANCE) && IsFocus())
		{
			const char** quoteArray = kAsciiQuotes;
			
			// 80% of the time we'll use text that's appropriate for the device's
			// character encoding. The other 20%, we'll use 7-bit ASCII.
			if (randN(10) < 8)
			{
				UInt32 encoding;
				if (FtrGet(sysFtrCreator, sysFtrNumEncoding, &encoding) != errNone)
				{
					encoding = charEncodingPalmLatin;
				}
				
				for (UInt16 i = 0; i < sizeof(kQuotesInfo) / sizeof(QuotesInfoType); i++)
				{
					if (kQuotesInfo[i].charEncoding == encoding)
					{
						quoteArray = kQuotesInfo[i].strings;
						break;
					}
				}
			}

			quote = quoteArray[randN(NUM_OF_QUOTES)];
			strcat(charsToType, quote);
			
			// The full field functionality doesn't need to be tested much
			// If charsToType is more than the space remaining in the current
			// field, then for each char past the full point give 1/3 chance to
			// stop at that char.
			spaceLeft = SpaceLeftInFocus();
			if (strlen(charsToType) > (size_t) spaceLeft) {
				UInt32 charStart, charEnd;
				TxtCharBounds(charsToType, spaceLeft, &charStart, &charEnd);
				i = charStart;
				while (charsToType[i] != '\0') {
					if (randPercent < 33) {
						charsToType[i] = '\0';
						break;
					} else {
						i += TxtNextCharSize(charsToType, i);
					}
				}
			}

			Bool	result = SendCharsToType ();

			if (!result)
				EmEventPlayback::RecordNullEvent ();

			PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: sent chars to type (2); leaving", (long) number);
			return result;
			}
		else
			{
			if (keyProbabilitiesSum == 0) {
				for (Int16 i = 0; i < NUM_OF_KEYS; i++) {
					if ((i > 0x00FF)
					|| ((TxtByteAttr(i) & byteAttrSingle) != 0)) {
						keyProbabilitiesSum += chanceForKey[i];
					}
				}
			}

			chance = randN(keyProbabilitiesSum);

			// Skip chars which cannot be single-byte, since we don't want to
			// generate bogus high-byte values.
			
			for (i = 0; i < NUM_OF_KEYS; i++) {
				if ((i < 0x0100)
				&& ((TxtByteAttr(i) & byteAttrSingle) == 0)) {
					continue;
				} else if (chance < chanceForKey[i]) {
					break;
				} else {
					chance -= chanceForKey[i];
				}
			}

			// There's a fractional chance for a greater number to be generated.  If
			// so we do nothing.
			if (i >= NUM_OF_KEYS)
				return false;
			
			if ((i > 255) || (i == chrPageUp) || (i == chrPageDown))
			{
				EmEventPlayback::RecordKeyEvent (i, 0, commandKeyMask);
				StubAppEnqueueKey(i, 0, commandKeyMask);
			}
			else
			{
				EmEventPlayback::RecordKeyEvent (i, 0, 0);
				StubAppEnqueueKey(i, 0, 0);
			}

			PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted key = %ld; leaving", (long) number, i);
			return true;
			}
	}		

	else if (chance < PEN_DOWN_EVENT_CHANCE)
	{
		needPenUp = true;

		FakeEventXY(&pen.x, &pen.y);
	
		lastPointX = pen.x;
		lastPointY = pen.y;
		lastPenDown = true;
		EmEventPlayback::RecordPenEvent (pen);
		StubAppEnqueuePt(&pen);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted pen event = (%ld, %ld), leaving",
				(long) number, (long) pen.x, (long) pen.y);

		// Draw a test pixel on the overlay				
		StubViewDrawPixel(pen.x, pen.y);
		return true;
	}


	else if (chance < MENU_EVENT_CHANCE)
	{
		EmEventPlayback::RecordKeyEvent (vchrMenu, vchrMenu, commandKeyMask);
		StubAppEnqueueKey(vchrMenu, vchrMenu, commandKeyMask);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted key = vchrMenu, leaving", (long) number);
		return true;
	}


	else if (chance < FIND_EVENT_CHANCE)
	{
		EmEventPlayback::RecordKeyEvent (vchrFind, vchrFind, commandKeyMask);
		StubAppEnqueueKey(vchrFind, vchrFind, commandKeyMask);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted key = vchrFind, leaving", (long) number);
		return true;
	}


	else if (chance < KEYBOARD_EVENT_CHANCE)
	{
		EmEventPlayback::RecordKeyEvent (vchrKeyboard, vchrKeyboard, commandKeyMask);
		StubAppEnqueueKey(vchrKeyboard, vchrKeyboard, commandKeyMask);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted key = vchrKeyboard, leaving", (long) number);
		return true;
	}


	else if (chance < LOW_BATTERY_EVENT_CHANCE)
	{
		EmEventPlayback::RecordKeyEvent (vchrLowBattery, vchrLowBattery, commandKeyMask);
		StubAppEnqueueKey(vchrLowBattery, vchrLowBattery, commandKeyMask);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted key = vchrLowBattery, leaving", (long) number);
		return true;
	}


	else if (chance < APP_SWITCH_EVENT_CHANCE)
	{
		// Modify the standard APP_SWITCH_EVENT_CHANCE by another factor
		// of 5%.  Without it, we enter this code way too often, and
		// Gremlins slows down a LOT! (Like, by a factor of 2.3).

		if (randPercent < 5)
		{
			DatabaseInfoList	appList = Hordes::GetAppList ();

			if (appList.size () > 0)
			{
				// Switch to a random app on the list.

				DatabaseInfo&	dbInfo = appList [randN (appList.size ())];

				//---------------------------------------------------------------------
				// If this is an executable, call SysUIAppSwitch
				//---------------------------------------------------------------------
				if (::IsExecutable (dbInfo.type, dbInfo.creator, dbInfo.dbAttrs))
				{
					EmuAppInfo		currentApp = EmPatchState::GetCurrentAppInfo ();
	
					EmEventPlayback::RecordSwitchEvent (dbInfo.cardNo, dbInfo.dbID,
						currentApp.fCardNo, currentApp.fDBID);

					Err err = ::SysUIAppSwitch (dbInfo.cardNo, dbInfo.dbID,
									sysAppLaunchCmdNormalLaunch, NULL);
					Errors::ThrowIfPalmError (err);

					PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: switched to app %s, leaving",
						(long) number, dbInfo.name);

					return true;
				}

				//---------------------------------------------------------------------
				// else, say we tried and call it quits by falling through
				//---------------------------------------------------------------------

			}
		}
	}

/*
	else if (chance < POWER_OFF_CHANCE)
	{
		EmEventPlayback::RecordKeyEvent (vchrAutoOff, vchrAutoOff, commandKeyMask);
		StubAppEnqueueKey(vchrAutoOff, vchrAutoOff, commandKeyMask);
		PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: posted key = vchrAutoOff, leaving", (long) number);
		return true;
	}
*/
	PRINTF ("--- Gremlin #%ld Gremlins::GetFakeEvent: exiting with no event.",
			(long) number);

	// If nothing happened fall out to generate a nilEvent	

	EmEventPlayback::RecordNullEvent ();
	return false;
}


/************************************************************
 *
 * FUNCTION: GetPenMovement
 *
 * DESCRIPTION: Make a phony pen movement.
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 * CALLED BY: 
 *
 * REVISION HISTORY:
 *			Name	Date		Description
 *			----	----		-----------
 *			Roger	6/1/95	Created.
 *			David	7/31/95	Moved to emulator level.
 *
 *************************************************************/
void Gremlins::GetPenMovement()	
{
	// This function is not called anywhere that I can see.  And since it
	// calls FakeLocalMovement, which calls WinGetDisplayExtent, which
	// doesn't exist in PalmDebugger, out it goes...

#ifndef __DEBUGGER_APPLICATION__
	PointType	pen;


	// check to see if Gremlins has produced its max. # of "events."
/*	if (counter > until)
	{
		testMode &= ~gremlinsOn;
		theApp->UpdateMenus();
	}
*/
#ifdef forSimulator
	counter++;
#endif

	needPenUp = false;
	if (randPercent < PEN_MOVE_CHANCE)
	{
		if (lastPenDown)
		{
			// move a small distance from the last pen position
			if (randPercent < PEN_BIG_MOVE_CHANCE)
			{
				RandomScreenXY(&pen.x, &pen.y);
			}
			else
			{
			FakeLocalMovement(&pen.x, &pen.y, lastPointX, lastPointY);
			}
		}
		else
		{
			// start the pen anywhere!
			RandomScreenXY(&pen.x, &pen.y);
		}
		StubViewDrawLine(pen.x, pen.y, lastPointX, lastPointY);
	}
	else
	{
		lastPenDown = false;
		pen.x = -1;
		pen.y = -1;
		catchUp = true;
	}
	lastPointX = pen.x;
	lastPointY = pen.y;
	EmEventPlayback::RecordPenEvent (pen);
	StubAppEnqueuePt(&pen);
#endif

	PRINTF ("--- Gremlin #%ld Gremlins::GetPenMovement: pen = (%ld, %ld)",
			(long) number, (long) pen.x, (long) pen.y);
}


/************************************************************
 *
 * FUNCTION: BumpCounter
 *
 * DESCRIPTION: Bumps Gremlin event counter
 *
 * PARAMETERS: None.
 *
 * RETURNS: Nothing.
 * 
 *************************************************************/
void Gremlins::BumpCounter()
{
	PRINTF ("--- Gremlin #%ld: bumping counter", (long) number);
	++counter;
}
