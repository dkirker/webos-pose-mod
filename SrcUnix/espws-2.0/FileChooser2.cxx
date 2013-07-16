//
// "$Id: FileChooser2.cxx,v 1.22 2000/01/04 13:45:51 mike Exp $"
//
//   More FileChooser routines.
//
//   Copyright 1997-2000 by Easy Software Products.
//
//   These coded instructions, statements, and computer programs are the
//   property of Easy Software Products and are protected by Federal
//   copyright law.  Distribution and use rights are outlined in the file
//   "COPYING" which should have been included with this file.  If this
//   file is missing or damaged please contact Easy Software Products
//   at:
//
//       Attn: ESP Licensing Information
//       Easy Software Products
//       44141 Airport View Drive, Suite 204
//       Hollywood, Maryland 20636-3111 USA
//
//       Voice: (301) 373-9600
//       EMail: info@easysw.com
//         WWW: http://www.easysw.com
//
// Contents:
//
//   FileChooser::directory()  - Set the directory in the file chooser.
//   FileChooser::count()      - Return the number of selected files.
//   FileChooser::value()      - Return a selected filename.
//   FileChooser::up()         - Go up one directory.
//   FileChooser::newdir()     - Make a new directory.
//   FileChooser::rescan()     - Rescan the current directory.
//   FileChooser::fileListCB() - Handle clicks (and double-clicks) in the
//                               FileBrowser.
//   FileChooser::fileNameCB() - Handle text entry in the FileBrowser.
//

//
// Include necessary headers.
//

#include "FileChooser.h"
#include <FL/filename.H>
#include <FL/fl_ask.H>
#include <FL/x.H>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#if defined(WIN32) || defined(__EMX__)
#  include <direct.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif /* WIN32 || __EMX__ */


//
// 'FileChooser::directory()' - Set the directory in the file chooser.
//

void
FileChooser::directory(const char *d)	// I - Directory to change to
{
  char	pathname[1024],			// Full path of directory
	*pathptr,			// Pointer into full path
	*dirptr;			// Pointer into directory
  int	levels;				// Number of levels in directory


  // NULL == current directory
  if (d == NULL)
    d = ".";

  if (d[0] != '\0')
  {
    // Make the directory absolute...
#if defined(WIN32) || defined(__EMX__)
    if (d[0] != '/' && d[0] != '\\' && d[1] != ':')
#else
    if (d[0] != '/' && d[0] != '\\')
#endif /* WIN32 || __EMX__ */
      fl_filename_absolute(directory_, d);
    else
    {
      strncpy(directory_, d, sizeof(directory_) - 1);
      directory_[sizeof(directory_) - 1] = '\0';
    }

    // Strip any trailing slash and/or period...
    dirptr = directory_ + strlen(directory_) - 1;
    if (*dirptr == '.')
      *dirptr-- = '\0';
    if ((*dirptr == '/' || *dirptr == '\\') && dirptr > directory_)
      *dirptr = '\0';
  }
  else
    directory_[0] = '\0';

  // Clear the directory menu and fill it as needed...
  dirMenu->clear();
#if defined(WIN32) || defined(__EMX__)
  dirMenu->add("My Computer");
#else
  dirMenu->add("File Systems");
#endif /* WIN32 || __EMX__ */

  levels = 0;
  for (dirptr = directory_, pathptr = pathname; *dirptr != '\0';)
  {
    if (*dirptr == '/' || *dirptr == '\\')
    {
      // Need to quote the slash first, and then add it to the menu...
      *pathptr++ = '\\';
      *pathptr++ = '/';
      *pathptr++ = '\0';
      dirptr ++;

      dirMenu->add(pathname);
      levels ++;
      pathptr = pathname;
    }
    else
      *pathptr++ = *dirptr++;
  }

  if (pathptr > pathname)
  {
    *pathptr = '\0';
    dirMenu->add(pathname);
    levels ++;
  }

  dirMenu->value(levels);

  // Rescan the directory...
  rescan();
}


//
// 'FileChooser::count()' - Return the number of selected files.
//

