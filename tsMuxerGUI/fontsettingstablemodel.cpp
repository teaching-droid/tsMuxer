// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#include "fontsettingstablemodel.h"

#include <QBrush>
#include <QColor>
#include <QFontDatabase>
#include <QFontInfo>
#include <QSettings>
#include <array>
#include <functional>

namespace
{
constexpr int NUM_ROWS = 4;
constexpr int NUM_COLUMNS = 2;

QString fontOptions(const QFont& font)
{
    QString optStr;
    if (font.italic())
        optStr += "Italic";
    if (font.bold())
    {
        if (!optStr.isEmpty())
            optStr += ',';
        optStr += "Bold";
    }
    if (font.underline())
    {
        if (!optStr.isEmpty())
            optStr += ',';
        optStr += "Underline";
    }
    if (font.strikeOut())
    {
        if (!optStr.isEmpty())
            optStr += ',';
        optStr += "Strikeout";
    }
    return optStr;
}

void stringToFontOptions(const QString& serialized, QFont& font)
{
    font.setItalic(serialized.contains("Italic"));
    font.setBold(serialized.contains("Bold"));
    font.setUnderline(serialized.contains("Underline"));
    font.setStrikeOut(serialized.contains("Strikeout"));
}

int colorLight(QColor color) { return (0.257 * color.red()) + (0.504 * color.green()) + (0.098 * color.blue()) + 16; }
}  // namespace

// The subtitle font is Arial unless another is chosen, and a Linux system has no Arial: the meta asked for a font the
// muxer could not find there. Where the family is not installed, the font the system draws in its place is named
// instead, which is one the muxer finds. Windows has Arial, and draws a font it lacks with one of its own.
static void useInstalledFamily(QFont& font)
{
#ifndef Q_OS_WIN
    if (!QFontDatabase::hasFamily(font.family()))
    {
        const QString drawnWith = QFontInfo(font).family();
        if (!drawnWith.isEmpty())
            font.setFamily(drawnWith);
    }
#else
    Q_UNUSED(font);
#endif
}

FontSettingsTableModel::FontSettingsTableModel(QObject* parent) : QAbstractTableModel(parent)
{
    useInstalledFamily(m_font);
}

// The font, its size, colour and style are read and written through the window's own settings object. This model used
// a QSettings of its own, which is always the registry on Windows, so with a tsMuxerGUI.ini beside the program every
// other choice went to the ini but these four went to the registry: a copy of the program carried along with its ini
// lost them, and wrote the registry of the machine it ran on. A key that is not there leaves the value as it is.
void FontSettingsTableModel::load(QSettings& settings)
{
    beginResetModel();
    settings.beginGroup("subtitles");
    QString fontName = settings.value("family").toString();
    if (fontName.isEmpty())
        fontName = settings.value("famaly").toString();  // the key versions before 2.6.15 wrote
    if (!fontName.isEmpty())
        m_font.setFamily(fontName);

    const int fontSize = settings.value("size").toInt();
    if (fontSize > 0)
        m_font.setPointSize(fontSize);

    // A colour that is not a number became 0, which is transparent black and was drawn as opaque black: it is left as
    // it was
    bool colorOk = false;
    const uint color = settings.value("color").toUInt(&colorOk);
    if (!settings.value("color").isNull() && colorOk)
        m_color = color;

    if (settings.contains("options"))
        stringToFontOptions(settings.value("options").toString(), m_font);
    useInstalledFamily(m_font);  // a family saved by an earlier version, Arial, that is not installed
    settings.endGroup();
    endResetModel();
}

void FontSettingsTableModel::save(QSettings& settings) const
{
    settings.beginGroup("subtitles");
    settings.setValue("family", m_font.family());
    settings.setValue("size", m_font.pointSize());
    settings.setValue("color", m_color);
    settings.setValue("options", fontOptions(m_font));
    settings.remove("famaly");
    settings.endGroup();
}

int FontSettingsTableModel::rowCount(const QModelIndex&) const { return NUM_ROWS; }

int FontSettingsTableModel::columnCount(const QModelIndex&) const { return NUM_COLUMNS; }

QVariant FontSettingsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
    {
        return QVariant();
    }
    switch (role)
    {
    case Qt::BackgroundRole:
        if (index.row() == 2 && index.column() == 1)
        {
            return QBrush(m_color);
        }
        else
        {
            return QVariant();
        }

    case Qt::ForegroundRole:
        if (index.row() == 2 && index.column() == 1)
        {
            if (colorLight(QColor(m_color)) < 128)
            {
                return QBrush(QColor(255, 255, 255, 255));
            }
            else
            {
                return QBrush(QColor(0, 0, 0, 255));
            }
        }
        else
        {
            return QVariant();
        }

    case Qt::DisplayRole:
    {
        const std::array<std::function<QVariant()>, 8> providers = {
            // column 0
            []() { return tr("Name:"); }, []() { return tr("Size:"); }, []() { return tr("Color:"); },
            []() { return tr("Options:"); },
            // column 1
            [this]() { return m_font.family(); }, [this]() { return m_font.pointSize(); },
            [this]() { return QString("0x%1").arg(m_color, 8, 16, QLatin1Char('0')); },
            [this]() { return fontOptions(m_font); }};
        auto idx = index.column() * NUM_ROWS + index.row();
        return idx < static_cast<int>(providers.size()) ? providers[idx]() : QVariant();
    }

    default:
        return QVariant();
    }
}

Qt::ItemFlags FontSettingsTableModel::flags(const QModelIndex& index) const
{
    return QAbstractTableModel::flags(index) & (~Qt::ItemIsEditable);
}

void FontSettingsTableModel::setFont(const QFont& font)
{
    m_font = font;
    emit dataChanged(index(0, 1), index(3, 1));
}

void FontSettingsTableModel::setColor(quint32 color)
{
    m_color = color;
    emit dataChanged(index(2, 1), index(2, 1));
}

void FontSettingsTableModel::onLanguageChanged() { emit dataChanged(index(0, 0), index(3, 0)); }
