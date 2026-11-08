// Copyright 2026 teaching-droid (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "systemcolours.h"

#include <QApplication>

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#include <qt_windows.h>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleHints>

// WHY THIS EXISTS. Qt 6.8 paints a selection in the colour it reads as the Windows 10 accent colour.
// It reads that colour from HKCU\Software\Microsoft\Windows\DWM\AccentColor, and Windows 7 has no
// such value. Qt then keeps an INVALID colour, and an invalid colour is painted as black, while the
// text drawn on it still takes the colour chosen in Windows under Selected Items. That hits the
// selected track row (a right click selects it), the entry under the mouse in the track list's
// right-click menu (its style sheet asks for palette(highlight)) and selected text in any edit box;
// with the Windows Classic theme also the file list, the entry under the mouse in an open drop-down
// list and in any other menu. With the standard Windows 7 colours it is white on black. With a
// scheme whose selected text is dark it is dark on black, and the item cannot be read. The link and
// accent colours come from the same value and are black for the same reason. Qt 5 took the
// selection from the Selected Items setting, as the native Windows controls do.
//
// The style below sees every palette Qt builds, including the ones it builds again when the Windows
// colours are changed while the window is open, and puts the matching Windows setting in place of
// any colour that came out invalid. A colour Qt did obtain is never touched, and in dark mode
// nothing is touched at all.
namespace
{
QColor windowsColour(const int index)
{
    const DWORD rgb = GetSysColor(index);
    return QColor(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

class SystemColourStyle : public QProxyStyle
{
   public:
    using QProxyStyle::polish;
    using QProxyStyle::QProxyStyle;

    void polish(QPalette& palette) override
    {
        QProxyStyle::polish(palette);
        if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark)
            return;
        const QColor selection = windowsColour(COLOR_HIGHLIGHT);
        const QColor selectedText = windowsColour(COLOR_HIGHLIGHTTEXT);
        const QColor link = windowsColour(COLOR_HOTLIGHT);
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        {
            // The text colour goes with the background it was chosen for, so both are taken from
            // the same Windows setting or neither is.
            if (!palette.color(group, QPalette::Highlight).isValid())
            {
                palette.setColor(group, QPalette::Highlight, selection);
                palette.setColor(group, QPalette::HighlightedText, selectedText);
            }
            if (!palette.color(group, QPalette::Accent).isValid())
                palette.setColor(group, QPalette::Accent, selection);
            if (!palette.color(group, QPalette::Link).isValid())
                palette.setColor(group, QPalette::Link, link);
            if (!palette.color(group, QPalette::LinkVisited).isValid())
                palette.setColor(group, QPalette::LinkVisited, link);
        }
    }
};
}  // namespace
#endif

void useSystemColoursWhereQtHasNone()
{
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    // Only where something is missing. On Windows 10 and 11 Qt obtains every colour, and the style
    // is then left exactly as Qt chose it, so nothing changes there.
    if (QApplication::palette().color(QPalette::Active, QPalette::Highlight).isValid())
        return;
    // The same style Qt picked, windowsvista on Windows 7, or whatever -style asked for, wrapped
    // rather than replaced, so everything it draws stays as it was.
    if (QStyle* const native = QStyleFactory::create(QApplication::style()->name()))
        QApplication::setStyle(new SystemColourStyle(native));
#endif
}
