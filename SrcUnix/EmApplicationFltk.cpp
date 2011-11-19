/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 1999-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#include "EmCommon.h"
#include "EmApplicationFltk.h"

#include "EmDlgFltk.h"			// HandleDialogs
#include "EmDocument.h"			// gDocument
#include "EmMenus.h"			// MenuInitialize
#include "EmWindowFltk.h"

#include <FL/Fl.H>				// Fl::wait
#include <FL/x.H>				// fl_display
#include <FL/Fl_Widget.H>

#ifdef __QNXNTO__
#include <sys/neutrino.h>
#endif

EmApplicationFltk*	gHostApplication;

// These variables are defined in Platform_Unix.cpp.

const double			kClipboardFreq = 0.1;
extern ByteList			gClipboardDataPalm;
extern ByteList			gClipboardDataHost;
extern omni_mutex		gClipboardMutex;
extern omni_condition	gClipboardCondition;
extern Bool				gClipboardHaveOutgoingData;
extern Bool				gClipboardNeedIncomingData;
extern Bool				gClipboardPendingIncomingData;
extern Bool				gClipboardHaveIncomingData;

// Little private widget used solely for receiving notification
// that clipboard data has arrived.

class EmClipboardWidget : public Fl_Widget
{
public:
	EmClipboardWidget () : Fl_Widget (0, 0, 0, 0) {}
	virtual ~EmClipboardWidget () {};
	int handle (int);
	void draw (void) {}
};


/***********************************************************************
 *
 * FUNCTION:	main
 *
 * DESCRIPTION:	Application entry point.  Creates the preferences and
 *				then the application object.  Uses the application
 *				object to initizalize, run, and shutdown the system.
 *				A top-level exception handler is also installed in
 *				order to catch any wayward exceptions and report them
 *				to the user with a Fatal Internal Error message.
 *
 * PARAMETERS:	Standard main parameters
 *
 * RETURNED:	Zero by default.  If a non-fatal error occurred, returns
 *				1.  If a fatal error occurred while running a Gremlin,
 *				returns 2.  This is handy for Palm's automated testing.
 *
 ***********************************************************************/

