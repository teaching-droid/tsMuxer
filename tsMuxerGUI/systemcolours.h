// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef SYSTEMCOLOURS_H
#define SYSTEMCOLOURS_H

// Gives the window back the Windows colours that Qt could not read for itself. Called once, after
// the QApplication exists and before the first window is created. It changes nothing where Qt found
// every colour, which is Windows 10 and 11, and nothing outside Windows.
void useSystemColoursWhereQtHasNone();

#endif  // SYSTEMCOLOURS_H