int				// O - Number of selected files
FileChooser::count()
{
  int		i;		// Looping var
  int		count;		// Number of selected files
  const char	*filename;	// Filename in input field or list
  char		pathname[1024];	// Full path to file


  if (type_ != MULTI)
  {
    // Check to see if the file name input field is blank...
    filename = fileName->value();
    if (filename == NULL || filename[0] == '\0')
      return (0);

    // Is the file name a directory?
    if (directory_[0] != '\0')
      sprintf(pathname, "%s/%s", directory_, filename);
    else
    {
      strncpy(pathname, filename, sizeof(pathname) - 1);
      pathname[sizeof(pathname) - 1] = '\0';
    }

    if (fl_filename_isdir(pathname) && !directory_chooser_)
      return (0);
    else
      return (1);
  }

  for (i = 1, count = 0; i <= fileList->size(); i ++)
    if (fileList->selected(i))
    {
      // See if this file is a directory...
      filename = (char *)fileList->text(i);
      if (directory_[0] != '\0')
	sprintf(pathname, "%s/%s", directory_, filename);
      else
      {
	strncpy(pathname, filename, sizeof(pathname) - 1);
	pathname[sizeof(pathname) - 1] = '\0';
      }

      if (!fl_filename_isdir(pathname) || directory_chooser_)
	count ++;
    }

  return (count);
}


//
// 'FileChooser::value()' - Return a selected filename.
//

const char *			// O - Filename or NULL
FileChooser::value(int f)	// I - File number
{
  int		i;		// Looping var
  int		count;		// Number of selected files
  const char	*name;		// Current filename
  static char	pathname[1024];	// Filename + directory

// There seems to be a little bit of a bug...sometimes
// the returned path can start with '//'.  I think the
// Unix file system handles this OK ("ls //tmp" works),
// but lets clean it up anyway.

  if (strlen (directory_) >= 2 &&
      directory_[0] == '/' && directory_[1] == '/')
  {
      memmove (directory_, directory_ + 1, strlen (directory_));
  }

  if (type_ != MULTI)
  {
    name = fileName->value();
    if (name[0] == '\0')
      return (NULL);

    sprintf(pathname, "%s/%s", directory_, name);
    return ((const char *)pathname);
  }

  for (i = 1, count = 0; i <= fileList->size(); i ++)
    if (fileList->selected(i))
    {
      // See if this file is a directory...
      name = fileList->text(i);
      sprintf(pathname, "%s/%s", directory_, name);

      if (!fl_filename_isdir(pathname) || directory_chooser_)
      {
        // Nope, see if this this is "the one"...
	count ++;
	if (count == f)
          return ((const char *)pathname);
      }
    }

  return (NULL);
}


//
// 'FileChooser::value()' - Set the current filename.
//

void
FileChooser::value(const char *filename)	// I - Filename + directory
{
  int	i,					// Looping var
  	count;					// Number of items in list
  char	*slash;					// Directory separator
  char	pathname[1024];				// Local copy of filename


  // See if the filename is actually a directory...
  if (filename == NULL || fl_filename_isdir(filename))
  {
    // Yes, just change the current directory...
    directory(filename);
    return;
  }

  // Switch to single-selection mode as needed
  if (type_ == MULTI)
    type(SINGLE);

  // See if there is a directory in there...
  strncpy(pathname, filename, sizeof(pathname) - 1);
  pathname[sizeof(pathname) - 1] = '\0';

  if ((slash = strrchr(pathname, '/')) == NULL)
    slash = strrchr(pathname, '\\');

  if (slash != NULL)
  {
    // Yes, change the display to the directory... 
    *slash++ = '\0';
    directory(pathname);
  }
  else
    slash = pathname;

  // Set the input field to the remaining portion
  fileName->value(slash);
  fileName->position(0, strlen(slash));
  okButton->activate();

  // Then find the file in the file list and select it...
  count = fileList->size();

  for (i = 1; i <= count; i ++)
    if (strcmp(fileList->text(i), slash) == 0)
    {
      fileList->select(i);
      break;
    }
}


//
// 'FileChooser::up()' - Go up one directory.
//

void
FileChooser::up()
{
  char *slash;		// Trailing slash


  if ((slash = strrchr(directory_, '/')) == NULL)
    slash = strrchr(directory_, '\\');

  if (directory_[0] != '\0')
    dirMenu->value(dirMenu->value() - 1);

  if (slash != NULL)
    *slash = '\0';
  else
  {
    upButton->deactivate();
    directory_[0] = '\0';
  }

  rescan();
}