int main (int argc, char** argv)
{
	EmulatorPreferences		prefs;
	EmApplicationFltk		theApp;

	try
	{
		if (theApp.Startup (argc, argv))
		{
			theApp.Run ();
		}
	}
	catch (...)
	{
		// !!! TBD
//		::MessageBox (NULL, "Palm OS Emulator: Fatal Internal Error",
//			"Fatal Error", MB_OK);
	}

	theApp.Shutdown ();

	return
		gErrorHappened ? 2 :
		gWarningHappened ? 1 : 0;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::EmApplicationFltk
 *
 * DESCRIPTION:	Constructor.  Sets the globalhost application variable
 *				to point to us.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

EmApplicationFltk::EmApplicationFltk (void) :
	EmApplication (),
	fAppWindow (NULL)
{
	EmAssert (gHostApplication == NULL);
	gHostApplication = this;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::~EmApplicationFltk
 *
 * DESCRIPTION:	Destructor.  Closes our window and sets the host
 *				application variable to NULL.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

EmApplicationFltk::~EmApplicationFltk (void)
{
	delete fAppWindow;

	EmAssert (gHostApplication == this);
	gHostApplication = NULL;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::Startup
 *
 * DESCRIPTION:	Performes one-time startup initialization.
 *
 * PARAMETERS:	argc, argv from main()
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

Bool EmApplicationFltk::Startup (int argc, char** argv)
{
#ifdef __QNXNTO__
	// speed NTO up so the timing will be about right (for an x86 anyway)
	struct _clockperiod cpnew;
	struct _clockperiod old;

	cpnew.nsec = 5000000;
	cpnew.fract = 0;
	ClockPeriod(CLOCK_REALTIME, &cpnew, &old, 0);
//	printf ("%d %d %d\n", old.nsec, old.fract);
#endif

	// Initialize the base system.

	if (!EmApplication::Startup (argc, argv))
		return false;

	// Create our window.

	this->PrvCreateWindow (argc, argv);

	// Start up the sub-systems.

	::MenuInitialize (false);

	// Start the clipboard idling.
	// !!! Get rid of this special clipboard window.  I think that
	// we can roll this functionality into fAppWindow.

	(void) this->PrvGetClipboardWidget ();

	return true;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::Run
 *
 * DESCRIPTION:	Generally run the application.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmApplicationFltk::Run (void)
{
	this->HandleStartupActions ();

	while (1)
	{
		Fl::wait (0.1);

		if (this->GetTimeToQuit ())
			break;

		this->HandleIdle ();
	}
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::Shutdown
 *
 * DESCRIPTION:	Performs one-time shutdown operations.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmApplicationFltk::Shutdown (void)
{
	// Delete our window now, so that its position will be recorded
	// in the preferences before EmApplication::Shutdown saves them.

	delete fAppWindow;
	fAppWindow = NULL;

	// Perform common shutdown.

	EmApplication::Shutdown ();
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::HandleIdle
 *
 * DESCRIPTION:	Perform idle-time operations.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmApplicationFltk::HandleIdle (void)
{
	// Idle the clipboard.  Do this first, in case the CPU
	// thread is blocked waiting for the data.

	if (!this->PrvIdleClipboard ())
		return;	// CPU thread is still blocked.

	// Handle any modeless dialogs

	::HandleDialogs ();

	EmApplication::HandleIdle ();
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::PrvCreateWindow
 *
 * DESCRIPTION:	Create the window that displays the LCD/skin stuff,
 *				or the message saying to right-click to show a menu.
 *
 * PARAMETERS:	argc, argv from main()
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmApplicationFltk::PrvCreateWindow (int argc, char** argv)
{
	Fl::visual (FL_RGB);

	fAppWindow = new EmWindowFltk;
	fAppWindow->WindowInit ();
	fAppWindow->show (argc, argv);
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::PrvClipbardPeriodic
 *
 * DESCRIPTION:	Idle time routine called when it's time to check to
 *				see if there are any clipboard tasks to perform.
 *				Handles those tasks and then queues up another
 *				callback into us.
 *
 * PARAMETERS:	data - value passed in to Fl::add_timeout when this
 *					function was queued up for execution.
 *
 * RETURNED:	Nothing
 *
 ***********************************************************************/

void EmApplicationFltk::PrvClipboardPeriodic (void* data)
{
	EmApplicationFltk* This = (EmApplicationFltk*) data;
	(void) This->PrvIdleClipboard ();
	Fl::add_timeout (kClipboardFreq, &PrvClipboardPeriodic, This);
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::PrvGetClipboardWidget
 *
 * DESCRIPTION:	Return the widget being used to receive the message
 *				that clipboard data has arrived.  This clipboard is
 *				created on demand.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	Pointer to the "clipboard widget".
 *
 ***********************************************************************/

Fl_Widget* EmApplicationFltk::PrvGetClipboardWidget (void)
{
	if (!fClipboardWidget)
	{
		fClipboardWidget = new EmClipboardWidget;
		Fl::add_timeout (kClipboardFreq, &PrvClipboardPeriodic, this);
	}

	return fClipboardWidget;
}


/***********************************************************************
 *
 * FUNCTION:	EmApplicationFltk::PrvIdleClipboard
 *
 * DESCRIPTION:	Check to see if there is any incoming or outgoing
 *				clipboard data and handle it if there is.
 *
 * PARAMETERS:	None
 *
 * RETURNED:	TRUE if there is pending incoming data.  This helps
 *				prevent us from deadlocking with the CPU thread, since
 *				it's now blocked on us waiting for data.
 *
 ***********************************************************************/

Bool EmApplicationFltk::PrvIdleClipboard (void)
{
	omni_mutex_lock lock (gClipboardMutex);

	// See if there's anything outgoing.

	if (gClipboardHaveOutgoingData)
	{
		// Get the widget that FLTK requires for clipboard management.

		Fl_Widget* w = this->PrvGetClipboardWidget ();
		EmAssert (w);

		// Tell FLTK the data to make available to other X programs if they need
		// to know the current selection.  We make a local copy of the data in
		// case ... <something about race conditions>.

		fClipboardData = gClipboardDataHost;

		Fl::selection (*w, (const char*) &fClipboardData[0], (int) fClipboardData.size ());

		// Clear the flag saying that we changed the clipboard.

		gClipboardHaveOutgoingData = false;
	}

	// See if there's a request for incoming data.

	if (gClipboardNeedIncomingData)
	{
		// Clear the flag saying that we need to ask for the
		// current selection.

		gClipboardNeedIncomingData = false;

		// FLTK only sends a FL_PASTE event if a selection exists.  If there
		// is no selection, we note that ourselves instead of waiting for an
		// event that will never arrive.
		//
		// !!! Do we get a FL_PASTE event if there is a primary selection
		// but it can't be converted to XA_STRING?  We might still wait
		// forever in that case.

		if (XGetSelectionOwner (fl_display, XA_PRIMARY) != None)
		{
			gClipboardPendingIncomingData = true;

			// Get the widget that FLTK requires for clipboard management.

			Fl_Widget* w = PrvGetClipboardWidget ();
			EmAssert (w);

			// Our clipboard widget may get called in this
			// context.  It will try to acquire the clipboard
			// mutex, so give it up here.

			omni_mutex_unlock unlock (gClipboardMutex);

			// Tell FLTK to get the clipboard data.  Usually, this will
			// require sending a request to the X server to get the
			// data from the application holding the current selection.
			// When the remote application responds, we'll get notified
			// via an FL_PASTE event sent to our widget's handle() method.

			Fl::paste (*w);
		}
		else
		{
			gClipboardHaveIncomingData = true;
			gClipboardCondition.broadcast ();
		}
	}

	return !gClipboardPendingIncomingData;
}


/***********************************************************************
 *
 * FUNCTION:	EmClipboardWidget::handle
 *
 * DESCRIPTION:	Handle the event indicating that clipboard data has
 *				arrived from the selection owner.
 *
 * PARAMETERS:	event - number indicating the event that occurred.
 *
 * RETURNED:	Non-zero if we handled the event.
 *
 ***********************************************************************/

int EmClipboardWidget::handle (int event)
{
	// It's a "paste" event, meaning that our application has requested
	// data to paste, and it just showed up from the X server.

	if (event == FL_PASTE)
	{
		// Get exclusive access to our clipboard data.

		omni_mutex_lock lock (gClipboardMutex);

		// Say that the data is here.

		gClipboardPendingIncomingData = false;
		gClipboardHaveIncomingData = true;

		// Copy the data to the host data clipboard.  The Palm-specific
		// clipboard will remain empty, and the host data will get
		// convert to it on demand.

		gClipboardDataPalm.clear ();
		gClipboardDataHost.clear ();

		copy ((char*) Fl::e_text,
			  (char*) Fl::e_text + Fl::e_length,
			  back_inserter (gClipboardDataHost));

		// Tell the CPU thread that the new data is here.

		gClipboardCondition.broadcast ();

		// Return that we handled the event.

		return 1;
	}

	// Return that we didn't handle the event.

	return 0;
}
