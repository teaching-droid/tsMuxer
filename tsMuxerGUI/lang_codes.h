// Changed by teaching-droid, 2026 (https://github.com/teaching-droid/tsMuxer).
// Apache License 2.0, see LICENSE and NOTICE.
#ifndef LANG_CODES_H_
#define LANG_CODES_H_

#include <array>
#include <functional>

#include <QAbstractListModel>

class LangCodesModel : public QAbstractListModel
{
    Q_OBJECT
   public:
    LangCodesModel(QObject* parent = nullptr) : QAbstractListModel(parent) { onLanguageChanged(); }

    void onLanguageChanged();
    // How a code is shown. The rows always hold the terminological code, which is what the window matches tracks
    // against; with the bibliographic form chosen on the General tab, "ces (Czech)" is shown as "cze (Czech)".
    void setShownCode(std::function<QString(const QString&)> shownCode);

   private:
    struct QtvLangCode
    {
        const char* code;
        QString lang;
        QVariant toVariant(int role, const std::function<QString(const QString&)>& shownCode) const;
    };
    std::function<QString(const QString&)> m_shownCode;

    std::array<QtvLangCode, 13> m_shortLangList;
    std::array<QtvLangCode, 486> m_fullLangList;
    QtvLangCode m_undLang;

    static std::array<QtvLangCode, 13> getShortLangList();
    static std::array<QtvLangCode, 486> getFullLangList();

    static constexpr int UND_ROW_IDX = 0;
    static constexpr int COMMON_ROW_IDX = 1;
    static constexpr int ALL_ROW_IDX = COMMON_ROW_IDX + 1 + std::tuple_size<decltype(m_shortLangList)>::value;
    static constexpr int ROW_COUNT = 2 /* und + "common" */ + std::tuple_size<decltype(m_shortLangList)>::value +
                                     1 /* "all" */ + std::tuple_size<decltype(m_fullLangList)>::value;

    int rowCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
};

#endif