//
// 'FileChooser::newdir()' - Make a new directory.
//

void
FileChooser::newdir()
{
  const char	*dir;		// New directory name
  char		pathname[1024];	// Full path of directory


  // Get a directory name from the user
  if ((dir = fl_input("New Directory?", NULL)) == NULL)
    return;

  // Make it relative to the current directory as needed...
#if defined(WIN32) || defined(__EMX__)
  if (dir[0] != '/' && dir[0] != '\\' && dir[1] != ':')
#else
  if (dir[0] != '/' && dir[0] != '\\')
#endif /* WIN32 || __EMX__ */
    sprintf(pathname, "%s/%s", directory_, dir);
  else
  {
    strncpy(pathname, dir, sizeof(pathname) - 1);
    pathname[sizeof(pathname) - 1] = '\0';
  }

  // Create the directory; ignore EEXIST errors...
#if defined(WIN32) || defined(__EMX__)
  if (mkdir(pathname))
#else
  if (mkdir(pathname, 0777))
#endif /* WIN32 || __EMX__ */
    if (errno != EEXIST)
    {
      fl_alert("Unable to create directory!");
      return;
    }

  // Show the new directory...
  directory(pathname);
}


//
// 'FileChooser::rescan()' - Rescan the current directory.
//

void
FileChooser::rescan()
{
  // Clear the current filename
  fileName->value("");
  okButton->deactivate();

  // Build the file list...
  fileList->load(directory_);
}


//
// 'FileChooser::fileListCB()' - Handle clicks (and double-clicks) in the
//                               FileBrowser.
//

void
FileChooser::fileListCB()
{
  char	*filename,		// New filename
	pathname[1024];		// Full pathname to file


  filename = (char *)fileList->text(fileList->value());
  if (directory_[0] != '\0')
    sprintf(pathname, "%s/%s", directory_, filename);
  else
  {
    strncpy(pathname, filename, sizeof(pathname) - 1);
    pathname[sizeof(pathname) - 1] = '\0';
  }

  if (Fl::event_clicks())
  {
#if defined(WIN32) || defined(__EMX__)
    if ((strlen(pathname) == 2 && pathname[1] == ':') ||
        fl_filename_isdir(pathname))
#else
    if (fl_filename_isdir(pathname))
#endif /* WIN32 || __EMX__ */
    {
      directory(pathname);
      upButton->activate();
    }
    else
      window->hide();
  }
  else
  {
    fileName->value(filename);

    if (!fl_filename_isdir(pathname) || directory_chooser_)
      okButton->activate();
  }
}


//
// 'FileChooser::fileNameCB()' - Handle text entry in the FileBrowser.
//

void
FileChooser::fileNameCB()
{
  char		*filename,	// New filename
		*slash,		// Pointer to trailing slash
		pathname[1024];	// Full pathname to file
  int		i,		// Looping var
		min_match,	// Minimum number of matching chars
		max_match,	// Maximum number of matching chars
		num_files,	// Number of files in directory
		first_line;	// First matching line
  const char	*file;		// File from directory


  // Get the filename from the text field...
  filename = (char *)fileName->value();

  if (filename == NULL || filename[0] == '\0')
  {
    okButton->deactivate();
    return;
  }

#if defined(WIN32) || defined(__EMX__)
  if (directory_[0] != '\0' &&
      filename[0] != '/' &&
      filename[0] != '\\' &&
      !(isalpha(filename[0]) && filename[1] == ':'))
    sprintf(pathname, "%s/%s", directory_, filename);
  else
  {
    strncpy(pathname, filename, sizeof(pathname) - 1);
    pathname[sizeof(pathname) - 1] = '\0';
  }
#else
  if (directory_[0] != '\0' &&
      filename[0] != '/')
    sprintf(pathname, "%s/%s", directory_, filename);
  else
  {
    strncpy(pathname, filename, sizeof(pathname) - 1);
    pathname[sizeof(pathname) - 1] = '\0';
  }
#endif /* WIN32 || __EMX__ */

  if (Fl::event_key() == FL_Enter)
  {
    // Enter pressed - select or change directory...

#if defined(WIN32) || defined(__EMX__)
    if (((strlen(pathname) == 2 && pathname[1] == ':') ||
        fl_filename_isdir(pathname)) && !directory_chooser_)
#else
    if (fl_filename_isdir(pathname) && !directory_chooser_)
#endif /* WIN32 || __EMX__ */
      directory(pathname);
    else if (type_ == CREATE || access(pathname, 0) == 0)
    {
      // New file or file exists...  If we are in multiple selection mode,
      // switch to single selection mode...
      if (type_ == MULTI)
        type(SINGLE);

      // Hide the window to signal things are done...
      window->hide();
    }
    else
    {
      // File doesn't exist, so beep at and alert the user...
      // TODO: NEED TO ADD fl_beep() FUNCTION TO 2.0!
#ifdef WIN32
      MessageBeep(MB_ICONEXCLAMATION);
#else
      XBell(fl_display, 100);
#endif // WIN32

      fl_alert("Please choose an existing file!");
    }
  }
  else if (Fl::event_key() != FL_Delete)
  {
    // Check to see if the user has entered a directory...
    if ((slash = strrchr(filename, '/')) == NULL)
      slash = strrchr(filename, '\\');

    if (slash != NULL)
    {
      // Yes, change directories and update the file name field...
      if ((slash = strrchr(pathname, '/')) == NULL)
	slash = strrchr(pathname, '\\');

      if (slash > pathname)		// Special case for "/"
        *slash++ = '\0';
      else
        slash++;

      if (strcmp(filename, "../") == 0)	// Special case for "../"
        up();
      else
        directory(pathname);

      // If the string ended after the slash, we're done for now...
      if (*slash == '\0')
        return;

      // Otherwise copy the remainder and proceed...
      fileName->value(slash);
      fileName->position(strlen(slash));
      filename = slash;
    }

    // Other key pressed - do filename completion as possible...
    num_files  = fileList->size();
    min_match  = strlen(filename);
    max_match  = 100000;
    first_line = 0;

    for (i = 1; i <= num_files && max_match > min_match; i ++)
    {
      file = fileList->text(i);

#if defined(WIN32) || defined(__EMX__)
      if (strnicmp(filename, file, min_match) == 0)
#else
      if (strncmp(filename, file, min_match) == 0)
#endif // WIN32 || __EMX__
      {
        // OK, this one matches; check against the previous match
	if (max_match == 100000)
	{
	  // First match; copy stuff over...
	  strncpy(pathname, file, sizeof(pathname) - 1);
	  pathname[sizeof(pathname) - 1] = '\0';
	  max_match = strlen(pathname);

	  // And then make sure that the item is visible
          fileList->topline(i);
	  first_line = i;
	}
	else
	{
	  // Succeeding match; compare to find maximum string match...
	  while (max_match > min_match)
#if defined(WIN32) || defined(__EMX__)
	    if (strnicmp(file, pathname, max_match) == 0)
#else
	    if (strncmp(file, pathname, max_match) == 0)
#endif // WIN32 || __EMX__
	      break;
	    else
	      max_match --;

          // Truncate the string as needed...
          pathname[max_match] = '\0';
	}
      }
    }

    fileList->deselect(0);
    fileList->redraw();

    // If we have any matches, add them to the input field...
    if (first_line > 0 && min_match == max_match &&
        max_match == (int)strlen(fileList->text(first_line)))
      fileList->select(first_line);
    else if (max_match > min_match && max_match != 100000)
    {
      // Add the matching portion...
      fileName->replace(0, min_match, pathname);

      // Highlight it; if the user just pressed the backspace
      // key, position the cursor at the start of the selection.
      // Otherwise, put the cursor at the end of the selection so
      // s/he can press the right arrow to accept the selection
      // (Tab and End also do this for both cases.)
      if (Fl::event_key() == FL_BackSpace)
        fileName->position(min_match - 1, max_match);
      else
        fileName->position(max_match, min_match);
    }

    // See if we need to enable the OK button...
    sprintf(pathname, "%s/%s", directory_, fileName->value());

    if ((type_ == CREATE || access(pathname, 0) == 0) &&
        (!fl_filename_isdir(pathname) || directory_chooser_))
      okButton->activate();
    else
      okButton->deactivate();
  }
}


//
// End of "$Id: FileChooser2.cxx,v 1.22 2000/01/04 13:45:51 mike Exp $".
//
