/***************************************************************************
**
** Copyright (C) 2010, 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (directui@nokia.com)
**
** This file is part of libmeegotouch.
**
** If you have questions regarding the use of this file, please contact
** Nokia at directui@nokia.com.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include "mlocale.h"
#include "mlocale_p.h"

#include "debug.h"

#ifdef HAVE_ICU
#include <unicode/unistr.h>
#include <unicode/ucal.h>
#include <unicode/coll.h>
#include <unicode/fieldpos.h>
#include <unicode/datefmt.h>
#include <unicode/calendar.h>
#include <unicode/smpdtfmt.h> // SimpleDateFormat
#include <unicode/numfmt.h>
#include <unicode/uloc.h>
#include <unicode/dtfmtsym.h> // date format symbols
#include <unicode/putil.h> // u_setDataDirectory
#include <unicode/numsys.h>

using namespace icu;
#endif

#include <MDebug>
#include <QTranslator>
#include <QDir>
#include <QMetaProperty>
#include <QCoreApplication>
#include <QMutex>
#include <QDateTime>
#include <QPointer>
#include <QRegularExpression>

#ifdef HAVE_ICU
#include "mcollator.h"
#include "mcalendar.h"
#include "mcalendar_p.h"
#include "micuconversions.h"
#endif

#include "mlocaleabstractconfigitem.h"
#include "mlocaleabstractconfigitemfactory.h"
#include "mlocalenullconfigitemfactory.h"

namespace ML10N {

void MLocale::clearSystemDefault()
{
    if ( MLocale::s_systemDefault )
    {
        delete MLocale::s_systemDefault;
        MLocale::s_systemDefault = 0;
    }
}


static QPointer<QTranslator> s_ltrTranslator = 0;
static QPointer<QTranslator> s_rtlTranslator = 0;

static const MLocaleAbstractConfigItemFactory* g_pConfigItemFactory = 0;

namespace
{
    const char *const BackupNameFormatString = "%d%t%g%t%m%t%f";
    const QString RtlLanguages("ar:fa:he:ps:ur:");
    const char *const Languages = "Languages";
    const char *const Countries = "Countries";

    const QString SettingsLanguage("/meegotouch/i18n/language");
    const QString SettingsLcTime("/meegotouch/i18n/lc_time");
    const QString SettingsLcTimeFormat24h("/meegotouch/i18n/lc_timeformat24h");
    const QString SettingsLcCollate("/meegotouch/i18n/lc_collate");
    const QString SettingsLcNumeric("/meegotouch/i18n/lc_numeric");
    const QString SettingsLcMonetary("/meegotouch/i18n/lc_monetary");
    const QString SettingsLcTelephone("/meegotouch/i18n/lc_telephone");
    QMap<QString, QString> gconfLanguageMap;
}

/// Helper
// Copied from Qt's QCoreApplication
static void replacePercentN(QString *result, int n)
{
    if (n >= 0) {
        int percentPos = 0;
        int len = 0;
        while ((percentPos = result->indexOf(QLatin1Char('%'), percentPos + len)) != -1) {
            len = 1;
            //TODO replace fmt to other type to do our own native digit conversions
            QString fmt;
            if (result->at(percentPos + len) == QLatin1Char('L')) {
                ++len;
                fmt = QLatin1String("%L1");
            } else {
                fmt = QLatin1String("%1");
            }
            if (result->at(percentPos + len) == QLatin1Char('n')) {
                fmt = fmt.arg(n);
                ++len;
                result->replace(percentPos, len, fmt);
                len = fmt.length();
            }
        }
    }
}


/////////////////////////////////////////////////
//// The private Mtranslationcatalog class ////

//! \internal
class MTranslationCatalog: public QSharedData
{
public:

    MTranslationCatalog(const QString &name);
    virtual ~MTranslationCatalog();

    // called by detach
    MTranslationCatalog(const MTranslationCatalog &other);

    /*!
    * \brief Load the actual translation file using locale and category info
    *
    * As an example lets assume that
    *
    * - MLocale::translationPaths()
    *   is the list ("/usr/share/l10n/meegotouch", "/usr/share/l10n")
    * - the category is  MLocale::MLcMessages
    * - the name of the locale (returned by mlocale->categoryName(category))
    *   is "en_US"
    * - the base name of the translation file is "foo"
    *
    * then the function will try to load translation catalogs in the following order:
    *
    *   /usr/share/l10n/meegotouch/foo_en_US.qm
    *   /usr/share/l10n/meegotouch/foo_en_US
    *   /usr/share/l10n/meegotouch/foo_en.qm
    *   /usr/share/l10n/meegotouch/foo_en
    *   /usr/share/l10n/foo_en_US.qm
    *   /usr/share/l10n/foo_en_US
    *   /usr/share/l10n/foo_en.qm
    *   /usr/share/l10n/foo_en
    *
    * and return when the first translation catalog was found.
    * If no translation can be found this function returns false.
    *
    * The way locale specific parts are successively cut away from the
    * translation file name is inherited from
    * <a href="http://qt.nokia.com/doc/4.6/qtranslator.html#load">QTranslator::load()</a>
    * because this is used to load the translation files.
    *
    * @param mlocale the locale for which the translations are loaded
    * @param category  this is usually MLocale::MLcMessages but it could
    *                  also be MLocale::MLcTime, MLocale::MLcNumeric,
    *                  etc...
    *
    * @return true if translations could be found, false if not.
    */
    bool loadWith(MLocale *mlocale, MLocale::Category category);

    // the abstract name for a translation. together with locale info and
    // category a concrete path is created when the file is loaded
    QString _name;

    // the actual translator
    QTranslator _translator;

private:
    MTranslationCatalog &operator=(const MTranslationCatalog &other);
};
//! \internal_end

// //////
// MTranslationCatalog implementation

MTranslationCatalog::MTranslationCatalog(const QString &name)
    : _name(name), _translator()
{
    // nothing
}

MTranslationCatalog::~MTranslationCatalog()
{
    // nothing
}

MTranslationCatalog::MTranslationCatalog(const MTranslationCatalog &other)
    : QSharedData(), _name(other._name)
{
    // nothing
}

bool MTranslationCatalog::loadWith(MLocale *mlocale, MLocale::Category category)
{
    QStringList localeDirs;
    QString fname;
    if (QFileInfo(_name).isRelative()) {
        localeDirs = MLocale::translationPaths();
        fname = _name;
    }
    else {
        localeDirs = (QStringList() << QFileInfo(_name).path());
        fname = QFileInfo(_name).fileName();
    }

    const int size = localeDirs.size();
    for (int i = 0; i < size; ++i) {
        QString prefix = QDir(localeDirs.at(i)).absolutePath();
        if (prefix.length() && !prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        QString realname;

        if (fname.endsWith(QLatin1String(".qm"))) {
            // this is either engineering English or the locale
            // specific parts of the file name have been fully
            // specified already. We don’t want any fallbacks in that
            // case, we try to load only the exact file name:
            realname = prefix + fname;
            if(QFileInfo(realname).isReadable() && _translator.load(realname))
                return true;
        }
        else {
            QString delims("_.@");
            QString engineeringEnglishName = fname;
            fname += '_' + mlocale->categoryName(category);
            for (;;) {
                realname = prefix + fname + ".qm";
                if (QFileInfo(realname).isReadable() && _translator.load(realname))
                    return true;
                realname = prefix + fname;
                if (QFileInfo(realname).isReadable() && _translator.load(realname))
                    return true;

                int rightmost = 0;
                for (int i = 0; i < (int)delims.length(); i++) {
                    int k = fname.lastIndexOf(delims[i]);
                    if (k > rightmost)
                        rightmost = k;
                }

                // no truncations?
                if (rightmost == 0)
                    break;

                fname.truncate(rightmost);

                if (fname == engineeringEnglishName) {
                    // do not fall back to engineering English when
                    // trying to load real translations. But if this
                    // point is reached, it means that no real
                    // translations were found for the requested
                    // locale. As a last fallback, try to load the
                    // real English translations (not the engineering
                    // English) here.
                    realname = prefix + fname + "_en.qm";
                    if (QFileInfo(realname).isReadable() && _translator.load(realname))
                        return true;
                    // nothing at all was found
                    break;
                }
            }
        }
    }
    // Loading the new file into the QTranslator failed.
    // Clear any old contents of the QTranslator before returning false.
    // This is necessary because the QTranslator may still have old contents.
    // For example, assume that an Arabic translation "foo_ar.qm" has been loaded
    // into the translator before and now this loadWith() function tries to
    // load "foo_de.qm" because the language has been switched to German
    // but "foo_de.qm" does not exist. We do *not* want to keep the previous
    // "foo_ar.qm" contents in that case.
    bool _ = _translator.load("", 0);
    Q_UNUSED(_)
    return false;
}

////////////////////////////////
//// Private stuff for MLocale

QStringList MLocalePrivate::translationPaths;
QStringList MLocalePrivate::dataPaths;

#ifdef HAVE_ICU
bool MLocalePrivate::truncateLocaleName(QString *localeName)
{
    // according to http://userguide.icu-project.org/locale the separators
    // that specify the parts of a locale are "_", "@", and ";", e.g.
    // in sr_Latn_RS_REVISED@currency=USD;calendar=islamic-civil
    // so we remove them from the end of the locale string.

    int semicolonIndex = localeName->lastIndexOf(';');
    if (semicolonIndex != -1)
    {
        // found semicolon, remove it and remaining part of string
        localeName->truncate(semicolonIndex);
        return true;
    }
    else
    {
        int atIndex = localeName->lastIndexOf('@');
        if (atIndex != -1)
        {
            // found "@", remove it and remaining part of string
            localeName->truncate(atIndex);
            return true;
        }
        else
        {
            int underscoreIndex = localeName->lastIndexOf('_');
            if (underscoreIndex != -1)
            {
                // found "_", remove it and remaining part of string
                localeName->truncate(underscoreIndex);
                return true;
            }
        }
    }
    // no truncation possible
    return false;
}
#endif

#ifdef HAVE_ICU
icu::DateFormatSymbols *MLocalePrivate::createDateFormatSymbols(const icu::Locale &locale)
{
    // This is a bit dirty but the only way to currently get the symbols
    // is like this. Only the internal API supports directly creating DateFormatSymbols
    // with an arbitrary calendar
    UErrorCode status = U_ZERO_ERROR;
    SimpleDateFormat dummyFormatter("", locale, status);

    if (U_FAILURE(status)) {
        return 0;
    }

    const DateFormatSymbols *dfs = dummyFormatter.getDateFormatSymbols();
    return new DateFormatSymbols(*dfs);
}
#endif

#ifdef HAVE_ICU
bool MLocalePrivate::isTwelveHours(const QString &icuFormatQString) const
{
    if (icuFormatQString.contains('\'')) {
        bool isQuoted = false;
        for (int i = 0; i < icuFormatQString.size(); ++i) {
            if (icuFormatQString[i] == '\'')
                isQuoted = !isQuoted;
            if (!isQuoted && icuFormatQString[i] == 'a')
                return true;
        }
        return false;
    }
    else {
        if(icuFormatQString.contains('a'))
            return true;
        else
            return false;
    }
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::dateFormatTo24h(icu::DateFormat *df) const
{
    if (df) {
        icu::UnicodeString icuFormatString;
        QString icuFormatQString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
        if (isTwelveHours(icuFormatQString)) {
            // remove unquoted 'a' characters and remove space left of 'a'
            // and change unquoted h -> H and K -> k
            QString tmp;
            bool isQuoted = false;
            for (int i = 0; i < icuFormatQString.size(); ++i) {
                QChar c = icuFormatQString[i];
                if (c == '\'')
                    isQuoted = !isQuoted;
                if (!isQuoted) {
                    if (c == 'h')
                        tmp.append("H");
                    else if (c == 'K')
                        tmp.append("k");
                    else if (c == 'a') {
                        if (tmp.endsWith(' ')) {
                            // remove space before 'a' if character
                            // after 'a' is space as well:
                            if (i < icuFormatQString.size() - 1
                                && icuFormatQString[i+1] == ' ')
                                tmp.remove(tmp.size()-1,1);
                            // remove space before 'a' if 'a' is last
                            // character in string:
                            if (i == icuFormatQString.size() - 1)
                                tmp.remove(tmp.size()-1,1);
                        }
                    }
                    else
                        tmp.append(c);
                }
                else {
                    tmp.append(c);
                }
            }
            icuFormatQString = tmp;
        }
        icuFormatString = MIcuConversions::qStringToUnicodeString(icuFormatQString);
        static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
    }
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::dateFormatTo12h(icu::DateFormat *df) const
{
    if (df) {
        icu::UnicodeString icuFormatString;
        QString icuFormatQString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
        if (!isTwelveHours(icuFormatQString)) {
            // change unquoted H -> h and k -> K
            // add 'a' at the right position (maybe adding a space as well)
            QString tmp;
            bool isQuoted = false;
            bool amPmMarkerWritten = false;
            QString language = categoryName(MLocale::MLcTime);
            bool writeAmPmMarkerBeforeHours = false;
            if (language.startsWith( QLatin1String("ja"))
                || language.startsWith( QLatin1String("zh")))
                writeAmPmMarkerBeforeHours = true;
            if (writeAmPmMarkerBeforeHours) {
                for (int i = 0; i < icuFormatQString.size(); ++i) {
                    QChar c = icuFormatQString[i];
                    if (c == '\'')
                        isQuoted = !isQuoted;
                    if (!isQuoted) {
                        if (c == 'H') {
                            if (!amPmMarkerWritten) {
                                tmp.append("a");
                                amPmMarkerWritten = true;
                            }
                            tmp.append("h");
                        }
                        else if (c == 'k') {
                            if (!amPmMarkerWritten) {
                                tmp.append("a");
                                amPmMarkerWritten = true;
                            }
                            tmp.append("K");
                        }
                        else
                            tmp.append(c);
                    }
                    else {
                        tmp.append(c);
                    }
                }
                icuFormatQString = tmp;
            }
            else {
                for (int i = 0; i < icuFormatQString.size(); ++i) {
                    QChar c = icuFormatQString[i];
                    if (c == '\'')
                        isQuoted = !isQuoted;
                    if (!isQuoted) {
                        if (c == 'H')
                            tmp.append("h");
                        else if (c == 'k')
                            tmp.append("K");
                        else if (c == 'z') {
                            if (!amPmMarkerWritten) {
                                if (!tmp.endsWith(' '))
                                    tmp.append(' ');
                                tmp.append("a ");
                                amPmMarkerWritten = true;
                            }
                            tmp.append(c);
                        }
                        else
                            tmp.append(c);
                    }
                    else {
                        tmp.append(c);
                    }
                }
                if (!amPmMarkerWritten)
                    tmp.append(" a");
                icuFormatQString = tmp;
            }
        }
        icuFormatString = MIcuConversions::qStringToUnicodeString(icuFormatQString);
        static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
    }
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::dateFormatToYearAndMonth(icu::DateFormat *df) const
{
    if (df) {
        icu::UnicodeString icuFormatString;
        QString icuFormatQString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
        QString categoryNameTime = categoryName(MLocale::MLcTime);
        QString categoryNameMessages = categoryName(MLocale::MLcMessages);
        if(categoryNameTime.startsWith("zh"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("yyyy年 LLLL"); // 2011年 十二月
            else
                icuFormatQString = QString::fromUtf8("yyyy LLLL");
        else if(categoryNameTime.startsWith("ja"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("yyyy年M月"); // 2011年12月
            else
                icuFormatQString = QString::fromUtf8("yyyy LLLL");
        else if(categoryNameTime.startsWith("ko"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("yyyy년 M월");
            else
                icuFormatQString = QString::fromUtf8("yyyy LLLL");
        else if(categoryNameTime.startsWith("vi"))
            icuFormatQString = QString::fromUtf8("LLLL - yyyy");
        else if(categoryNameTime.startsWith("eu")
                || categoryNameTime.startsWith("hu")
                || categoryNameTime.startsWith("ms"))
            icuFormatQString = QLatin1String("yyyy LLLL");
        else
            icuFormatQString = QLatin1String("LLLL yyyy");
        icuFormatString = MIcuConversions::qStringToUnicodeString(icuFormatQString);
        static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
    }
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::dateFormatToWeekdayAbbreviatedAndDayOfMonth(icu::DateFormat *df) const
{
    if (df) {
        icu::UnicodeString icuFormatString;
        QString icuFormatQString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
        QString categoryNameTime = categoryName(MLocale::MLcTime);
        QString categoryNameMessages = categoryName(MLocale::MLcMessages);
        if(categoryNameTime.startsWith("zh"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("d日ccc"); // 5日周一
            else
                icuFormatQString = QString::fromUtf8("d ccc");
        else if(categoryNameTime.startsWith("ja"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("d日(ccc)"); // 5日(月)
            else
                icuFormatQString = QString::fromUtf8("d ccc");
        else if(categoryNameTime.startsWith("ko"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("d일 ccc");
            else
                icuFormatQString = QString::fromUtf8("d ccc");
        else
            icuFormatQString = QLatin1String("ccc d");
        icuFormatString = MIcuConversions::qStringToUnicodeString(icuFormatQString);
        static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
    }
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::dateFormatToWeekdayWideAndDayOfMonth(icu::DateFormat *df) const
{
    if (df) {
        icu::UnicodeString icuFormatString;
        QString icuFormatQString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
        QString categoryNameTime = categoryName(MLocale::MLcTime);
        QString categoryNameMessages = categoryName(MLocale::MLcMessages);
        if(categoryNameTime.startsWith("zh"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("d日cccc"); // 5日星期一
            else
                icuFormatQString = QString::fromUtf8("d cccc");
        else if(categoryNameTime.startsWith("ja"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("d日(cccc)"); // 5日(月曜日)
            else
                icuFormatQString = QString::fromUtf8("d cccc");
        else if(categoryNameTime.startsWith("ko"))
            if(!mixingSymbolsWanted(categoryNameMessages, categoryNameTime))
                icuFormatQString = QString::fromUtf8("d일 cccc");
            else
                icuFormatQString = QString::fromUtf8("d cccc");
        else
            icuFormatQString = QLatin1String("cccc d");
        icuFormatString = MIcuConversions::qStringToUnicodeString(icuFormatQString);
        static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
    }
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::simplifyDateFormatForMixing(icu::DateFormat *df) const
{
    if (df) {
        icu::UnicodeString icuFormatString;
        QString icuFormatQString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
        QString categoryNameTime = categoryName(MLocale::MLcTime);
        QString categoryNameMessages = categoryName(MLocale::MLcMessages);
        QString categoryScriptTime = MLocale::localeScript(categoryNameTime);
        QString categoryScriptMessages = MLocale::localeScript(categoryNameMessages);
        // replace some known language specific stuff with something
        // generic which is understandable in a all languages or remove it
        // if there is no good generic replacement:
        if((categoryNameTime.startsWith("zh")
           || categoryNameTime.startsWith("ja"))
           && !categoryNameMessages.startsWith("zh")
           && !categoryNameMessages.startsWith("ja")) {
            // when mixing something *neither* Chinese *nor* Japanese,
            // into a Chinese or Japanese date format, replace the
            // Chinese characters with something understandable in the
            // non-CJ language.  If mixing versions of Chinese or
            // Japanese, do nothing. The only difference then is
            // whether the simplified 时 or the traditional character
            // 時 for hour is used.
            icuFormatQString.replace(QString::fromUtf8("年"), QLatin1String("-"));
            icuFormatQString.replace(QString::fromUtf8("月"), QLatin1String("-"));
            icuFormatQString.replace(QString::fromUtf8("日"), QLatin1String(""));
            icuFormatQString.replace(QString::fromUtf8("時"), QLatin1String(":"));
            icuFormatQString.replace(QString::fromUtf8("时"), QLatin1String(":"));
            icuFormatQString.replace(QString::fromUtf8("分"), QLatin1String(":"));
            icuFormatQString.replace(QString::fromUtf8("秒"), QLatin1String(""));
        }
        if(categoryNameTime.startsWith("ko")) {
            icuFormatQString.replace(QString::fromUtf8("년 "), QLatin1String("-"));
            icuFormatQString.replace(QString::fromUtf8("월 "), QLatin1String("-"));
            icuFormatQString.replace(QString::fromUtf8("일 "), QLatin1String(" "));
            icuFormatQString.replace(QString::fromUtf8("시 "), QLatin1String(":"));
            icuFormatQString.replace(QString::fromUtf8("분 "), QLatin1String(":"));
            icuFormatQString.replace(QString::fromUtf8("초"), QLatin1String(""));
        }
        // es_AR contains “hh'h'''mm:ss” or “HH'h'''mm:ss”
        icuFormatQString.replace(QLatin1String("h'h'''m"), QLatin1String("h:m"));
        icuFormatQString.replace(QLatin1String("H'h'''m"), QLatin1String("H:m"));
        // es_PE contains “hh'H'mm''ss''” or “HH'H'mm''ss''”
        icuFormatQString.replace(QLatin1String("h'H'm"), QLatin1String("h:m"));
        icuFormatQString.replace(QLatin1String("H'H'm"), QLatin1String("H:m"));
        icuFormatQString.replace(QLatin1String("m''s"), QLatin1String("m:s"));
        icuFormatQString.replace(QLatin1String("s''"), QLatin1String("s"));
        // eo contains “h-'a' 'horo' 'kaj' m:ss” or “H-'a' 'horo' 'kaj' m:ss” or “EEEE, d-'a' 'de' MMMM y”
        icuFormatQString.replace(QLatin1String("H-'a' 'horo' 'kaj' m:ss"), QLatin1String("HH:mm:ss"));
        icuFormatQString.replace(QLatin1String("h-'a' 'horo' 'kaj' m:ss"), QLatin1String("hh:mm:ss"));
        icuFormatQString.replace(QLatin1String("d-'a'"), QLatin1String("d "));
        // fa_IR may contain “ساعت” between date and time
        icuFormatQString.replace(QString::fromUtf8("ساعت"), QLatin1String(""));
        // pt_PT, ... contain “HH'h'mm'min'ss's' or “hh'h'mm'min'ss's'” and
        // en_BE, fr_CA, ... contain “HH 'h' mm 'min' ss 's'” or “hh 'h' mm 'min' ss 's'”:
        icuFormatQString.replace(QLatin1String("h'h'mm"), QLatin1String("h:mm"));
        icuFormatQString.replace(QLatin1String("h 'h' mm"), QLatin1String("h:mm"));
        icuFormatQString.replace(QLatin1String("H'h'mm"), QLatin1String("H:mm"));
        icuFormatQString.replace(QLatin1String("H 'h' mm"), QLatin1String("H:mm"));
        icuFormatQString.replace(QLatin1String("m'min's"), QLatin1String("m:s"));
        icuFormatQString.replace(QLatin1String("m 'min' s"), QLatin1String("m:s"));
        icuFormatQString.replace(QLatin1String("ss's'"), QLatin1String("ss"));
        icuFormatQString.replace(QLatin1String("ss 's'"), QLatin1String("ss"));
        // kk contains “'ж'.”
        icuFormatQString.replace(QString::fromUtf8("'ж'."), QLatin1String(""));
        // ru_RU contains “y 'г'.” (e.g. “2008 г.”)
        // (note the U+00A0 NO-BREAK SPACE in front of the “'г'.”):
        icuFormatQString.replace(QString::fromUtf8(" 'г'."), QLatin1String(""));
        // sv_SE contains “d:'e'” (e.g. “18:e”):
        icuFormatQString.replace(QLatin1String(":'e'"), QLatin1String(""));
        // sv_SE and nb_NO contain “'kl'.”
        icuFormatQString.replace(QLatin1String("'kl'."), QLatin1String(""));
        // uk_UA contains “y 'р'.” (e.g. “2008 р.”):
        icuFormatQString.replace(QString::fromUtf8("'р'."), QLatin1String(""));
        // remove remaining quoted stuff not covered by the special
        // cases above from the format strings, quoted stuff is
        // hardcoded text in the language of the the time category and
        // most likely not understandable in the language of the
        // message locale:
        icuFormatQString.replace(
            QRegularExpression("'[^']*'"),
            QLatin1String(""));
        // use stand-alone versions of month names and weekday names only
        // inflected versions will make no sense in the context of a different
        // language:
        icuFormatQString.replace(QLatin1String("EEEE"), QLatin1String("cccc"));
        icuFormatQString.replace(QLatin1String("MMMM"), QLatin1String("LLLL"));
        icuFormatQString.replace(QLatin1String("EEE"), QLatin1String("ccc"));
        icuFormatQString.replace(QLatin1String("MMM"), QLatin1String("LLL"));
        if(categoryNameTime.startsWith("th")) {
            // th_TH contains “H นาฬิกา m นาที ss วินาที”
            icuFormatQString.replace(QString::fromUtf8("H นาฬิกา m"), QLatin1String("H:m"));
            icuFormatQString.replace(QString::fromUtf8("h นาฬิกา m"), QLatin1String("h:m"));
            icuFormatQString.replace(QString::fromUtf8("m นาที s"), QLatin1String("m:s"));
            icuFormatQString.replace(QString::fromUtf8("s วินาที"), QLatin1String("s"));
            // th_TH contains “EEEEที่” or “ccccที่”
            icuFormatQString.replace(QString::fromUtf8("cที่"), QLatin1String("c"));
        }
        if((categoryNameTime.startsWith("zh")
           || categoryNameTime.startsWith("ja"))
           && !categoryNameMessages.startsWith("zh")
           && !categoryNameMessages.startsWith("ja")) {
            // when mixing a language which is *neither* Chinese *nor*
            // Japanese, into a Chinese or Japanese date format, add a
            // few spaces for better readability:
            icuFormatQString.replace(QLatin1String("cz"), QLatin1String("c z"));
            icuFormatQString.replace(QLatin1String("zH"), QLatin1String("z H"));
            icuFormatQString.replace(QLatin1String("za"), QLatin1String("z a"));
            icuFormatQString.replace(QLatin1String("ca"), QLatin1String("c a"));
            icuFormatQString.replace(QLatin1String("cH"), QLatin1String("c H"));
            icuFormatQString.replace(QLatin1String("ah"), QLatin1String("a h"));
            icuFormatQString.replace(QLatin1String("da"), QLatin1String("d a"));
            icuFormatQString.replace(QLatin1String("dH"), QLatin1String("d H"));
            icuFormatQString.replace(QLatin1String("dz"), QLatin1String("d z"));
            icuFormatQString.replace(QLatin1String("dccc"), QLatin1String("d ccc"));
        }
        if(categoryScriptTime == QLatin1String("Hebr")
           && categoryScriptMessages != QLatin1String("Hebr")) {
            // he_IL has “בMMMM” or “בLLLL”
            icuFormatQString.replace(QString::fromUtf8("בL"), QLatin1String("L"));
        }
        if(!categoryNameTime.startsWith("zh")
           && !categoryNameTime.startsWith("ja")
           && categoryScriptTime != QLatin1String("Arab")
           && categoryScriptTime != QLatin1String("Hebr")) {
            // remove remaining non-ASCII stuff which was not yet
            // specially handled above (Keep it if the time locale is
            // Chinese or Japanese or has Arabic or Hebrew script).
            QString tmp;
            for(int i = 0; i < icuFormatQString.size(); ++i)
                if(icuFormatQString.at(i) < QChar(0x0080))
                    tmp.append(icuFormatQString.at(i));
            icuFormatQString = tmp;
        }
        // remove superfluous whitespace:
        icuFormatQString = icuFormatQString.simplified();
        icuFormatString = MIcuConversions::qStringToUnicodeString(icuFormatQString);
        static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
    }
}
#endif


#ifdef HAVE_ICU
void MLocalePrivate::maybeEmbedDateFormat(icu::DateFormat *df, const QString &categoryNameMessages, const QString &categoryNameTime) const
{
    // If the message locale and the time locale have different script directions,
    // it may happen that the date format gets reordered in an unexpected way if
    // it is not used on its own but together with text from the message
    // locale. Protect the date format against such unexpected reordering by
    // wrapping it in RLE...PDF or LRE...PDF.
    if (df) {
        QString categoryScriptTime = MLocale::localeScript(categoryNameTime);
        QString categoryScriptMessages = MLocale::localeScript(categoryNameMessages);
        bool timeIsRtl = (categoryScriptTime == QLatin1String("Arab")
                          || categoryScriptTime == QLatin1String("Hebr"));
        bool messagesIsRtl = (categoryScriptMessages == QLatin1String("Arab")
                              || categoryScriptMessages == QLatin1String("Hebr"));
        if (timeIsRtl != messagesIsRtl) {
            icu::UnicodeString icuFormatString;
            QString icuFormatQString;
            static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
            icuFormatQString
                = MIcuConversions::unicodeStringToQString(icuFormatString);
            if(!icuFormatQString.isEmpty()) {
                if (timeIsRtl && !messagesIsRtl) {
                    icuFormatQString.prepend(QChar(0x202B)); // RIGHT-TO-LEFT EMBEDDING
                    icuFormatQString.append(QChar(0x202C));  // POP DIRECTIONAL FORMATTING
                }
                else if (!timeIsRtl && messagesIsRtl) {
                    icuFormatQString.prepend(QChar(0x202A)); // LEFT-TO-RIGHT EMBEDDING
                    icuFormatQString.append(QChar(0x202C));  // POP DIRECTIONAL FORMATTING
                }
                icuFormatString
                    = MIcuConversions::qStringToUnicodeString(icuFormatQString);
                static_cast<SimpleDateFormat *>(df)->applyPattern(icuFormatString);
            }
        }
    }
}
#endif

QString MLocalePrivate::fixCategoryNameForNumbers(const QString &categoryName) const
{
#ifdef HAVE_ICU
    Q_Q(const MLocale);
    QString categoryLanguage = parseLanguage(categoryName);
    // do nothing for languages other than ar, fa, hi, kn, mr, ne, pa, bn:
    if(categoryLanguage != "ar"
       && categoryLanguage != "fa"
       && categoryLanguage != "hi"
       && categoryLanguage != "kn"
       && categoryLanguage != "mr"
       && categoryLanguage != "ne"
       && categoryLanguage != "pa"
       && categoryLanguage != "bn")
        return categoryName;
    QString numericCategoryLanguage = q->categoryLanguage(MLocale::MLcNumeric);
    // if @numbers=<something> is already there, don’t touch it
    // and return immediately:
    if(!MIcuConversions::parseOption(categoryName, "numbers").isEmpty())
        return categoryName;
    if(categoryLanguage == "ar" && numericCategoryLanguage == "ar")
        return MIcuConversions::setOption(categoryName, "numbers", "arab");
    else if(categoryLanguage == "fa" && numericCategoryLanguage == "fa")
        return MIcuConversions::setOption(categoryName, "numbers", "arabext");
    else if(categoryLanguage == "hi" && numericCategoryLanguage == "hi")
        return MIcuConversions::setOption(categoryName, "numbers", "deva");
    else if(categoryLanguage == "kn" && numericCategoryLanguage == "kn")
        return MIcuConversions::setOption(categoryName, "numbers", "knda");
    else if(categoryLanguage == "mr" && numericCategoryLanguage == "mr")
        return MIcuConversions::setOption(categoryName, "numbers", "deva");
    else if(categoryLanguage == "ne" && numericCategoryLanguage == "ne")
        return MIcuConversions::setOption(categoryName, "numbers", "deva");
    else if(categoryLanguage == "or" && numericCategoryLanguage == "or")
        return MIcuConversions::setOption(categoryName, "numbers", "orya");
    else if(categoryLanguage == "pa" && numericCategoryLanguage == "pa")
        return MIcuConversions::setOption(categoryName, "numbers", "guru");
    else if(categoryLanguage == "bn" && numericCategoryLanguage == "bn")
        return MIcuConversions::setOption(categoryName, "numbers", "beng");
    return MIcuConversions::setOption(categoryName, "numbers", "latn");
#else
    // nothing to do without ICU:
    return categoryName;
#endif
}

#ifdef HAVE_ICU
QString MLocalePrivate::icuFormatString(MLocale::DateType dateType,
                                        MLocale::TimeType timeType,
                                        MLocale::CalendarType calendarType,
                                        MLocale::TimeFormat24h timeFormat24h) const
{
    icu::DateFormat *df = createDateFormat(dateType, timeType, calendarType, timeFormat24h);

    QString icuFormatQString;

    if (df)
    {
        icu::UnicodeString icuFormatString;
        static_cast<SimpleDateFormat *>(df)->toPattern(icuFormatString);
        icuFormatQString = MIcuConversions::unicodeStringToQString(icuFormatString);
    }
    return icuFormatQString;
}
#endif

#ifdef HAVE_ICU
bool MLocalePrivate::mixingSymbolsWanted(const QString &categoryNameMessages, const QString &categoryNameTime) const
{
    QString languageMessages = parseLanguage(categoryNameMessages);
    QString languageTime =  parseLanguage(categoryNameTime);
    QString categoryScriptTime = MLocale::localeScript(categoryNameTime);
    QString categoryScriptMessages = MLocale::localeScript(categoryNameMessages);
    bool timeIsRtl = (categoryScriptTime == QLatin1String("Arab")
                      || categoryScriptTime == QLatin1String("Hebr"));
    bool messagesIsRtl = (categoryScriptMessages == QLatin1String("Arab")
                          || categoryScriptMessages == QLatin1String("Hebr"));
    if (categoryNameTime.contains(QRegularExpression("@.*mix-time-and-language=yes"))) {
        return true;
    } else if(!categoryNameTime.contains(QRegularExpression("@.*mix-time-and-language=no"))
       && languageMessages != languageTime
       && languageMessages != "zh"
       && languageMessages != "ja"
       && languageMessages != "ko"
       && (timeIsRtl == messagesIsRtl)) {
        // mixing symbols like month name and weekday name from the
        // message locale into the date format of the time locale.
        // Don’t do this, if the language is the same, i.e. don’t do
        // it if one locale is “zh” and the other “zh_TW” or one
        // locale is “pt” and the other “pt_PT”. When the locales
        // share the same language, mixing should not be necessary,
        // the symbols should be understandable already.
        //
        // Disable the mixing *always* if the language is "zh", "ja"
        // or "ko", results of mixing a CJK language with a non-CJK
        // language are really weird, it is just nonsense to do this.
        // (See https://projects.maemo.org/bugzilla/show_bug.cgi?id=244444)
        //
        // Also disable mixing *always* if the message locale and the
        // time locale have use scripts with different direction, i.e.
        // do not attempt to do this mixing if one of the locales has
        // a right-to-left script and the other a left-to-right
        // script.  Mixing for locales with different script
        // directions almost always gives nonsensical results, trying
        // to fix this for all corner cases in
        // MLocalePrivate::simplifyDateFormatForMixing() is quite
        // hopeless.  (See also
        // https://projects.maemo.org/bugzilla/show_bug.cgi?id=270020)
        return true;
    } else {
        return false;
    }
}
#endif

#ifdef HAVE_ICU
icu::DateFormat *MLocalePrivate::createDateFormat(MLocale::DateType dateType,
                                                  MLocale::TimeType timeType,
                                                  MLocale::CalendarType calendarType,
                                                  MLocale::TimeFormat24h timeFormat24h) const
{
    QString categoryNameTime = categoryName(MLocale::MLcTime);
    QString categoryNameNumeric = categoryName(MLocale::MLcNumeric);
    QString categoryNameMessages = categoryName(MLocale::MLcMessages);
    QString key = QString("%1_%2_%3_%4_%5_%6_%7")
        .arg(dateType)
        .arg(timeType)
        .arg(calendarType)
        .arg(timeFormat24h)
        .arg(categoryNameTime)
        .arg(categoryNameNumeric)
        .arg(categoryNameMessages);
    if (_dateFormatCache.contains(key))
        return _dateFormatCache.object(key);
    categoryNameTime = fixCategoryNameForNumbers(
        MIcuConversions::setCalendarOption(categoryNameTime, calendarType));
    categoryNameMessages = fixCategoryNameForNumbers(
        MIcuConversions::setCalendarOption(categoryNameMessages, calendarType));
    icu::Locale calLocale = icu::Locale(qPrintable(categoryNameTime));
    icu::DateFormat::EStyle dateStyle;
    icu::DateFormat::EStyle timeStyle;
    if (dateType == MLocale::DateYearAndMonth
        || dateType == MLocale::DateWeekdayAbbreviatedAndDayOfMonth
        || dateType == MLocale::DateWeekdayWideAndDayOfMonth) {
        // doesn’t matter really will be customized anyway
        dateStyle = MIcuConversions::toEStyle(MLocale::DateFull);
        timeStyle = MIcuConversions::toEStyle(MLocale::TimeNone);
    }
    else {
        dateStyle = MIcuConversions::toEStyle(dateType);
        timeStyle = MIcuConversions::toEStyle(timeType);
    }
    icu::DateFormat *df
        = icu::DateFormat::createDateTimeInstance(dateStyle, timeStyle, calLocale);
    if (dateType == MLocale::DateYearAndMonth) {
        MLocalePrivate::dateFormatToYearAndMonth(df);
    }
    else if (dateType == MLocale::DateWeekdayAbbreviatedAndDayOfMonth) {
        MLocalePrivate::dateFormatToWeekdayAbbreviatedAndDayOfMonth(df);
    }
    else if (dateType == MLocale::DateWeekdayWideAndDayOfMonth) {
        MLocalePrivate::dateFormatToWeekdayWideAndDayOfMonth(df);
    }
    else if (timeType != MLocale::TimeNone) {
        switch (timeFormat24h) {
        case(MLocale::TwelveHourTimeFormat24h):
            MLocalePrivate::dateFormatTo12h(df);
            break;
        case(MLocale::TwentyFourHourTimeFormat24h):
            MLocalePrivate::dateFormatTo24h(df);
            break;
        case(MLocale::LocaleDefaultTimeFormat24h):
            break;
        default:
            break;
        }
    }
    if(mixingSymbolsWanted(categoryNameMessages, categoryNameTime)) {
        // If we are mixing really different languages, simplify the
        // date format first to make the results less bad:
        MLocalePrivate::simplifyDateFormatForMixing(df);
        DateFormatSymbols *dfs =
            MLocalePrivate::createDateFormatSymbols(
                icu::Locale(qPrintable(categoryNameMessages)));
        // This is not nice but seems to be the only way to set the
        // symbols with the public API
        static_cast<SimpleDateFormat *>(df)->adoptDateFormatSymbols(dfs);
    }
    MLocalePrivate::maybeEmbedDateFormat(df, categoryNameMessages, categoryNameTime);
    _dateFormatCache.insert(key, df);
    return df;
}
#endif

// Constructors

MLocalePrivate::MLocalePrivate()
    : _valid(true),
      _timeFormat24h(MLocale::LocaleDefaultTimeFormat24h),
      _phoneNumberGrouping( MLocale::DefaultPhoneNumberGrouping ),
#ifdef HAVE_ICU
      _numberFormat(0),
      _numberFormatLcTime(0),
#endif
      pCurrentLanguage(0),
      pCurrentLcTime(0),
      pCurrentLcTimeFormat24h(0),
      pCurrentLcCollate(0),
      pCurrentLcNumeric(0),
      pCurrentLcMonetary(0),
      pCurrentLcTelephone(0),
#ifdef HAVE_ICU
      _pDateTimeCalendar(0),
#endif
      q_ptr(0)
{
    lmlDebug( "MLocalePrivate ctor called" );

    if (translationPaths.isEmpty())
    {
#ifdef Q_OS_WIN
        // walk to translation dir relative to bin dir
        QDir appDir(QCoreApplication::applicationDirPath());

	appDir.cdUp();
	appDir.cd("share");
	appDir.cd("l10n");
	appDir.cd("meegotouch");

        translationPaths = (QStringList() << appDir.absolutePath());
#else
        translationPaths = (QStringList() << QString(TRANSLATION_DIR));
#endif
    }

    if (dataPaths.isEmpty())
        MLocale::setDataPath(ML_ICUEXTRADATA_DIR);
}

// copy constructor
MLocalePrivate::MLocalePrivate(const MLocalePrivate &other)
    : _valid(other._valid),
      _defaultLocale(other._defaultLocale),
      _messageLocale(other._messageLocale),
      _numericLocale(other._numericLocale),
      _collationLocale(other._collationLocale),
      _calendarLocale(other._calendarLocale),
      _monetaryLocale(other._monetaryLocale),
      _nameLocale(other._nameLocale),
      _telephoneLocale(other._telephoneLocale),
      _validCountryCodes( other._validCountryCodes ),
      _timeFormat24h(other._timeFormat24h),
      _phoneNumberGrouping( other._phoneNumberGrouping ),
#ifdef HAVE_ICU
      _numberFormat(0),
      _numberFormatLcTime(0),
#endif
      _messageTranslations(other._messageTranslations),
      _timeTranslations(other._timeTranslations),
      _trTranslations(other._trTranslations),

      pCurrentLanguage(0),
      pCurrentLcTime(0),
      pCurrentLcTimeFormat24h(0),
      pCurrentLcCollate(0),
      pCurrentLcNumeric(0),
      pCurrentLcMonetary(0),
      pCurrentLcTelephone(0),

#ifdef HAVE_ICU
      _pDateTimeCalendar(0),
#endif
      q_ptr(0)
{
#ifdef HAVE_ICU
    if (other._numberFormat != 0) {
        _numberFormat = static_cast<icu::NumberFormat *>((other._numberFormat)->clone());
    }
    if (other._numberFormatLcTime != 0) {
        _numberFormatLcTime = static_cast<icu::NumberFormat *>((other._numberFormatLcTime)->clone());
    }
#endif
}

MLocalePrivate::~MLocalePrivate()
{
#ifdef HAVE_ICU
    delete _numberFormat;
    delete _numberFormatLcTime;
    // note: if tr translations are inserted into QCoreApplication
    // deleting the QTranslator removes them from the QCoreApplication

    delete _pDateTimeCalendar;
    _pDateTimeCalendar = 0;
#endif

    delete pCurrentLanguage;
    delete pCurrentLcTime;
    delete pCurrentLcTimeFormat24h;
    delete pCurrentLcCollate;
    delete pCurrentLcNumeric;
    delete pCurrentLcMonetary;
    delete pCurrentLcTelephone;
}

MLocalePrivate &MLocalePrivate::operator=(const MLocalePrivate &other)
{
    _valid = other._valid;
    _defaultLocale = other._defaultLocale;
    _messageLocale = other._messageLocale;
    _numericLocale = other._numericLocale;
    _collationLocale = other._collationLocale;
    _calendarLocale = other._calendarLocale;
    _monetaryLocale = other._monetaryLocale;
    _nameLocale = other._nameLocale;
    _timeFormat24h = other._timeFormat24h;
    _messageTranslations = other._messageTranslations;
    _timeTranslations = other._timeTranslations;
    _trTranslations = other._trTranslations;
    _validCountryCodes = other._validCountryCodes;
    _telephoneLocale = other._telephoneLocale;

#ifdef HAVE_ICU
    delete _numberFormat;
    delete _numberFormatLcTime;

    if (other._numberFormat) {
        _numberFormat = static_cast<icu::NumberFormat *>((other._numberFormat)->clone());

    } else {
        _numberFormat = 0;
    }
    if (other._numberFormatLcTime) {
        _numberFormatLcTime = static_cast<icu::NumberFormat *>((other._numberFormatLcTime)->clone());

    } else {
        _numberFormatLcTime = 0;
    }
#endif

    return *this;
}

void MLocalePrivate::dropCaches()
{
#ifdef HAVE_ICU
    // call this function when the MLocale has changed so that
    // cached data cannot be used any more

    // delete MCalendar instance for this MLocale
    if ( _pDateTimeCalendar )
    {
        delete _pDateTimeCalendar;
        _pDateTimeCalendar = 0;
    }

    // drop cached formatString conversions
    _icuFormatStringCache.clear();
#endif
}

bool MLocalePrivate::isValidCountryCode( const QString& code ) const
{

    // no valid code starts with 0
    if (code.at(0) == '0') {
        return false;
    }

    // if the conversion fails, it will return 0, which is an invalid
    // code, so we don't need to check for the error.
    uint uIntCode = code.toUInt();

    switch (uIntCode) {
    case 1:
    case 20:
    case 212:
    case 213:
    case 214:
    case 215:
    case 216:
    case 218:
    case 219:
    case 220:
    case 221:
    case 222:
    case 223:
    case 224:
    case 225:
    case 226:
    case 227:
    case 228:
    case 229:
    case 230:
    case 231:
    case 232:
    case 233:
    case 234:
    case 235:
    case 236:
    case 237:
    case 238:
    case 239:
    case 240:
    case 241:
    case 242:
    case 243:
    case 244:
    case 245:
    case 246:
    case 247:
    case 248:
    case 249:
    case 250:
    case 251:
    case 252:
    case 253:
    case 254:
    case 255:
    case 256:
    case 257:
    case 258:
    case 259:
    case 260:
    case 261:
    case 262:
    case 263:
    case 264:
    case 265:
    case 266:
    case 267:
    case 268:
    case 269:
    case 27:
    case 290:
    case 291:
    case 297:
    case 298:
    case 299:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 350:
    case 351:
    case 352:
    case 353:
    case 354:
    case 355:
    case 356:
    case 357:
    case 358:
    case 359:
    case 36:
    case 370:
    case 371:
    case 372:
    case 373:
    case 374:
    case 375:
    case 376:
    case 377:
    case 378:
    case 379:
    case 380:
    case 381:
    case 382:
    case 385:
    case 386:
    case 387:
    case 388:
    case 389:
    case 39:
    case 40:
    case 41:
    case 420:
    case 421:
    case 423:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
    case 48:
    case 49:
    case 500:
    case 501:
    case 502:
    case 503:
    case 504:
    case 505:
    case 506:
    case 507:
    case 508:
    case 509:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    case 58:
    case 590:
    case 591:
    case 592:
    case 593:
    case 594:
    case 595:
    case 596:
    case 597:
    case 598:
    case 599:
    case 60:
    case 61:
    case 62:
    case 63:
    case 64:
    case 65:
    case 66:
    case 670:
    case 672:
    case 673:
    case 674:
    case 675:
    case 676:
    case 677:
    case 678:
    case 679:
    case 680:
    case 681:
    case 682:
    case 683:
    case 685:
    case 686:
    case 687:
    case 688:
    case 689:
    case 690:
    case 691:
    case 692:
    case 7:
    case 800:
    case 808:
    case 81:
    case 82:
    case 84:
    case 850:
    case 852:
    case 853:
    case 855:
    case 856:
    case 86:
    case 870:
    case 871:
    case 872:
    case 873:
    case 874:
    case 878:
    case 880:
    case 881:
    case 882:
    case 883:
    case 886:
    case 888:
    case 90:
    case 91:
    case 92:
    case 93:
    case 94:
    case 95:
    case 960:
    case 961:
    case 962:
    case 963:
    case 964:
    case 965:
    case 966:
    case 967:
    case 968:
    case 970:
    case 971:
    case 972:
    case 973:
    case 974:
    case 975:
    case 976:
    case 977:
    case 979:
    case 98:
    case 991:
    case 992:
    case 993:
    case 994:
    case 995:
    case 996:
    case 998:
        return true;
    default:
        return false;
    }
}

#ifdef HAVE_ICU
// creates icu::Locale presenting a category
Locale MLocalePrivate::getCategoryLocale(MLocale::Category category) const
{
    return icu::Locale(qPrintable(categoryName(category)));
}
#endif

// creates an QString for category or default as a fallback
QString MLocalePrivate::categoryName(MLocale::Category category) const
{
    switch (category) {
    case(MLocale::MLcMessages):
        if (!_messageLocale.isEmpty()) {
            return _messageLocale;
        }
        break;

    case(MLocale::MLcNumeric):
        if (!_numericLocale.isEmpty()) {
            return _numericLocale;
        }
        break;

    case(MLocale::MLcCollate):
        if (!_collationLocale.isEmpty()) {
            return _collationLocale;
        }
        break;

    case(MLocale::MLcMonetary):
        if (!_monetaryLocale.isEmpty()) {
            return _monetaryLocale;
        }
        break;

    case(MLocale::MLcTime):
        if (!_calendarLocale.isEmpty()) {
            return _calendarLocale;
        }
        break;

    case(MLocale::MLcName):
        if (!_nameLocale.isEmpty()) {
            return _nameLocale;
        }
        break;

    case(MLocale::MLcTelephone):
        if (!_telephoneLocale.isEmpty()) {
            return _telephoneLocale;
        }
        break;
    }

    return _defaultLocale;
}

void MLocalePrivate::loadTrCatalogs()
{
    Q_Q(MLocale);
    foreach(const QExplicitlySharedDataPointer<MTranslationCatalog>& sharedCatalog, _trTranslations) { // krazy:exclude=foreach
        if(sharedCatalog->_translator.isEmpty()
           || !sharedCatalog->_name.endsWith(QLatin1String(".qm"))) {
            sharedCatalog->loadWith(q, MLocale::MLcMessages);
        }
    }
}

void MLocalePrivate::insertTrToQCoreApp()
{
    foreach(const QExplicitlySharedDataPointer<MTranslationCatalog>& sharedCatalog, _trTranslations) { // krazy:exclude=foreach
        QCoreApplication::installTranslator(&sharedCatalog->_translator);
    }
}

void MLocalePrivate::removeTrFromQCoreApp()
{
    foreach(const QExplicitlySharedDataPointer<MTranslationCatalog>& sharedCatalog, _trTranslations) { // krazy:exclude=foreach
        QCoreApplication::removeTranslator(&sharedCatalog->_translator);
    }
}

void MLocalePrivate::insertDirectionTrToQCoreApp()
{
    if (s_rtlTranslator == 0) {
        s_rtlTranslator = new QTranslator( QCoreApplication::instance() );
        bool ok = s_rtlTranslator->load(":/libmeegotouch_rtl.qm");
	Q_UNUSED(ok);
        Q_ASSERT(ok);
    }
    if (s_ltrTranslator == 0) {
        s_ltrTranslator = new QTranslator( QCoreApplication::instance() );
        bool ok = s_ltrTranslator->load(":/libmeegotouch_ltr.qm");
	Q_UNUSED(ok);
        Q_ASSERT(ok);
    }

    if (MLocale::s_systemDefault->textDirection() == Qt::RightToLeft) {
        // make sure previous installations of the direction translators
        // are removed:
        QCoreApplication::removeTranslator(s_ltrTranslator);
        QCoreApplication::removeTranslator(s_rtlTranslator);
        // install the correct direction translator for the current
        // system default locale:
        QCoreApplication::installTranslator(s_rtlTranslator);
    }
    else {
        // make sure previous installations of the direction translators
        // are removed:
        QCoreApplication::removeTranslator(s_rtlTranslator);
        QCoreApplication::removeTranslator(s_ltrTranslator);
        // install the correct direction translator for the current
        // system default locale:
        QCoreApplication::installTranslator(s_ltrTranslator);
    }
}

QLocale MLocalePrivate::createQLocale(MLocale::Category category) const
{
    // This function is mainly used to create a QLocale which is then passed to
    // QLocale::setDefault(...) to get support for localized numbers
    // in translations via %Ln, %L1, %L2, ... .
    Q_Q(const MLocale);
    QString language = q->categoryLanguage(category);
    QString country = q->categoryCountry(category);
    QString categoryName = q->categoryName(category);
#ifdef HAVE_ICU
    QString numberOption = MIcuConversions::parseOption(categoryName, "numbers");
#else
    QString numberOption;
#endif

    switch(category) {
    case(MLocale::MLcNumeric):
    case(MLocale::MLcTime):
    case(MLocale::MLcMonetary):
        if(language == "ar" || language == "fa") {
            if(numberOption == "latn") {
                // We have no way to disable use of Eastern Arabic digits
                // in QLocale. Therefore, we change the locale to US English
                // if Latin numbers are requested, this produces reasonably
                // good results:
                language = QLatin1String("en");
                country = QLatin1String("US");
            }
            else if(country == "TN" || country == "MA" || country == "DZ") {
                // for TN (Tunisia), MA (Morocco), and DZ (Algeria),
                // Qt always formats with Western digits (because that
                // is the default in CLDR for these countries, for the
                // same reason libicu formats with Western digits by
                // default for these countries). But we want Arabic digits
                // by default, unless they are explicitely disabled
                // by an option like “ar_TN@numbers=latn” (this case is handled
                // above). So we switch the country to EG (Egypt) because
                // the numeric formats for Egypt are similar to those for
                // the above 3 countries except that Qt uses Eastern Arabic
                // digits for Egypt:
                country = "EG";
            }
        }
        break;
    default:
        break;
    }
    QLocale qlocale(language + '_' + country);
    return qlocale;
}

// sets category to specific locale
void MLocalePrivate::setCategoryLocale(MLocale *mlocale,
        MLocale::Category category,
        const QString &localeName)
{
    Q_UNUSED(mlocale);

    if (category == MLocale::MLcMessages) {
        _messageLocale = localeName;
    } else if (category == MLocale::MLcTime) {
        _calendarLocale = localeName;
#ifdef HAVE_ICU
        // recreate the number formatter
        delete _numberFormatLcTime;
        QString categoryNameTime =
            fixCategoryNameForNumbers(categoryName(MLocale::MLcTime));
        icu::Locale timeLocale = icu::Locale(qPrintable(categoryNameTime));
        UErrorCode status = U_ZERO_ERROR;
        _numberFormatLcTime = icu::NumberFormat::createInstance(timeLocale, status);
        if (!U_SUCCESS(status)) {
            mDebug("MLocalePrivate") << "Unable to create number format for LcTime" << u_errorName(status);
            _valid = false;
        }
#endif
    } else if (category == MLocale::MLcNumeric) {
        _numericLocale = localeName;
#ifdef HAVE_ICU
        // recreate the number formatters
        delete _numberFormat;
        QString categoryNameNumeric =
            fixCategoryNameForNumbers(categoryName(MLocale::MLcNumeric));
        icu::Locale numericLocale = icu::Locale(qPrintable(categoryNameNumeric));
        UErrorCode status = U_ZERO_ERROR;
        _numberFormat = icu::NumberFormat::createInstance(numericLocale, status);
        if (!U_SUCCESS(status)) {
            mDebug("MLocalePrivate") << "Unable to create number format for LcNumeric" << u_errorName(status);
            _valid = false;
        }
        delete _numberFormatLcTime;
        QString categoryNameTime =
            fixCategoryNameForNumbers(categoryName(MLocale::MLcTime));
        icu::Locale timeLocale = icu::Locale(qPrintable(categoryNameTime));
        status = U_ZERO_ERROR;
        _numberFormatLcTime = icu::NumberFormat::createInstance(timeLocale, status);
        if (!U_SUCCESS(status)) {
            mDebug("MLocalePrivate") << "Unable to create number format for LcTime" << u_errorName(status);
            _valid = false;
        }
#endif
    } else if (category == MLocale::MLcCollate) {
        _collationLocale = localeName;
    } else if (category == MLocale::MLcMonetary) {
        _monetaryLocale = localeName;
    } else if (category == MLocale::MLcName) {
        _nameLocale = localeName;
    } else if (category == MLocale::MLcTelephone) {
        _telephoneLocale = localeName;
        // here we set the phone number grouping depending on the
        // setting in the gconf key
        if ( _telephoneLocale.startsWith( QLatin1String( "en_US" ) ) ) {
            _phoneNumberGrouping = MLocale::NorthAmericanPhoneNumberGrouping;
        } else {
            _phoneNumberGrouping = MLocale::NoPhoneNumberGrouping;
        }
    } else {
        //mDebug("MLocalePrivate") << "unimplemented category change"; // DEBUG
    }
}

bool MLocalePrivate::parseIcuLocaleString(const QString &localeString, QString *language, QString *script, QString *country, QString *variant)
{
    // A ICU locale string looks like this:
    //     aa_Bbbb_CC_DDDDDD@foo=fooval;bar=barval;
    // see also http://userguide.icu-project.org/locale
    // The country part is usually a 2 letter uppercase code
    // as in the above example, but there is the exception
    // es_419, i.e. Spanish in Latin America where the “country code”
    // is “419”.
    QRegularExpression regexp("^([a-z]{2,3})(?:_([A-Z][a-z]{3,3}))?(?:_([A-Z]{2,2}|419))?(?:_{1,2}([A-Z][A-Z_]*))?(?:@.*)?$");
    QRegularExpressionMatch match = regexp.match(localeString);

    if (match.hasMatch() && match.capturedTexts().size() == 5) {
        *language = match.captured(1);
        *script = match.captured(2);
        *country = match.captured(3);
        *variant = match.captured(4);
        return true;
    } else {
        *language = "";
        *script = "";
        *country = "";
        *variant= "";
        return false;
    }
}

QString MLocalePrivate::parseLanguage(const QString &localeString)
{
    QString language, script, country, variant;
    parseIcuLocaleString(localeString, &language, &script, &country, &variant);
    return language;
}

QString MLocalePrivate::parseCountry(const QString &localeString)
{
    QString language, script, country, variant;
    parseIcuLocaleString(localeString, &language, &script, &country, &variant);
    return country;
}

QString MLocalePrivate::parseScript(const QString &localeString)
{
    QString language, script, country, variant;
    parseIcuLocaleString(localeString, &language, &script, &country, &variant);
    return script;
}

QString MLocalePrivate::parseVariant(const QString &localeString)
{
    QString language, script, country, variant;
    parseIcuLocaleString(localeString, &language, &script, &country, &variant);
    return variant;
}

QString MLocalePrivate::removeAccents(const QString &str)
{
    QString result;
    for(int i = 0; i < str.size(); ++i) {
        QString decomposition = str[i].decomposition();
        if(decomposition == "")
            result += str[i];
        else
            for(int j = 0; j < decomposition.size(); ++j)
                if(!decomposition[j].isMark())
                    result += decomposition[j];
    }
    return result;
}

//////////////////////////////////
///// MLocale class implementation


// icu handles this string as synonym for posix locale behaviour
namespace
{
    const char *const PosixStr = "en_US_POSIX";
}

// Converts POSIX style locale code to Nokia (ICU) style without variant
// eg. snd_AF.UTF-8@Arab (POSIX) to snd_Arab_AF (Nokia)
//
// The syntax of the locale string in the POSIX environment variables
// related to locale is:
//
//    [language[_territory][.codeset][@modifier]]
//
// (see: http://www.opengroup.org/onlinepubs/000095399/basedefs/xbd_chap08.html)
//
// language is usually lower case in Linux but according to the above specification
// it may start with uppercase as well (i.e. LANG=Fr_FR is allowed).
//
static QString
cleanLanguageCountryPosix(QString &localeString)
{
    // we do not need the encoding and therefore use non-capturing
    // parentheses for the encoding part here.
    // The country part is usually a 2 letter uppercase code
    // as in the above example, but there is the exception
    // es_419, i.e. Spanish in Latin America where the “country code”
    // is “419”. es_419 isn’t really a valid value for LANG, but for consistency
    // let’s make this behave the same way as the icu locale names work for es_419,
    // we only use LANG as a fallback to specify a locale when gconf isn’t available
    // or doesn’t work.
    QRegularExpression regexp("([a-z]{2,3})(_([A-Z]{2,2}|419))?(?:.(?:[a-zA-Z0-9-]+))?(@([A-Z][a-z]+))?");
    QRegularExpressionMatch match = regexp.match(localeString);

    if (match.hasMatch()
            && match.capturedTexts().size() == 6) { // size of regexp pattern above
        QStringList strings;
        strings << match.captured(1); // language

        // POSIX locale modifier, interpreted as script
        if (!match.captured(5).isEmpty()) {
            strings << match.captured(5);
        }

        if (!match.captured(3).isEmpty()) {
            strings << match.captured(3); // country
        }

        // we don't need variant
        return strings.join("_");
    } else {
        //Malformed locale code
        return QString(PosixStr);
    }
}

void MLocale::setConfigItemFactory( const MLocaleAbstractConfigItemFactory* factory )
{
  if ( g_pConfigItemFactory )
    {
      delete g_pConfigItemFactory;
    }

  g_pConfigItemFactory = factory;
}

const MLocaleAbstractConfigItemFactory * MLocale::configItemFactory()
{
    if ( ! g_pConfigItemFactory )
    {
        g_pConfigItemFactory = new MLocaleNullConfigItemFactory;
    }

    return g_pConfigItemFactory;
}

MLocale *
MLocale::createSystemMLocale()
{
    QString language;
    QString lcTime;
    QString lcTimeFormat24h;
    QString lcCollate;
    QString lcNumeric;
    QString lcMonetary;
    QString lcTelephone;

    {
        const MLocaleAbstractConfigItemFactory *factory = MLocale::configItemFactory();

        MLocaleAbstractConfigItem *pCurrentLanguage        = factory->createConfigItem( SettingsLanguage );
        MLocaleAbstractConfigItem *pCurrentLcTime          = factory->createConfigItem( SettingsLcTime );
        MLocaleAbstractConfigItem *pCurrentLcTimeFormat24h = factory->createConfigItem( SettingsLcTimeFormat24h );
        MLocaleAbstractConfigItem *pCurrentLcCollate       = factory->createConfigItem( SettingsLcCollate );
        MLocaleAbstractConfigItem *pCurrentLcNumeric       = factory->createConfigItem( SettingsLcNumeric );
        MLocaleAbstractConfigItem *pCurrentLcMonetary      = factory->createConfigItem( SettingsLcMonetary );
        MLocaleAbstractConfigItem *pCurrentLcTelephone     = factory->createConfigItem( SettingsLcTelephone );

        language        = pCurrentLanguage->value();
        lcTime          = pCurrentLcTime->value();
        lcTimeFormat24h = pCurrentLcTimeFormat24h->value();
        lcCollate       = pCurrentLcCollate->value();
        lcNumeric       = pCurrentLcNumeric->value();
        lcMonetary      = pCurrentLcMonetary->value();
        lcTelephone     = pCurrentLcTelephone->value();

        delete pCurrentLanguage;
        delete pCurrentLcTime;
        delete pCurrentLcTimeFormat24h;
        delete pCurrentLcCollate;
        delete pCurrentLcNumeric;
        delete pCurrentLcMonetary;
        delete pCurrentLcTelephone;
    }

    MLocale *systemLocale;

    if (language.isEmpty()) {
        QString locale = qgetenv("LANG");
        language = cleanLanguageCountryPosix(locale);

        if (language.isEmpty()) {
            language = PosixStr;
            lcTime = PosixStr;
            lcTimeFormat24h = "12";
            lcCollate = PosixStr;
            lcNumeric = PosixStr;
            lcMonetary = PosixStr;
            // no default for lcTelephone
        }

        // No need to set the category according to env here
        systemLocale = new MLocale(language);
    } else {
        // Empty country codes cause problems in some applications.
        // Try to add the “right” country when reading the gconf
        // keys.  But the gconf key /meegotouch/i18n/langauge is often
        // only set to a language without country.  Try to add a the
        // “right” country if it is missing.  For example “zh”
        // means simplified Chinese in the Nokia translations,
        // therefore it is OK to change this to “zh_CN”. “ar” is
        // used for all variants of Arabic, change this to “ar_EG”,
        // etc. ...
        if(gconfLanguageMap.isEmpty()) {
            gconfLanguageMap["ar"] = "ar_EG";
            gconfLanguageMap["cs"] = "cs_CZ";
            gconfLanguageMap["da"] = "da_DK";
            gconfLanguageMap["de"] = "de_DE";
            gconfLanguageMap["en"] = "en_GB";
            gconfLanguageMap["es"] = "es_ES";
            // “es_419” is used for Latin American Spanish
            // translations, but some applications have problems with
            // a country code like “419”, we cannot easily replace
            // it with “es_MX” though because this breaks loading of
            // the Latin American Spanisch translations.
            //
            // gconfLanguageMap["es_419"] = "es_MX";
            gconfLanguageMap["fi"] = "fi_FI";
            gconfLanguageMap["fr"] = "fr_FR";
            gconfLanguageMap["hu"] = "hu_HU";
            gconfLanguageMap["id"] = "id_ID";
            gconfLanguageMap["it"] = "it_IT";
            gconfLanguageMap["ms"] = "ms_MY";
            gconfLanguageMap["nl"] = "nl_NL";
            gconfLanguageMap["no"] = "no_NO";
            gconfLanguageMap["pl"] = "pl_PL";
            gconfLanguageMap["pt"] = "pt_PT";
            gconfLanguageMap["ro"] = "ro_RO";
            gconfLanguageMap["ru"] = "ru_RU";
            gconfLanguageMap["sk"] = "sk_SK";
            gconfLanguageMap["sv"] = "sv_SE";
            gconfLanguageMap["th"] = "th_TH";
            gconfLanguageMap["tr"] = "tr_TR";
            gconfLanguageMap["uk"] = "uk_UA";
            gconfLanguageMap["zh"] = "zh_CN";
        }

        if (gconfLanguageMap.contains(language))
            language = gconfLanguageMap.value(language);
        systemLocale = new MLocale(language);
    }

    if (!lcTime.isEmpty())
        systemLocale->setCategoryLocale(MLocale::MLcTime, lcTime);
    if (lcTimeFormat24h == "24")
        systemLocale->setTimeFormat24h(MLocale::TwentyFourHourTimeFormat24h);
    else if (lcTimeFormat24h == "12")
        systemLocale->setTimeFormat24h(MLocale::TwelveHourTimeFormat24h);
    else
        systemLocale->setTimeFormat24h(MLocale::LocaleDefaultTimeFormat24h);
    if (!lcCollate.isEmpty())
        systemLocale->setCategoryLocale(MLocale::MLcCollate, lcCollate);
    if (!lcNumeric.isEmpty())
        systemLocale->setCategoryLocale(MLocale::MLcNumeric, lcNumeric);
    if (!lcMonetary.isEmpty())
        systemLocale->setCategoryLocale(MLocale::MLcMonetary, lcMonetary);
    if (!lcTelephone.isEmpty())
        systemLocale->setCategoryLocale(MLocale::MLcTelephone, lcTelephone);

    return systemLocale;
}

MLocale MLocale::createCLocale()
{
    return MLocale(PosixStr);
}

void
MLocale::connectSettings()
{
    Q_D(MLocale);

    const MLocaleAbstractConfigItemFactory *factory = MLocale::configItemFactory();

    if ( !d->pCurrentLanguage )
        d->pCurrentLanguage        = factory->createConfigItem( SettingsLanguage );
    if ( !d->pCurrentLcTime )
        d->pCurrentLcTime          = factory->createConfigItem( SettingsLcTime );
    if ( !d->pCurrentLcTimeFormat24h )
        d->pCurrentLcTimeFormat24h = factory->createConfigItem( SettingsLcTimeFormat24h );
    if ( !d->pCurrentLcCollate )
        d->pCurrentLcCollate       = factory->createConfigItem( SettingsLcCollate );
    if ( !d->pCurrentLcNumeric )
        d->pCurrentLcNumeric       = factory->createConfigItem( SettingsLcNumeric );
    if ( !d->pCurrentLcMonetary )
        d->pCurrentLcMonetary      = factory->createConfigItem( SettingsLcMonetary );
    if ( !d->pCurrentLcTelephone )
        d->pCurrentLcTelephone     = factory->createConfigItem( SettingsLcTelephone );

    QObject::connect(d->pCurrentLanguage, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
    QObject::connect(d->pCurrentLcTime, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
    QObject::connect(d->pCurrentLcTimeFormat24h, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
    QObject::connect(d->pCurrentLcCollate, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
    QObject::connect(d->pCurrentLcNumeric, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
    QObject::connect(d->pCurrentLcMonetary, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
    QObject::connect(d->pCurrentLcTelephone, SIGNAL(valueChanged(QString)),
                     this, SLOT(refreshSettings()));
}

void
MLocale::disconnectSettings()
{
    Q_D(MLocale);

    QObject::disconnect(d->pCurrentLanguage, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
    QObject::disconnect(d->pCurrentLcTime, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
    QObject::disconnect(d->pCurrentLcTimeFormat24h, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
    QObject::disconnect(d->pCurrentLcCollate, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
    QObject::disconnect(d->pCurrentLcNumeric, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
    QObject::disconnect(d->pCurrentLcMonetary, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
    QObject::disconnect(d->pCurrentLcTelephone, SIGNAL(valueChanged(QString)),
			this, SLOT(refreshSettings()));
}

///// Constructors  /////

//! Constructs a MLocale with data copied from default Locale
MLocale::MLocale(QObject *parent)
    : QObject(parent),
      d_ptr(new MLocalePrivate)
{
    Q_D(MLocale);
    d->q_ptr = this;
    // copy the system default
    MLocale &defaultLocale = getDefault();
    *this = defaultLocale;
}

MLocale::MLocale(const QString &localeName, QObject *parent)
    : QObject(parent),
      d_ptr(new MLocalePrivate)
{
    Q_D(MLocale);
    d->q_ptr = this;
    d->_defaultLocale = qPrintable(localeName);
    // If a system default locale exists already copy the translation
    // catalogs and reload them for this locale:
    if (s_systemDefault)
        copyCatalogsFrom(*s_systemDefault);

#ifdef HAVE_ICU
    // we cache the number formatter for better performance
    QString categoryNameNumeric =
        d->fixCategoryNameForNumbers(categoryName(MLocale::MLcNumeric));
    UErrorCode status = U_ZERO_ERROR;
    d->_numberFormat =
        icu::NumberFormat::createInstance(icu::Locale(qPrintable(categoryNameNumeric)),
                                          status);
    if (!U_SUCCESS(status)) {
        qWarning() << "NumberFormat creating for LcNumeric failed:" << u_errorName(status);
        d->_valid = false;
    }
    QString categoryNameTime =
        d->fixCategoryNameForNumbers(categoryName(MLocale::MLcTime));
    status = U_ZERO_ERROR;
    d->_numberFormatLcTime =
        icu::NumberFormat::createInstance(icu::Locale(qPrintable(categoryNameTime)),
                                          status);
    if (!U_SUCCESS(status)) {
        qWarning() << "NumberFormat creating for LcTime failed:" << u_errorName(status);
        d->_valid = false;
    }
#endif
}


//! Copy constructor
MLocale::MLocale(const MLocale &other, QObject *parent)
    : QObject(parent),
      d_ptr(new MLocalePrivate(*other.d_ptr))
{
    Q_D(MLocale);
    d->q_ptr = this;
}


//! Destructor
MLocale::~MLocale()
{
    // do not delete the d_ptr of s_systemDefault unless we are s_systemDefault
    if (d_ptr) {
        if (s_systemDefault == 0) {
            delete d_ptr;
        } else if (d_ptr != s_systemDefault->d_ptr) {
            delete d_ptr;
        } else if (this == s_systemDefault) {
            delete d_ptr;
            s_systemDefault = 0;
        }
    }
}

//! Assignment operator
MLocale &MLocale::operator=(const MLocale &other)
{
    if (this == &other) {
        return *this;
    }

    *d_ptr = *other.d_ptr;

    return *this;
}



///////////////////
//// normal methods

// mutex to guard default locale
static QMutex defaultLocaleMutex;

// The static default locale
MLocale *MLocale::s_systemDefault = 0;

static Qt::LayoutDirection _defaultLayoutDirection = Qt::LeftToRight;

struct MStaticLocaleDestroyer {
    ~MStaticLocaleDestroyer() {
        delete MLocale::s_systemDefault; MLocale::s_systemDefault = 0;
    }
};
static MStaticLocaleDestroyer staticLocaleDestroyer;

static void setApplicationLayoutDirection(Qt::LayoutDirection layoutDirection)
{
    if (QCoreApplication *app = QCoreApplication::instance()) {
        int layoutDirProperty = app->metaObject()->indexOfProperty("layoutDirection");
        if (layoutDirProperty != -1) {
            app->metaObject()->property(layoutDirProperty).write(app, layoutDirection);
        }
    }
}

void MLocale::setDefault(const MLocale &locale)
{
    defaultLocaleMutex.lock();

    if (s_systemDefault == 0) {
        s_systemDefault = new MLocale(locale);
    } else if (&locale == s_systemDefault || locale.d_ptr == s_systemDefault->d_ptr) {
        defaultLocaleMutex.unlock();
        return;
    } else {
        s_systemDefault->disconnectSettings();
        disconnect(s_systemDefault, SIGNAL(settingsChanged()), 0, 0 );

        // remove the previous tr translations
        (s_systemDefault->d_ptr)->removeTrFromQCoreApp();
        *s_systemDefault = locale;
    }
    defaultLocaleMutex.unlock();
    // load special translations to make QApplication detect the
    // correct direction (see qapplication.cpp in the Qt source
    // code). If this is not done, the QEvent::LanguageChange events
    // triggered by QCoreApplication::removeTranslator() and
    // QCoreApplication::installTranslator() which are called by
    // removeTrFromQCoreApp() and insertTrToQCoreApp() may set a wrong
    // direction because these QEvent::LanguageChange may be processed
    // later than the QEvent::ApplicationLayoutDirectionChange event
    // triggered by
    // qApp->setLayoutDirection(s_systemDefault->textDirection());
    (s_systemDefault->d_ptr)->insertDirectionTrToQCoreApp();

    // sends QEvent::LanguageChange to qApp:
    (s_systemDefault->d_ptr)->insertTrToQCoreApp();
    // Setting the default QLocale is needed to get localized number
    // support in translations via %Ln, %L1, %L2, ...:
    QLocale::setDefault((s_systemDefault->d_ptr)->createQLocale(MLcNumeric));
    // sends QEvent::ApplicationLayoutDirectionChange to qApp:
    setApplicationLayoutDirection(s_systemDefault->textDirection());
#ifdef HAVE_ICU
    _defaultLayoutDirection = MIcuConversions::parseLayoutDirectionOption(s_systemDefault->name());
#else
    _defaultLayoutDirection = Qt::LeftToRight;
#endif

    QCoreApplication *qapp = QCoreApplication::instance();
    if ( qapp && qapp->metaObject() && qapp->metaObject()->className() == QString( "MApplication" ) )
    {
        QObject::connect(s_systemDefault, SIGNAL(settingsChanged()),
                         qapp, SIGNAL(localeSettingsChanged()));
    }

    QObject::connect(s_systemDefault, SIGNAL(settingsChanged()),
                     s_systemDefault, SIGNAL(localeSettingsChanged()));

    emit s_systemDefault->settingsChanged();
    s_systemDefault->connectSettings();
}

MLocale &MLocale::getDefault()
{
    if (s_systemDefault == 0) {
        // no default created, do it now

        // avoid race condition for multiple getDefaults()
        defaultLocaleMutex.lock();

        if (s_systemDefault == 0) {
            // we won the race
            s_systemDefault = createSystemMLocale();
            s_systemDefault->connectSettings();
        }

        defaultLocaleMutex.unlock();
    }

    return *s_systemDefault;
}

bool MLocale::isValid() const
{
    Q_D(const MLocale);
    return d->_valid;
}

void MLocale::setCategoryLocale(Category category, const QString &localeName)
{
    Q_D(MLocale);
    d->setCategoryLocale(this, category, localeName);

    d->dropCaches();
}

void MLocale::setCollation(Collation collation)
{
    Q_D(MLocale);
    d->dropCaches();
#ifdef HAVE_ICU
    if(!d->_collationLocale.isEmpty())
        d->_collationLocale =
            MIcuConversions::setCollationOption(d->_collationLocale, collation);
    else
        d->_defaultLocale =
            MIcuConversions::setCollationOption(d->_defaultLocale, collation);
#else
    Q_UNUSED(collation);
#endif
}

MLocale::Collation MLocale::collation() const
{
#ifdef HAVE_ICU
    return MIcuConversions::parseCollationOption(categoryName(MLcCollate));
#else
    return MLocale::DefaultCollation;
#endif
}

void MLocale::setCalendarType(CalendarType calendarType)
{
    Q_D(MLocale);
    d->dropCaches();
#ifdef HAVE_ICU
    if(!d->_calendarLocale.isEmpty())
        d->_calendarLocale =
            MIcuConversions::setCalendarOption(d->_calendarLocale, calendarType);
    else
        d->_defaultLocale =
            MIcuConversions::setCalendarOption(d->_defaultLocale, calendarType);
#else
    Q_UNUSED(calendarType);
#endif
}

MLocale::CalendarType MLocale::calendarType() const
{
#ifdef HAVE_ICU
    return MIcuConversions::parseCalendarOption(categoryName(MLcTime));
#else
    return MLocale::DefaultCalendar;
#endif
}

void MLocale::setTimeFormat24h(TimeFormat24h timeFormat24h)
{
    Q_D(MLocale);
    d->_timeFormat24h = timeFormat24h;

    d->dropCaches();
}

MLocale::TimeFormat24h MLocale::timeFormat24h() const
{
    Q_D(const MLocale);
    return d->_timeFormat24h;
}

#ifdef HAVE_ICU
MLocale::TimeFormat24h MLocale::defaultTimeFormat24h() const
{
    Q_D(const MLocale);
    QString defaultTimeShortFormat
        = d->icuFormatString(MLocale::DateNone, MLocale::TimeShort,
                             calendarType(),
                             MLocale::LocaleDefaultTimeFormat24h);
    if (d->isTwelveHours(defaultTimeShortFormat))
        return MLocale::TwelveHourTimeFormat24h;
    else
        return MLocale::TwentyFourHourTimeFormat24h;
}
#endif

#ifdef HAVE_ICU
MCollator MLocale::collator() const
{
    return MCollator(*this);
}
#endif

QString MLocale::toLower(const QString &string) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    // we don’t have MLcCtype, MLcMessages comes closest
    return MIcuConversions::unicodeStringToQString(
        MIcuConversions::qStringToUnicodeString(string).toLower(
            d->getCategoryLocale(MLcMessages)));
#else
    // QString::toLower() is *not* locale aware, this is only
    // a “better than nothing” fallback.
    return string.toLower();
#endif
}

QString MLocale::toUpper(const QString &string) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    // we don’t have MLcCtype, MLcMessages comes closest
    return MIcuConversions::unicodeStringToQString(
        MIcuConversions::qStringToUnicodeString(string).toUpper(
            d->getCategoryLocale(MLcMessages)));
#else
    // QString::toUpper() is *not* locale aware, this is only
    // a “better than nothing” fallback.
    return string.toUpper();
#endif
}

QString MLocale::language() const
{
    return MLocalePrivate::parseLanguage(name());
}

QString MLocale::country() const
{
    return MLocalePrivate::parseCountry(name());
}

QString MLocale::script() const
{
    return MLocalePrivate::parseScript(name());
}

QString MLocale::variant() const
{
    return MLocalePrivate::parseVariant(name());
}

QString MLocale::name() const
{
    Q_D(const MLocale);
    return d->_defaultLocale;
}

QString MLocale::categoryLanguage(Category category) const
{
    QString wholeName = categoryName(category);
    return MLocalePrivate::parseLanguage(wholeName);
}

QString MLocale::categoryCountry(Category category) const
{
    QString wholeName = categoryName(category);
    return MLocalePrivate::parseCountry(wholeName);
}

QString MLocale::categoryScript(Category category) const
{
    QString wholeName = categoryName(category);
    return MLocalePrivate::parseScript(wholeName);
}

QString MLocale::categoryVariant(Category category) const
{
    QString wholeName = categoryName(category);
    return MLocalePrivate::parseVariant(wholeName);
}

QString MLocale::categoryName(Category category) const
{
    Q_D(const MLocale);
    return d->categoryName(category);
}

QString MLocale::formatNumber(qlonglong i) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    UnicodeString str;
    // This might generate a warning by the Krazy code analyzer,
    // but it allows the code to compile with ICU 4.0
    d->_numberFormat->format(static_cast<int64_t>(i), str); //krazy:exclude=typedefs
    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toString(i);
#endif
}

qlonglong MLocale::toLongLong(const QString &s, bool *ok) const
{
    if (s.length() == 0) {
        if (ok != NULL)
            *ok = false;
        return (int(0));
    }
#ifdef HAVE_ICU
    Q_D(const MLocale);
    QString parseInput = s;
    d->fixParseInputForRTL(&parseInput);
    icu::UnicodeString str = MIcuConversions::qStringToUnicodeString(parseInput);
    icu::Formattable formattable;
    icu::ParsePosition parsePosition;
    qint64 result;
    icu::DecimalFormat *decimalFormat
        = static_cast<icu::DecimalFormat *>(d->_numberFormat);
    if (!decimalFormat->isParseIntegerOnly()) {
        decimalFormat->setParseIntegerOnly(true);
        decimalFormat->parse(str, formattable, parsePosition);
        decimalFormat->setParseIntegerOnly(false);
    }
    else
        decimalFormat->parse(str, formattable, parsePosition);
    if (parsePosition.getIndex() < str.length()) {
        if (ok != NULL)
            *ok = false;
        return (qlonglong(0));
    }
    else {
        UErrorCode status = U_ZERO_ERROR;
        result = formattable.getInt64(status);
        if (!U_SUCCESS(status)) {
            if (ok != NULL)
                *ok = false;
            return (qlonglong(0));
        }
        else {
            if (ok != NULL)
                *ok = true;
            return (qlonglong(result));
        }
    }
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toLongLong(s, ok);
#endif
}

QString MLocale::formatNumber(short i) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    UnicodeString str;
    d->_numberFormat->format(i, str);
    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toString(i);
#endif
}

short MLocale::toShort(const QString &s, bool *ok) const
{
    if (s.length() == 0) {
        if (ok != NULL)
            *ok = false;
        return (int(0));
    }
#ifdef HAVE_ICU
    Q_D(const MLocale);
    QString parseInput = s;
    d->fixParseInputForRTL(&parseInput);
    icu::UnicodeString str = MIcuConversions::qStringToUnicodeString(parseInput);
    icu::Formattable formattable;
    icu::ParsePosition parsePosition;
    qint64 result;
    icu::DecimalFormat *decimalFormat
        = static_cast<icu::DecimalFormat *>(d->_numberFormat);
    if (!decimalFormat->isParseIntegerOnly()) {
        decimalFormat->setParseIntegerOnly(true);
        decimalFormat->parse(str, formattable, parsePosition);
        decimalFormat->setParseIntegerOnly(false);
    }
    else
        decimalFormat->parse(str, formattable, parsePosition);
    if (parsePosition.getIndex() < str.length()) {
        if (ok != NULL)
            *ok = false;
        return (short(0));
    }
    else {
        UErrorCode status = U_ZERO_ERROR;
        result = formattable.getInt64(status);
        if (!U_SUCCESS(status)) {
            if (ok != NULL)
                *ok = false;
            return (short(0));
        }
        else {
            if (result < SHRT_MIN || result > SHRT_MAX) {
                if (ok != NULL)
                    *ok = false;
                return (short(0));
            }
            else {
                if (ok != NULL)
                    *ok = true;
                return (short(result));
            }
        }
    }
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toShort(s, ok);
#endif
}

QString MLocale::formatNumber(int i) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    UnicodeString str;
    d->_numberFormat->format(i, str);
    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toString(i);
#endif
}

int MLocale::toInt(const QString &s, bool *ok) const
{
    if (s.length() == 0) {
        if (ok != NULL)
            *ok = false;
        return (int(0));
    }
#ifdef HAVE_ICU
    Q_D(const MLocale);
    QString parseInput = s;
    d->fixParseInputForRTL(&parseInput);
    icu::UnicodeString str = MIcuConversions::qStringToUnicodeString(parseInput);
    icu::Formattable formattable;
    icu::ParsePosition parsePosition;
    qint64 result;
    icu::DecimalFormat *decimalFormat
        = static_cast<icu::DecimalFormat *>(d->_numberFormat);
    if (!decimalFormat->isParseIntegerOnly()) {
        decimalFormat->setParseIntegerOnly(true);
        decimalFormat->parse(str, formattable, parsePosition);
        decimalFormat->setParseIntegerOnly(false);
    }
    else
        decimalFormat->parse(str, formattable, parsePosition);
    if (parsePosition.getIndex() < str.length()) {
        if (ok != NULL)
            *ok = false;
        return (int(0));
    }
    else {
        UErrorCode status = U_ZERO_ERROR;
        result = formattable.getInt64(status);
        if (!U_SUCCESS(status)) {
            if (ok != NULL)
                *ok = false;
            return (int(0));
        }
        else {
            if (result < INT_MIN || result > INT_MAX) {
                if (ok != NULL)
                    *ok = false;
                return (int(0));
            }
            else {
                if (ok != NULL)
                    *ok = true;
                return (int(result));
            }
        }
    }
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toInt(s, ok);
#endif
}

QString MLocale::formatNumber(double i, int maxPrecision) const
{
    return formatNumber(i, maxPrecision, 0);
}

QString MLocale::formatNumber(double i, int maxPrecision, int minPrecision) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    icu::UnicodeString str;
    icu::FieldPosition pos;

    if (maxPrecision < 0) {
        d->_numberFormat->format(i, str, pos);
    } else {
        // the cached number formatter isn't sufficient
        QString categoryNameNumeric =
            d->fixCategoryNameForNumbers(categoryName(MLocale::MLcNumeric));
        UErrorCode status = U_ZERO_ERROR;
        icu::NumberFormat *nf;
        nf = icu::NumberFormat::createInstance(icu::Locale(qPrintable(categoryNameNumeric)),
                                               status);
        if (!U_SUCCESS(status)) {
            qWarning() << "NumberFormat creating failed" << u_errorName(status);
            return QString(); // "null" string
        }

        nf->setMaximumFractionDigits(maxPrecision);
        nf->setMinimumFractionDigits(qBound(0, minPrecision, maxPrecision));
        nf->format(i, str);
        delete nf;
    }

    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
#else
    Q_D(const MLocale);
    Q_UNUSED(minPrecision);
    return d->createQLocale(MLcNumeric).toString(i, 'g', maxPrecision);
#endif
}

double MLocale::toDouble(const QString &s, bool *ok) const
{
    if (s.length() == 0) {
        if (ok != NULL)
            *ok = false;
        return (int(0));
    }
#ifdef HAVE_ICU
    Q_D(const MLocale);
    icu::DecimalFormat *decimalFormat
        = static_cast<icu::DecimalFormat *>(d->_numberFormat);
    const icu::DecimalFormatSymbols *decimalFormatSymbols
        = decimalFormat->getDecimalFormatSymbols();
    QString exponentialSymbol
        = MIcuConversions::unicodeStringToQString(
            decimalFormatSymbols->getSymbol(DecimalFormatSymbols::kExponentialSymbol));
    QString parseInput = s;
    d->fixParseInputForRTL(&parseInput);
    // accept “e” or “E” always as exponential symbols, even if the
    // locale uses something completely different:
    parseInput.replace(QChar('e'), exponentialSymbol, Qt::CaseInsensitive);
    // parse the exponential symbol in the input case insensitive:
    parseInput.replace(exponentialSymbol, exponentialSymbol, Qt::CaseInsensitive);
    icu::UnicodeString str = MIcuConversions::qStringToUnicodeString(parseInput);
    icu::Formattable formattable;
    icu::ParsePosition parsePosition;
    double result;
    if (decimalFormat->isParseIntegerOnly()) {
        decimalFormat->setParseIntegerOnly(false);
        decimalFormat->parse(str, formattable, parsePosition);
        decimalFormat->setParseIntegerOnly(true);
    }
    else
        decimalFormat->parse(str, formattable, parsePosition);
    if (parsePosition.getIndex() < str.length()) {
        if (ok != NULL)
            *ok = false;
        return (double(0.0));
    }
    else {
        UErrorCode status = U_ZERO_ERROR;
        result = formattable.getDouble(status);
        if (!U_SUCCESS(status)) {
            if (ok != NULL)
                *ok = false;
            return (double(0.0));
        }
        else {
            if (ok != NULL)
                *ok = true;
            return (result);
        }
    }
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toDouble(s, ok);
#endif
}

QString MLocale::formatNumber(float i) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    icu::UnicodeString str;
    icu::FieldPosition pos;
    d->_numberFormat->format(i, str, pos);
    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toString(i, 'g');
#endif
}

float MLocale::toFloat(const QString &s, bool *ok) const
{
    if (s.length() == 0) {
        if (ok != NULL)
            *ok = false;
        return (int(0));
    }
#ifdef HAVE_ICU
    Q_D(const MLocale);
    icu::DecimalFormat *decimalFormat
        = static_cast<icu::DecimalFormat *>(d->_numberFormat);
    const icu::DecimalFormatSymbols *decimalFormatSymbols
        = decimalFormat->getDecimalFormatSymbols();
    QString exponentialSymbol
        = MIcuConversions::unicodeStringToQString(
            decimalFormatSymbols->getSymbol(DecimalFormatSymbols::kExponentialSymbol));
    QString parseInput = s;
    d->fixParseInputForRTL(&parseInput);
    // accept “e” or “E” always as exponential symbols, even if the
    // locale uses something completely different:
    parseInput.replace(QChar('e'), exponentialSymbol, Qt::CaseInsensitive);
    // parse the exponential symbol in the input case insensitive:
    parseInput.replace(exponentialSymbol, exponentialSymbol, Qt::CaseInsensitive);
    icu::UnicodeString str = MIcuConversions::qStringToUnicodeString(parseInput);
    icu::Formattable formattable;
    icu::ParsePosition parsePosition;
    double result;
    if (decimalFormat->isParseIntegerOnly()) {
        decimalFormat->setParseIntegerOnly(false);
        decimalFormat->parse(str, formattable, parsePosition);
        decimalFormat->setParseIntegerOnly(true);
    }
    else
        decimalFormat->parse(str, formattable, parsePosition);
    if (parsePosition.getIndex() < str.length()) {
        if (ok != NULL)
            *ok = false;
        return (float(0.0));
    }
    else {
        UErrorCode status = U_ZERO_ERROR;
        result = formattable.getDouble(status);
        if (!U_SUCCESS(status)) {
            if (ok != NULL)
                *ok = false;
            return (float(0.0));
        }
        else {
            if (qAbs(result) > FLT_MAX) {
                if (ok != NULL)
                    *ok = false;
                return (float(0.0));
            }
            else {
                if (ok != NULL)
                    *ok = true;
                return (float(result));
            }
        }
    }
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).toFloat(s, ok);
#endif
}

#ifdef HAVE_ICU
void MLocalePrivate::removeDirectionalFormattingCodes(QString *str) const
{
    str->remove(QChar(0x200F)); // RIGHT-TO-LEFT MARK
    str->remove(QChar(0x200E)); // LEFT-TO-RIGHT MARK
    str->remove(QChar(0x202D)); // LEFT-TO-RIGHT OVERRIDE
    str->remove(QChar(0x202E)); // RIGHT-TO-LEFT OVERRIDE
    str->remove(QChar(0x202A)); // LEFT-TO-RIGHT EMBEDDING
    str->remove(QChar(0x202B)); // RIGHT-TO-LEFT EMBEDDING
    str->remove(QChar(0x202C)); // POP DIRECTIONAL FORMATTING
}
#endif


#ifdef HAVE_ICU
void MLocalePrivate::swapPostAndPrefixOfFormattedNumber(QString *formattedNumber) const
{
    QString newPostfix;
    QString newPrefix;
    while(!formattedNumber->isEmpty()
          && formattedNumber->at(0).direction() != QChar::DirEN
          && formattedNumber->at(0).direction() != QChar::DirAN) {
        if(formattedNumber->at(0).isLetter() || formattedNumber->at(0).isPunct()) {
            int i = 0;
            while (i < newPostfix.size() && (newPostfix.at(i).isLetter() || newPostfix.at(i).isPunct()))
                ++i;
            newPostfix.insert(i, formattedNumber->at(0));
        }
        else
            newPostfix.prepend(formattedNumber->at(0));
        formattedNumber->remove(0,1);
    }
    while(!formattedNumber->isEmpty()
          && formattedNumber->right(1).at(0).direction() != QChar::DirEN
          && formattedNumber->right(1).at(0).direction() != QChar::DirAN) {
        if(formattedNumber->right(1).at(0).isLetter() || formattedNumber->right(1).at(0).isPunct()) {
            int i = newPrefix.size();
            while (i > 0 && (newPrefix.at(i-1).isLetter() || newPrefix.at(i-1).isPunct()))
                --i;
            newPrefix.insert(i, formattedNumber->right(1).at(0));
        }
        else
            newPrefix.append(formattedNumber->right(1).at(0));
        formattedNumber->remove(formattedNumber->size()-1,1);
    }
    formattedNumber->prepend(newPrefix);
    formattedNumber->append(newPostfix);
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::fixFormattedNumberForRTL(QString *formattedNumber) const
{
    Q_Q(const MLocale);
    QString categoryNameNumeric = categoryName(MLocale::MLcNumeric);
    if(categoryNameNumeric.startsWith(QLatin1String("ar"))
       || categoryNameNumeric.startsWith(QLatin1String("fa"))) {
        // remove formatting codes already found in the format, there
        // should not be any but better make sure
        // (actually some of the Arabic currency symbols have RLM markers in the icu
        // data ...).
        removeDirectionalFormattingCodes(formattedNumber);
        if (formattedNumber->contains(QRegularExpression(QString::fromUtf8("[٠١٢٣٤٥٦٧٨٩۰۱۲۳۴۵۶۷۸۹]")))) {
            swapPostAndPrefixOfFormattedNumber(formattedNumber);
        }
    }
    if(formattedNumber->at(0).direction() == QChar::DirAL) {
        // there is an Arabic currency symbol at the beginning, add markup
        // like this: <RLE>currency symbol with trailing spaces<PDF><LRE>rest of number<PDF>
        int i = 0;
        while (i < formattedNumber->size()
               && (formattedNumber->at(i).isLetter()
                   || formattedNumber->at(i).isPunct()
                   || formattedNumber->at(i).isSpace()))
            ++i;
        formattedNumber->insert(i, QChar(0x202A)); // LEFT-TO-RIGHT EMBEDDING
        formattedNumber->insert(i, QChar(0x202C)); // POP DIRECTIONAL FORMATTING
        formattedNumber->prepend(QChar(0x202B));   // RIGHT-TO-LEFT EMBEDDING
        formattedNumber->append(QChar(0x202C));    // POP DIRECTIONAL FORMATTING
    } else if(MLocale::directionForText(*formattedNumber) == Qt::RightToLeft) {
        // there is an Arabic currency symbol at the end, add markup like this:
        // <LRE>rest of number<PDF><RLE>leading spaces and currency symbol<PDF>
        int i = formattedNumber->size();
        while (i > 0
               && (formattedNumber->at(i-1).isLetter()
                   || formattedNumber->at(i-1).isPunct()
                   || formattedNumber->at(i-1).isSpace()))
            --i;
        formattedNumber->insert(i, QChar(0x202B)); // RIGHT-TO-LEFT EMBEDDING
        formattedNumber->insert(i, QChar(0x202C)); // POP DIRECTIONAL FORMATTING
        formattedNumber->prepend(QChar(0x202A));   // LEFT-TO-RIGHT EMBEDDING
        formattedNumber->append(QChar(0x202C));    // POP DIRECTIONAL FORMATTING
    }
    // see http://comments.gmane.org/gmane.comp.internationalization.bidi/2
    // and consider the bugs:
    // https://projects.maemo.org/bugzilla/show_bug.cgi?id=232757
    // https://projects.maemo.org/bugzilla/show_bug.cgi?id=277180
    //
    // If the user interface (lc_messages) uses a language which uses
    // right-to-left script, wrap the result in LRE...PDF markers to
    // make sure the result is not reordered again depending on
    // context (this assumes that the formats are all edited exactly
    // as they should appear in display order already!):
#if 0 // non-functional
    if(q->localeScripts()[0] != "Arab" && q->localeScripts()[0] != "Hebr")
        return;
    formattedNumber->prepend(QChar(0x202A)); // LEFT-TO-RIGHT EMBEDDING
    formattedNumber->append(QChar(0x202C)); // POP DIRECTIONAL FORMATTING
#endif
    Q_UNUSED(q)
    return;
}
#endif

#ifdef HAVE_ICU
void MLocalePrivate::fixParseInputForRTL(QString *formattedNumber) const
{
    removeDirectionalFormattingCodes(formattedNumber);
    if(formattedNumber->contains(QRegularExpression(QString::fromUtf8("[٠١٢٣٤٥٦٧٨٩۰۱۲۳۴۵۶۷۸۹]")))) {
        swapPostAndPrefixOfFormattedNumber(formattedNumber);
    }
}
#endif

#ifdef HAVE_ICU
QString MLocale::formatPercent(double i, int decimals) const
{
    Q_D(const MLocale);
    QString categoryNameNumeric
        = d->fixCategoryNameForNumbers(categoryName(MLocale::MLcNumeric));
    icu::Locale numericLocale = icu::Locale(qPrintable(categoryNameNumeric));
    UErrorCode status = U_ZERO_ERROR;
    icu::NumberFormat *nf = NumberFormat::createPercentInstance(numericLocale, status);

    if (!U_SUCCESS(status)) {
        qWarning() << "NumberFormat creating failed" << u_errorName(status);
        return QString();
    }

    nf->setMinimumFractionDigits(decimals);
    icu::UnicodeString str;
    nf->format(i, str);
    delete nf;
    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
}
#endif

QString MLocale::formatCurrency(double amount, const QString &currency) const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    QString monetaryCategoryName = d->fixCategoryNameForNumbers(categoryName(MLcMonetary));
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale monetaryLocale = icu::Locale(qPrintable(monetaryCategoryName));
    icu::NumberFormat *nf = icu::NumberFormat::createCurrencyInstance(monetaryLocale, status);

    if (!U_SUCCESS(status)) {
        qWarning() << "icu::NumberFormat::createCurrencyInstance failed with error"
                   << u_errorName(status);
        return QString();
    }

    icu::UnicodeString currencyString = MIcuConversions::qStringToUnicodeString(currency);
    nf->setCurrency(currencyString.getTerminatedBuffer(), status);

    if (!U_SUCCESS(status)) {
        qWarning() << "icu::NumberFormat::setCurrency failed with error"
                   << u_errorName(status);
        delete nf;
        return QString();
    }

    icu::UnicodeString str;
    nf->format(amount, str);
    delete nf;
    QString result = MIcuConversions::unicodeStringToQString(str);
    d->fixFormattedNumberForRTL(&result);
    return result;
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcMonetary).toString(amount) + ' ' + currency;
#endif
}

QString MLocale::formatDateTime(const QDateTime &dateTime, DateType dateType,
                                  TimeType timeType, CalendarType calendarType) const
{
#ifdef HAVE_ICU
    MCalendar calendar(calendarType);
    calendar.setDateTime(dateTime);
    return formatDateTime(calendar, dateType, timeType);
#else
    Q_UNUSED(dateType);
    Q_UNUSED(timeType);
    Q_UNUSED(calendarType);
    Q_D(const MLocale);
    return d->createQLocale(MLcTime).toString(dateTime);
#endif
}

#ifdef HAVE_ICU
QString MLocale::formatDateTime(const MCalendar &mcalendar,
                                  DateType datetype, TimeType timetype) const
{
    Q_D(const MLocale);

    if (datetype == DateNone && timetype == TimeNone)
        return QString("");

    icu::FieldPosition pos;
    icu::UnicodeString resString;
    icu::Calendar *cal = mcalendar.d_ptr->_calendar;

    icu::DateFormat *df = d->createDateFormat(datetype, timetype,
                                              mcalendar.type(),
                                              d->_timeFormat24h);
    if(df)
        df->format(*cal, resString, pos);
    QString result = MIcuConversions::unicodeStringToQString(resString);
    return result;
}
#endif

#ifdef HAVE_ICU
QString MLocale::formatDateTime(const QDateTime &dateTime, CalendarType calendarType) const
{
    return formatDateTime(dateTime, DateLong, TimeLong, calendarType);
}
#endif

#ifdef HAVE_ICU
QString MLocale::formatDateTime(const QDateTime &dateTime,
                                  const QString &formatString) const
{
    Q_D(const MLocale);

    // convert QDateTime to MCalendar and format

    if ( ! d->_pDateTimeCalendar )
    {
        const_cast<MLocalePrivate *>(d)->_pDateTimeCalendar = new MCalendar( *this );
    }

    const_cast<MLocalePrivate *>(d)->_pDateTimeCalendar->setDateTime(dateTime);
    return formatDateTime(*d->_pDateTimeCalendar, formatString);
}
#endif

#ifdef HAVE_ICU
//! creates a string presentation for a QDateTime with specific format string
//! \param dateTime QDateTime to format
//! \param formatString in ICU SimpleDateFormat format
//! \note THIS MAY BE REMOVED FROM PUBLIC API
QString MLocale::formatDateTimeICU(const QDateTime &dateTime,
                                     const QString &formatString) const
{
    Q_D(const MLocale);

    // convert QDateTime to MCalendar and format

    if ( ! d->_pDateTimeCalendar )
    {
        const_cast<MLocalePrivate *>(d)->_pDateTimeCalendar = new MCalendar( *this );
    }

    const_cast<MLocalePrivate *>(d)->_pDateTimeCalendar->setDateTime(dateTime);
    return formatDateTimeICU(*d->_pDateTimeCalendar, formatString);
}
#endif

#ifdef HAVE_ICU
//! Formats the date time with ICU pattern
//! \note THIS MAY BE REMOVED FROM PUBLIC API
QString MLocale::formatDateTimeICU(const MCalendar &mCalendar,
                                     const QString &formatString) const
{
    Q_D(const MLocale);
    QString categoryNameTime = categoryName(MLocale::MLcTime);
    QString categoryNameNumeric = categoryName(MLocale::MLcNumeric);
    QString categoryNameMessages = categoryName(MLocale::MLcMessages);
    QString key = QString("%1_%2_%3_%4_%5")
        .arg(formatString)
        .arg(mCalendar.type())
        .arg(categoryNameTime)
        .arg(categoryNameNumeric)
        .arg(categoryNameMessages);
    categoryNameTime = d->fixCategoryNameForNumbers(
        MIcuConversions::setCalendarOption(categoryNameTime, mCalendar.type()));
    categoryNameMessages = d->fixCategoryNameForNumbers(
        MIcuConversions::setCalendarOption(categoryNameMessages, mCalendar.type()));
    icu::SimpleDateFormat *formatter;
    if(d->_simpleDateFormatCache.contains(key)) {
        formatter = d->_simpleDateFormatCache.object(key);
    }
    else {
        UErrorCode status = U_ZERO_ERROR;
        formatter = new icu::SimpleDateFormat(
            MIcuConversions::qStringToUnicodeString(formatString),
            icu::Locale(qPrintable(categoryNameTime)), status);
        if(U_FAILURE(status)) {
            qWarning() << "icu::SimpleDateFormat() failed with error"
                       << u_errorName(status);
            formatter = NULL;
        }
        if (formatter && d->mixingSymbolsWanted(categoryNameMessages, categoryNameTime)) {
            // mixing in symbols like month name and weekday name from the message locale
            DateFormatSymbols *dfs =
                MLocalePrivate::createDateFormatSymbols(
                    icu::Locale(qPrintable(categoryNameMessages)));
            formatter->adoptDateFormatSymbols(dfs);
         }
        if(formatter)
            d->_simpleDateFormatCache.insert(key, formatter);
    }
    if(!formatter) {
        return QString();
    }
    else {
        icu::FieldPosition pos;
        icu::UnicodeString resString;
        formatter->format(*mCalendar.d_ptr->_calendar, resString, pos);
        return MIcuConversions::unicodeStringToQString(resString);
    }
}
#endif

#ifdef HAVE_ICU
// return weeknumber based on first week starting from the first given weekday of the year
// i.e. using sunday week 1 is the first week that contains sunday, zero before it
// note: week also starts from given weekday
// TODO: should this be moved to MCalendar?
static int weekNumberStartingFromDay(const MCalendar &calendar, int weekday)
{
    MCalendar calendarCopy = calendar;
    calendarCopy.setFirstDayOfWeek(weekday);
    calendarCopy.setMinimalDaysInFirstWeek(1);
    // this is icu week number, starts from 1
    int weeknumber = calendarCopy.weekNumber();

    // check if there's week 0
    bool weekZero = true;
    int year = calendarCopy.year();
    calendarCopy.setDate(year, 1, 1); // reuse the copy

    // a bit crude. check if the first week includes sunday
    // note: should start always from week 1 because minimal days is 1.
    while (calendarCopy.weekOfYear() == 1) {
        if (calendarCopy.dayOfWeek() == weekday) {
            weekZero = false;
        }

        calendarCopy.addDays(1);
    }

    if (weekZero == true) {
        weeknumber--;
    }

    return weeknumber;
}
#endif

#ifdef HAVE_ICU
QString MLocale::formatDateTime(const MCalendar &mCalendar,
                                  const QString &formatString) const
{
    Q_D(const MLocale);
    // convert POSIX format string into ICU format

    QString icuFormat;

    if ( ! d->_icuFormatStringCache.contains( formatString ) )
    {
        // determine if we can cache this format string, or if
        // we have to add something to it that is a part of a date or time.
        bool canCacheIcuFormat = true;

        bool isInNormalText = false; // a-zA-Z should be between <'>-quotations

        const int length = formatString.length();
        for (int i = 0; i < length; ++i) {

            QChar current = formatString.at(i);

            if (current == '%') {
                i++;
                QChar next = formatString.at(i);

                // end plain text icu quotation
                if (isInNormalText == true) {
                    icuFormat.append('\'');
                    isInNormalText = false;
                }

                switch (next.unicode()) {

                    case 'a':
                        // abbreviated weekday name
                        icuFormat.append("ccc");
                        break;

                    case 'A':
                        // stand-alone full weekday name
                        icuFormat.append("cccc");
                        break;

                    case 'b':
                    case 'h':
                        // abbreviated month name
                        icuFormat.append("LLL");
                    break;

                    case 'B':
                        // full month name
                        icuFormat.append("LLLL");
                        break;

                    case 'c': {
                        // FDCC-set's appropriate date and time representation

                        // This is ugly but possibly the only way to get the appropriate presentation
                        icu::Locale msgLocale = d->getCategoryLocale(MLcMessages);
                        DateFormat *df
                            = icu::DateFormat::createDateTimeInstance(icu::DateFormat::kDefault,
                                                                      icu::DateFormat::kDefault,
                                                                      msgLocale);
                        icu::UnicodeString dateTime;
                        icu::FieldPosition fieldPos;
                        dateTime = df->format(*mCalendar.d_ptr->_calendar, dateTime, fieldPos);
                        icuFormat.append('\'');
                        QString pattern = MIcuConversions::unicodeStringToQString(dateTime);
                        icuFormat.append(MIcuConversions::icuDatePatternEscaped(pattern));
                        icuFormat.append('\'');
                        delete df;

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'C': {
                        // century, no corresponding icu pattern
                        UnicodeString str;
                        d->_numberFormatLcTime->format(static_cast<int32_t>(mCalendar.year() / 100), str); //krazy:exclude=typedefs
                        icuFormat.append(MIcuConversions::unicodeStringToQString(str));

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'd':
                        // Day of the month as a decimal number (01-31)
                        icuFormat.append("dd");
                        break;

                    case 'D':
                        // %D Date in the format mm/dd/yy.
                        icuFormat.append("MM/dd/yy"); // yy really shortened?
                        break;

                    case 'e':
                        // correct? there should be explicit space fill or something?
                        icuFormat.append("d");
                        break;

                    case 'F':
                        //The date in the format YYYY-MM-DD (An ISO 8601 format).
                        icuFormat.append("yyyy-MM-dd");
                        break;

                    case 'g':
                        icuFormat.append("YY");
                        break;

                    case 'G':
                        icuFormat.append("YYYY");
                        break;

                    case 'H':
                        // Hour (24-hour clock), as a decimal number (00-23).
                        icuFormat.append("HH");
                        break;

                    case 'I':
                        // Hour (12-hour clock), as a decimal number (01-12).
                        icuFormat.append("hh");
                        break;

                    case 'j':
                        // day of year
                        icuFormat.append("DDD");
                        break;

                    case 'm':
                        // month
                        icuFormat.append("MM");
                        break;

                    case 'M':
                        // minute
                        icuFormat.append("mm");
                        break;

                    case 'n':
                        // newline
                        icuFormat.append('\n');
                        break;

                    case 'p':
                        // AM/PM
                        icuFormat.append("aaa");
                        break;

                    case 'r': {
                        // 12 hour clock with am/pm
                        QString timeShortFormat
                            = d->icuFormatString(MLocale::DateNone, MLocale::TimeShort,
                                                 MLocale::GregorianCalendar,
                                                 MLocale::TwelveHourTimeFormat24h);
                        icuFormat.append(timeShortFormat);
                        break;
                    }

                    case 'R': {
                        // 24-hour clock time, in the format "%H:%M"
                        QString timeShortFormat
                            = d->icuFormatString(MLocale::DateNone, MLocale::TimeShort,
                                                 MLocale::GregorianCalendar,
                                                 MLocale::TwentyFourHourTimeFormat24h);
                        icuFormat.append(timeShortFormat);
                        break;
                    }

                    case 'S':
                        // seconds
                        icuFormat.append("ss");
                        break;

                    case 't':
                        // tab
                        icuFormat.append('\t');
                        break;

                    case 'T': // FIXME!
                        // 24 hour clock HH:MM:SS
                        icuFormat.append("kk:mm:ss");
                        break;

                    case 'u': {
                        // Weekday, as a decimal number (1(Monday)-7)
                        // no corresponding icu pattern for monday based weekday
                        UnicodeString str;
                        d->_numberFormatLcTime->format(static_cast<int32_t>(mCalendar.dayOfWeek()), str); //krazy:exclude=typedefs
                        icuFormat.append(MIcuConversions::unicodeStringToQString(str));

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'U': {
                        // Week number of the year (Sunday as the first day of the week) as a
                        // decimal number (00-53). First week starts from first Sunday.
                        UnicodeString str;
                        d->_numberFormatLcTime->format(static_cast<int32_t>(0), str); //krazy:exclude=typedefs
                        d->_numberFormatLcTime->format(static_cast<int32_t>(weekNumberStartingFromDay(mCalendar, MLocale::Sunday)), str); //krazy:exclude=typedefs
                        QString weeknumber = MIcuConversions::unicodeStringToQString(str);
                        if (weeknumber.length() > 2)
                            weeknumber = weeknumber.right(2);
                        icuFormat.append(weeknumber);

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'v': // same as %V, for compatibility
                    case 'V': {
                        // Week of the year (Monday as the first day of the week), as a decimal
                        // number (01-53). according to ISO-8601
                        MCalendar calendarCopy = mCalendar;
                        calendarCopy.setFirstDayOfWeek(MLocale::Monday);
                        calendarCopy.setMinimalDaysInFirstWeek(4);
                        UnicodeString str;
                        d->_numberFormatLcTime->format(static_cast<int32_t>(0), str); //krazy:exclude=typedefs
                        d->_numberFormatLcTime->format(static_cast<int32_t>(calendarCopy.weekNumber()), str); //krazy:exclude=typedefs
                        QString weeknumber = MIcuConversions::unicodeStringToQString(str);
                        if (weeknumber.length() > 2)
                            weeknumber = weeknumber.right(2); // cut leading 0
                        icuFormat.append(weeknumber);

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'w': {
                        // Weekday, as a decimal number (0(Sunday)-6)
                        int weekday = mCalendar.dayOfWeek();
                        if (weekday == Sunday) {
                            weekday = 0;
                        }
                        UnicodeString str;
                        d->_numberFormatLcTime->format(static_cast<int32_t>(weekday), str); //krazy:exclude=typedefs
                        icuFormat.append(MIcuConversions::unicodeStringToQString(str));

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'W': {
                        // Week number of the year (Monday as the first day of the week), as a
                        // decimal number (00-53). Week starts from the first monday
                        int weeknumber = weekNumberStartingFromDay(mCalendar, MLocale::Monday);
                        UnicodeString str;
                        d->_numberFormatLcTime->format(static_cast<int32_t>(weeknumber), str); //krazy:exclude=typedefs
                        icuFormat.append(MIcuConversions::unicodeStringToQString(str));

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'x': {
                        // appropriate date representation
                        icu::Locale msgLocale = d->getCategoryLocale(MLcMessages);
                        DateFormat *df
                            = icu::DateFormat::createDateInstance(icu::DateFormat::kDefault,
                                                                  msgLocale);
                        icu::UnicodeString dateTime;
                        icu::FieldPosition fieldPos;
                        dateTime = df->format(*mCalendar.d_ptr->_calendar, dateTime, fieldPos);
                        icuFormat.append('\'');
                        QString pattern = MIcuConversions::unicodeStringToQString(dateTime);
                        icuFormat.append(MIcuConversions::icuDatePatternEscaped(pattern));
                        icuFormat.append('\'');
                        delete df;

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'X': {
                        // appropriate time representation
                        icu::Locale msgLocale = d->getCategoryLocale(MLcMessages);
                        DateFormat *df
                            = icu::DateFormat::createTimeInstance(icu::DateFormat::kDefault,
                                                                  msgLocale);
                        icu::UnicodeString dateTime;
                        icu::FieldPosition fieldPos;
                        dateTime = df->format(*mCalendar.d_ptr->_calendar, dateTime, fieldPos);
                        icuFormat.append('\'');
                        QString pattern = MIcuConversions::unicodeStringToQString(dateTime);
                        icuFormat.append(MIcuConversions::icuDatePatternEscaped(pattern));
                        icuFormat.append('\'');
                        delete df;

                        canCacheIcuFormat = false;
                        break;
                    }

                    case 'y':
                        // year within century
                        icuFormat.append("yy");
                        break;

                    case 'Y':
                        // year with century
                        icuFormat.append("yyyy");
                        break;

                    case 'z':
                        // The offset from UTC in the ISO 8601 format "-0430" (meaning 4 hours
                        // 30 minutes behind UTC, west of Greenwich), or by no characters if no
                        // time zone is determinable
                        icuFormat.append("Z"); // correct?
                        break;

                    case 'Z':
                        // ISO-14652 (draft):
                        //   Time-zone name, or no characters if no time zone is determinable
                        // Linux date command, strftime (glibc):
                        //   alphabetic time zone abbreviation (e.g., EDT)
                        // note that the ISO-14652 draft does not mention abbreviation,
                        // i.e. it is a bit unclear how exactly this should look like.
                        icuFormat.append("vvvv"); // generic time zone info
                        break;

                    case '%':
                        icuFormat.append("%");
                        break;
                }

            } else {
                if (current == '\'') {
                    icuFormat.append("''"); // icu escape

                } else if ((current >= 'a' && current <= 'z') || (current >= 'A' && current <= 'Z')) {
                    if (isInNormalText == false) {
                        icuFormat.append('\'');
                        isInNormalText = true;
                    }

                    icuFormat.append(current);

                } else {
                    icuFormat.append(current);
                }
            }
        } // for loop

        // save formatString -> icuFormat pair for future use,
        // if it does not contain content from the input date or time
        if ( canCacheIcuFormat )
        {
            QString* value = new QString( icuFormat );

            d->_icuFormatStringCache.insert( formatString, value );
        }
    }
    else
    {
        // formatString does exist in hash
        icuFormat = *d->_icuFormatStringCache[ formatString ];
    }

    return formatDateTimeICU(mCalendar, icuFormat);
}
#endif

#ifdef HAVE_ICU
QString MLocale::icuFormatString( DateType dateType,
                          TimeType timeType,
                          CalendarType calendarType) const
{
    Q_D(const MLocale);
    return d->icuFormatString(dateType, timeType, calendarType,
                              d->_timeFormat24h);
}
#endif

#ifdef HAVE_ICU
QDateTime MLocale::parseDateTime(const QString &dateTime, DateType dateType,
                                   TimeType timeType, CalendarType calendarType) const
{
    if (dateType == DateNone && timeType == TimeNone)
        return QDateTime();

    Q_D(const MLocale);
    MCalendar mcalendar(calendarType);

    UnicodeString text = MIcuConversions::qStringToUnicodeString(dateTime);
    icu::DateFormat *df = d->createDateFormat(dateType, timeType,
                                              mcalendar.type(),
                                              d->_timeFormat24h);
    icu::ParsePosition pos(0);
    UDate parsedDate;
    if (df)
        parsedDate = df->parse(text, pos);
    else
        return QDateTime();

    UErrorCode status = U_ZERO_ERROR;
    icu::Calendar *cal = mcalendar.d_ptr->_calendar;
    cal->setTime(parsedDate, status);
    if (status != U_ZERO_ERROR)
        return QDateTime();

    return mcalendar.qDateTime();
}
#endif

#ifdef HAVE_ICU
QDateTime MLocale::parseDateTime(const QString &dateTime, CalendarType calendarType) const
{
    return parseDateTime(dateTime, DateLong, TimeLong, calendarType);
}
#endif

#ifdef HAVE_ICU
QString MLocale::monthName(const MCalendar &mCalendar, int monthNumber) const
{
    return monthName(mCalendar, monthNumber, MLocale::DateSymbolStandalone, MLocale::DateSymbolWide);
}
#endif

#ifdef HAVE_ICU
QString MLocale::monthName(const MCalendar &mCalendar, int monthNumber,
                             DateSymbolContext context,
                             DateSymbolLength symbolLength) const
{
    Q_D(const MLocale);

    monthNumber--; // months in array starting from index zero

    QString categoryNameMessages = d->categoryName(MLcMessages);
    QString categoryName = d->categoryName(MLcTime);
    if(d->mixingSymbolsWanted(categoryNameMessages, categoryName))
        categoryName = categoryNameMessages;
    categoryName = MIcuConversions::setCalendarOption(categoryName, mCalendar.type());
    icu::Locale symbolLocale = icu::Locale(qPrintable(categoryName));

    icu::DateFormatSymbols *dfs = MLocalePrivate::createDateFormatSymbols(symbolLocale);

    // map context type
    icu::DateFormatSymbols::DtContextType icuContext =
        MIcuConversions::mDateContextToIcu(context);

    // map length type
    icu::DateFormatSymbols::DtWidthType icuWidth =
        MIcuConversions::mDateWidthToIcu(symbolLength);

    int len = -1;
    const UnicodeString *months = dfs->getMonths(len, icuContext, icuWidth);

    QString result;

    if (len > 0 && monthNumber < len && monthNumber >= 0) {
        result = MIcuConversions::unicodeStringToQString(months[monthNumber]);
    }

    delete dfs;

    if(!result.isEmpty() && context == MLocale::DateSymbolStandalone)
        result[0] = toUpper(result.at(0))[0];
    return result;
}
#endif

#ifdef HAVE_ICU
QString MLocale::weekdayName(const MCalendar &mCalendar, int weekday) const
{
    return weekdayName(mCalendar, weekday, MLocale::DateSymbolStandalone, MLocale::DateSymbolWide);
}
#endif

#ifdef HAVE_ICU
QString MLocale::weekdayName(const MCalendar &mCalendar, int weekday,
                               DateSymbolContext context,
                               DateSymbolLength symbolLength) const
{
    Q_D(const MLocale);
    QString categoryNameMessages = d->categoryName(MLcMessages);
    QString categoryName = d->categoryName(MLcTime);
     if(d->mixingSymbolsWanted(categoryNameMessages, categoryName))
        categoryName = categoryNameMessages;
    categoryName = MIcuConversions::setCalendarOption(categoryName, mCalendar.type());
    icu::Locale symbolLocale = icu::Locale(qPrintable(categoryName));

    icu::DateFormatSymbols *dfs = MLocalePrivate::createDateFormatSymbols(symbolLocale);

    icu::DateFormatSymbols::DtContextType icuContext
    = MIcuConversions::mDateContextToIcu(context);

    icu::DateFormatSymbols::DtWidthType icuWidth
    = MIcuConversions::mDateWidthToIcu(symbolLength);

    int len = -1;
    const UnicodeString *weekdayNames = dfs->getWeekdays(len, icuContext, icuWidth);
    int weekdayNum = MIcuConversions::icuWeekday(weekday);

    QString result;

    if (len > 0 && weekdayNum < len && weekdayNum > 0) {
        result = MIcuConversions::unicodeStringToQString(weekdayNames[weekdayNum]);
    }

    delete dfs;

    if(!result.isEmpty() && context == MLocale::DateSymbolStandalone)
        result[0] = toUpper(result.at(0))[0];
    return result;
}
#endif

QString MLocale::languageEndonym() const
{
#ifdef HAVE_ICU
    return languageEndonym(this->name());
#else
    Q_D(const MLocale);
    return QLocale::languageToString(d->createQLocale(MLcMessages).language());
#endif
}

QString MLocale::countryEndonym() const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    QString resourceBundleLocaleName = d->_defaultLocale;
    QString countryCode = country();
    if (countryCode.isEmpty())
        return QString();

    do {
        // Trying several resource bundles is a workaround for
        // http://site.icu-project.org/design/resbund/issues
        UErrorCode status = U_ZERO_ERROR;
        UResourceBundle *res = ures_open(U_ICUDATA_NAME "-region",
                                         qPrintable(resourceBundleLocaleName),
                                         &status);
        if (U_FAILURE(status)) {
            mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_open"
                              << u_errorName(status);
            ures_close(res);
            return countryCode;
        }
        res = ures_getByKey(res, Countries, res, &status);
        if (U_FAILURE(status)) {
            mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_getByKey"
                              << u_errorName(status);
            ures_close(res);
            return countryCode;
        }
        int len;
        const UChar *val = ures_getStringByKey(res,
                                               countryCode.toStdString().c_str(),
                                               &len,
                                               &status);
        ures_close(res);
        if (U_SUCCESS(status)) {
            // found country endonym, return it:
            return QString::fromUtf16(val, len);
        }
    } while (d->truncateLocaleName(&resourceBundleLocaleName));
    // no country endonym found and there are no other resource
    // bundles left to try, return the country code as a fallback:
    return countryCode;
#else
    Q_D(const MLocale);
    return QLocale::countryToString(d->createQLocale(MLcMessages).country());
#endif
}

QString MLocalePrivate::numberingSystem(const QString &localeName) const
{
#ifdef HAVE_ICU
    QString numberingSystem
        = MIcuConversions::parseOption(localeName, "numbers");
    // if the numbers option is there in the locale name, trust it
    // and return it, don’t test whether the requested numbering
    // system actually exists for this locale:
    if (!numberingSystem.isEmpty())
        return numberingSystem;
    QString resourceBundleLocaleName = localeName;
    numberingSystem = QLatin1String("latn");
    do {
        // Trying several resource bundles is a workaround for
        // http://site.icu-project.org/design/resbund/issues
        UErrorCode status = U_ZERO_ERROR;
        UResourceBundle *res = ures_open(NULL,
                                         qPrintable(resourceBundleLocaleName),
                                         &status);
        if (U_FAILURE(status)) {
            mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_open"
                              << resourceBundleLocaleName
                              << u_errorName(status);
            ures_close(res);
            return numberingSystem;
        }
        int len;
        res = ures_getByKey(res, "NumberElements", res, &status);
        if (U_FAILURE(status)) {
            ures_close(res);
            continue;
        }
        const UChar *val = ures_getStringByKey(res,
                                               "default",
                                               &len,
                                               &status);

        ures_close(res);
        if (U_SUCCESS(status)) {
            // found numbering system, return it:
            return QString::fromUtf16(val, len);
        }
    } while (truncateLocaleName(&resourceBundleLocaleName));
    // no numbering system found and there are no other resource
    // bundles left to try, return “latn” as a fallback:
    return numberingSystem;
#else
    QString language = parseLanguage(localeName);
    if(language == "ar")
        return QString("arab");
    else if (language == "fa")
        return QString("arabext");
    else if (language == "hi")
        return QString("deva");
    else if (language == "kn")
        return QString("knda");
    else if (language == "mr")
        return QString("deva");
    else if (language == "ne")
        return QString("deva");
    else if (language == "or")
        return QString("orya");
    else if (language == "pa")
        return QString("guru");
    else if (language == "bn")
        return QString("beng");
    else
        return QString("latn");
#endif
}

QString MLocale::decimalPoint() const
{
#ifdef HAVE_ICU
    Q_D(const MLocale);
    QString categoryNameNumeric =
        d->fixCategoryNameForNumbers(d->categoryName(MLocale::MLcNumeric));
    QString numberingSystem = d->numberingSystem(categoryNameNumeric);
    QString resourceBundleLocaleName = categoryNameNumeric;
    QString decimal = QLatin1String(".");
    do {
        // Trying several resource bundles is a workaround for
        // http://site.icu-project.org/design/resbund/issues
        UErrorCode status = U_ZERO_ERROR;
        UResourceBundle *res = ures_open(NULL,
                                         qPrintable(resourceBundleLocaleName),
                                         &status);
        if (U_FAILURE(status)) {
            mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_open"
                              << resourceBundleLocaleName
                              << u_errorName(status);
            ures_close(res);
            return decimal;
        }
        res = ures_getByKey(res, "NumberElements", res, &status);
        if (U_FAILURE(status)) {
            ures_close(res);
            continue;
        }
        int len;
        res = ures_getByKey(res, numberingSystem.toStdString().c_str(),
                            res, &status);
        if (U_FAILURE(status)) {
            ures_close(res);
            continue;
        }
        res = ures_getByKey(res, "symbols", res, &status);
        if (U_FAILURE(status)) {
            ures_close(res);
            continue;
        }
        const UChar *val = ures_getStringByKey(res, "decimal", &len, &status);
        ures_close(res);
        if (U_SUCCESS(status)) {
            // found decimal point, return it:
            return QString::fromUtf16(val, len);
        }
    } while (d->truncateLocaleName(&resourceBundleLocaleName));
    // no decimal point found and there are no other resource
    // bundles left to try, return “.” as a fallback:
    return decimal;
#else
    Q_D(const MLocale);
    return d->createQLocale(MLcNumeric).decimalPoint();
#endif
}

#ifdef HAVE_ICU
QString MLocale::joinStringList(const QStringList &texts) const
{
    QStringList textsWithBidiMarkers;
    QString separator(QLatin1String(", "));
#if 0 // non-functional
    if (localeScripts()[0] == QLatin1String("Arab"))
        separator = QString::fromUtf8("، "); // U+060C ARABIC COMMA + space
    else if (localeScripts().contains(QLatin1String("Hani"))) {
        separator = QString::fromUtf8("、"); // U+3001 IDEOGRAPHIC COMMA, no space
    }
#endif
    foreach (const QString &text, texts) {
        if (MLocale::directionForText(text) == Qt::RightToLeft)
            // RIGHT-TO-LEFT EMBEDDING + text + POP DIRECTIONAL FORMATTING
            textsWithBidiMarkers << QChar(0x202B) + text + QChar(0x202C);
        else
            // LEFT-TO-RIGHT EMBEDDING + text + POP DIRECTIONAL FORMATTING
            textsWithBidiMarkers << QChar(0x202A) + text + QChar(0x202C);
    }
    return textsWithBidiMarkers.join(separator);
}
#endif

#ifdef HAVE_ICU
QStringList MLocale::exemplarCharactersIndex() const
{
    Q_D(const MLocale);
    QString collationLocaleName = d->categoryName(MLcCollate);
    // exemplarCharactersIndex is initialized with A...Z which is
    // returned as a fallback when no real index list can be found for
    // the current locale:
    QStringList exemplarCharactersIndex
        = QString::fromUtf8("A B C D E F G H I J K L M N O P Q R S T U V W X Y Z")
#if QT_VERSION < 0x051500
            .split(QLatin1String(" "),QString::SkipEmptyParts);
#else
            .split(QLatin1String(" "),Qt::SkipEmptyParts);
#endif
    QString charStr;
    if (collationLocaleName.contains(QLatin1String("collation=unihan"))) {
        charStr = QString::fromUtf8("⼀ ⼁ ⼂ ⼃ ⼄ ⼅ ⼆ ⼇ ⼈ ⼉ ⼊ ⼋ ⼌ ⼍ ⼎ ⼏ ⼐ ⼑ ⼒ ⼓ ⼔ ⼕ ⼖ ⼗ ⼘ ⼙ ⼚ ⼛ ⼜ ⼝ ⼞ ⼟ ⼠ ⼡ ⼢ ⼣ ⼤ ⼥ ⼦ ⼧ ⼨ ⼩ ⼪ ⼫ ⼬ ⼭ ⼮ ⼯ ⼰ ⼱ ⼲ ⼳ ⼴ ⼵ ⼶ ⼷ ⼸ ⼹ ⼺ ⼻ ⼼ ⼽ ⼾ ⼿ ⽀ ⽁ ⽂ ⽃ ⽄ ⽅ ⽆ ⽇ ⽈ ⽉ ⽊ ⽋ ⽌ ⽍ ⽎ ⽏ ⽐ ⽑ ⽒ ⽓ ⽔ ⽕ ⽖ ⽗ ⽘ ⽙ ⽚ ⽛ ⽜ ⽝ ⽞ ⽟ ⽠ ⽡ ⽢ ⽣ ⽤ ⽥ ⽦ ⽧ ⽨ ⽩ ⽪ ⽫ ⽬ ⽭ ⽮ ⽯ ⽰ ⽱ ⽲ ⽳ ⽴ ⽵ ⽶ ⽷ ⽸ ⽹ ⽺ ⽻ ⽼ ⽽ ⽾ ⽿ ⾀ ⾁ ⾂ ⾃ ⾄ ⾅ ⾆ ⾇ ⾈ ⾉ ⾊ ⾋ ⾌ ⾍ ⾎ ⾏ ⾐ ⾑ ⾒ ⾓ ⾔ ⾕ ⾖ ⾗ ⾘ ⾙ ⾚ ⾛ ⾜ ⾝ ⾞ ⾟ ⾠ ⾡ ⾢ ⾣ ⾤ ⾥ ⾦ ⾧ ⾨ ⾩ ⾪ ⾫ ⾬ ⾭ ⾮ ⾯ ⾰ ⾱ ⾲ ⾳ ⾴ ⾵ ⾶ ⾷ ⾸ ⾹ ⾺ ⾻ ⾼ ⾽ ⾾ ⾿ ⿀ ⿁ ⿂ ⿃ ⿄ ⿅ ⿆ ⿇ ⿈ ⿉ ⿊ ⿋ ⿌ ⿍ ⿎ ⿏ ⿐ ⿑ ⿒ ⿓ ⿔ ⿕");
        // add a dummy bucket at the end 𪛖 is the last character in unihan order:
        charStr += QString::fromUtf8(" 𪛖");
        return charStr
#if QT_VERSION < 0x051500
            .split(QLatin1String(" "),QString::SkipEmptyParts);
#else
            .split(QLatin1String(" "),Qt::SkipEmptyParts);
#endif
    }
    // special treatment for Chinese locales because these have the
    // collation options "stroke" and "pinyin" which require different
    // index buckets.  But libicu currently supports only one index
    // bucket list per locale.  As a workaround, force use of the
    // index bucket list from the zh_TW locale if collation=stroke is
    // set and force the use of the index bucket list from the zh_CN
    // locale if collation=pinyin is set:
    if(collationLocaleName.startsWith(QLatin1String("zh"))) {
        if(collationLocaleName.contains(QLatin1String("collation=zhuyin"))) {
            charStr = QString::fromUtf8("ㄅ ㄆ ㄇ ㄈ ㄉ ㄊ ㄋ ㄌ ㄍ ㄎ ㄏ ㄐ ㄑ ㄒ ㄓ ㄔ ㄕ ㄖ ㄗ ㄘ ㄙ ㄧ ㄨ ㄩ ㄚ ㄛ ㄜ ㄝ ㄞ ㄟ ㄠ ㄡ ㄢ ㄣ ㄤ ㄥ ㄦ ㄪ ㄫ ㄬ ㄭ");
            return charStr
#if QT_VERSION < 0x051500
                .split(QLatin1String(" "),QString::SkipEmptyParts);
#else
                .split(QLatin1String(" "),Qt::SkipEmptyParts);
#endif
        }
        if(collationLocaleName.contains(QLatin1String("collation=pinyinsearch"))) {
            collationLocaleName = QLatin1String("zh_CN@collation=pinyinsearch");
            charStr = QString::fromUtf8("A B C D E F G H I J K L M N O P Q R S T U V W X Y Z");
            exemplarCharactersIndex =
                charStr
#if QT_VERSION < 0x051500
                    .split(QLatin1String(" "),QString::SkipEmptyParts);
#else
                    .split(QLatin1String(" "),Qt::SkipEmptyParts);
#endif
            // to get all characters with pinyin starting with z
            // (last one is 蓙) into the Z bucket
            exemplarCharactersIndex << QString::fromUtf8("Α"); // GREEK CAPITAL LETTER ALPHA
            return exemplarCharactersIndex;
        }
        if(collationLocaleName.contains(QLatin1String("collation=stroke")))
            collationLocaleName = QLatin1String("zh_TW@collation=stroke");
        if(collationLocaleName.contains(QLatin1String("collation=pinyin")))
            collationLocaleName = QLatin1String("zh_CN@collation=pinyin");
    }

    UErrorCode status = U_ZERO_ERROR;
    UResourceBundle *res =
        ures_open(NULL, collationLocaleName.toUtf8().constData(), &status);

    if (U_FAILURE(status)) {
        mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_open"
                          << collationLocaleName << u_errorName(status);
        ures_close(res);
        return exemplarCharactersIndex;
    }

    qint32 len;
    status = U_ZERO_ERROR;
    const UChar *val = ures_getStringByKey(res,
                                           "ExemplarCharactersIndex",
                                           &len, &status);
    if (U_FAILURE(status)) {
        mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_getStringByKey"
                          << collationLocaleName << u_errorName(status);
        ures_close(res);

        return exemplarCharactersIndex;
    }

    charStr = QString::fromUtf16(val, len);
    ures_close(res);
    charStr.remove('[');
    charStr.remove(']');
    charStr.remove('{');
    charStr.remove('}');
    exemplarCharactersIndex = charStr
#if QT_VERSION < 0x051500
        .split(QLatin1String(" "),QString::SkipEmptyParts);
#else
        .split(QLatin1String(" "),Qt::SkipEmptyParts);
#endif

    // Special hack for the last Japanese bucket:
    if (exemplarCharactersIndex.last() == QString::fromUtf8("わ")) {
        exemplarCharactersIndex << QString::fromUtf8("ん"); // to get ワ, ゐ,ヰ,ヸ, ヹ, を, ヲ, ヺ, into the わ bucket
    }
    // Special hack for the last Korean bucket:
    if (exemplarCharactersIndex.last() == QString::fromUtf8("ᄒ")) {
        exemplarCharactersIndex << QString::fromUtf8("あ"); // to get 학,  學, ... ᄒ bucket
    }
    // Special hacks for the pinyin buckets:
    if (exemplarCharactersIndex.last() == QString::fromUtf8("Z") &&
        (collationLocaleName.contains(QLatin1String("collation=pinyin"))
         || collationLocaleName.startsWith(QLatin1String("zh_CN"))
         || collationLocaleName.startsWith(QLatin1String("zh_SG")))
        ) {
        charStr = QString::fromUtf8("ａ ｂ ｃ ｄ ｅ ｆ ｇ ｈ ｉ ｊ ｋ ｌ ｍ ｎ ｏ ｐ ｑ ｒ ｓ ｔ ｕ ｖ ｗ ｘ ｙ ｚ A B C D E F G H I J K L M N O P Q R S T U V W X Y Z");
        return charStr
#if QT_VERSION < 0x051500
                .split(QLatin1String(" "),QString::SkipEmptyParts);
#else
                .split(QLatin1String(" "),Qt::SkipEmptyParts);
#endif
    }
    return exemplarCharactersIndex;
}
#endif

#ifdef HAVE_ICU
QString MLocale::indexBucket(const QString &str, const QStringList &buckets, const MCollator &coll) const
{
    Q_D(const MLocale);
    if (str.isEmpty())
        return str;
    if (str.startsWith(QString::fromUtf8("𪛖")) && buckets.last() == QString::fromUtf8("𪛖")) {
        // 𪛖 is the last character in unihan order, should go into the ⿕ bucket
        return QString::fromUtf8("⿕");
    }
    if (str.startsWith(QString::fromUtf8("ン")) && buckets.last() == QString::fromUtf8("ん")) {
        // ン sorts after ん but should go into the ん bucket:
        return QString::fromUtf8("ん");
    }
    QString strUpperCase =
        MIcuConversions::unicodeStringToQString(
            MIcuConversions::qStringToUnicodeString(str).toUpper(
                d->getCategoryLocale(MLocale::MLcCollate)));
    if (strUpperCase.isEmpty())
        return strUpperCase;
    QString firstCharacter;
    if (strUpperCase.at(0).isHighSurrogate() && strUpperCase.length() > 1)
        firstCharacter = strUpperCase.at(0) + strUpperCase.at(1);
    else
        firstCharacter = strUpperCase.at(0);
    firstCharacter = MLocalePrivate::removeAccents(firstCharacter);
    if (firstCharacter.isEmpty())
        return firstCharacter;
    // removing the accents as above also does expansions
    // like “㈠ → (一)”. If this happened, take the first character
    //  of the expansion:
    if (!firstCharacter.at(0).isHighSurrogate())
        firstCharacter = firstCharacter.at(0);
    if (firstCharacter[0].isNumber())
        firstCharacter = this->toLocalizedNumbers(firstCharacter);
    for (int i = 0; i < buckets.size(); ++i) {
        if (coll(strUpperCase, buckets[i])) {
            if (i == 0) {
                return firstCharacter;
            } else if (buckets.first() == QString::fromUtf8("一")) { // stroke count sorting
                return QString::number(i) + QString::fromUtf8("劃");
            } else if (i > 1 && !coll(buckets[i-2], buckets[i-1])
                       && !str.startsWith(buckets[i-1], Qt::CaseInsensitive)) {
                // some locales have conflicting data as in exemplar characters containing accented variants
                // of some letters while collation doesn't have primary level difference between them,
                // for example hungarian short and long vowels, and russian Е/Ё.
                // in such case return the earlier bucket for all strings that don't start with the latter
                // To consider: do we need to handle even longer runs of primary level equal buckets?
                return buckets[i-2];
            }

            return buckets[i-1];
        }
    }
    // return the last bucket if any substring starting from the beginning compares
    // primary equal to the last bucket label:
    for (int i = 0; i < strUpperCase.size(); ++i) {
        if (!coll(buckets.last(),strUpperCase.left(i+1))
                && !coll(strUpperCase.left(i+1), buckets.last())) {
            return buckets.last();
        }
    }
    // last resort, no appropriate bucket found:
    return firstCharacter;
}
#endif

#ifdef HAVE_ICU
QString MLocale::indexBucket(const QString &str) const
{
    QStringList bucketList = exemplarCharactersIndex();
    MCollator coll = this->collator();
    coll.setStrength(MLocale::CollatorStrengthPrimary);
    return indexBucket(str, bucketList, coll);
}
#endif

QStringList MLocale::localeScripts() const
{
    QStringList scripts;
    qWarning() << "MLocale::localeScripts() missing proper implementation. Add if needed.";
#if 0 // LocaleScript data removed from ICU, no point trying.
#ifdef HAVE_ICU
    Q_D(const MLocale);
    UErrorCode status = U_ZERO_ERROR;

    UResourceBundle *res = ures_open(NULL, qPrintable(d->_defaultLocale), &status);

    if (U_FAILURE(status)) {
        mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_open"
                          << u_errorName(status);
    }

    res = ures_getByKey(res, "LocaleScript", res, &status);
    if (U_FAILURE(status)) {
        mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_getByKey"\
                          << u_errorName(status);
    }

    qint32 len;
    const UChar *val;

    while (NULL != (val = ures_getNextString(res, &len, NULL, &status))) {
        if (U_SUCCESS(status))
            scripts << QString::fromUtf16(val, len);
    }
    ures_close(res);
#endif
#endif

    if (scripts.isEmpty())
        // "Zyyy" Code for undetermined script,
        // see http://www.unicode.org/iso15924/iso15924-codes.html
        scripts << "Zyyy";

    return scripts;
}

void MLocale::copyCatalogsFrom(const MLocale &other)
{
    Q_D(MLocale);

    MLocalePrivate::CatalogList::const_iterator end =
        other.d_ptr->_messageTranslations.constEnd();

    for (MLocalePrivate::CatalogList::const_iterator i
            = other.d_ptr->_messageTranslations.constBegin();
            i != end; ++i) {

        // copy the name info
        MTranslationCatalog *tempCatalog = new MTranslationCatalog(**i);

        // reload the file
        tempCatalog->loadWith(this, MLcMessages);
        d->_messageTranslations.append(QExplicitlySharedDataPointer<MTranslationCatalog>(tempCatalog));

    }

    end = other.d_ptr->_timeTranslations.constEnd();
    for (MLocalePrivate::CatalogList::const_iterator i = other.d_ptr->_timeTranslations.constBegin();
            i != end; ++i) {

        MTranslationCatalog *tempCatalog = new MTranslationCatalog(**i);
        tempCatalog->loadWith(this, MLcTime);
        d->_timeTranslations.append(QExplicitlySharedDataPointer<MTranslationCatalog>(tempCatalog));
    }

    end = other.d_ptr->_trTranslations.constEnd();
    for (MLocalePrivate::CatalogList::const_iterator i = other.d_ptr->_trTranslations.constBegin();
            i != end; ++i) {

        MTranslationCatalog *tempCatalog = new MTranslationCatalog(**i);
        tempCatalog->loadWith(this, MLcMessages);
        d->_trTranslations.append(QExplicitlySharedDataPointer<MTranslationCatalog>(tempCatalog));

    }

}

void MLocale::installTrCatalog(const QString &name)
{
    Q_D(MLocale);

    // Make sure that previous installations of a catalog are removed
    // first before trying to install a catalog.  There is no need to
    // install the same catalog more then once with different
    // priorities.  One could skip the installation altogether if the
    // catalog is already installed, but it is better to remove the
    // first instance, then the priorities make more sense.
    // See https://projects.maemo.org/bugzilla/show_bug.cgi?id=198551
    removeTrCatalog(name);

    MTranslationCatalog *catalog
        = new MTranslationCatalog(name);
    catalog->loadWith(this, MLocale::MLcMessages);
    d->_trTranslations.append(QExplicitlySharedDataPointer<MTranslationCatalog>(catalog));
    if (!name.endsWith(QLatin1String(".qm"))) {
        MTranslationCatalog *engineeringEnglishCatalog
            = new MTranslationCatalog(name + ".qm");
        engineeringEnglishCatalog->loadWith(this, MLocale::MLcMessages);
        d->_trTranslations.prepend(QExplicitlySharedDataPointer<MTranslationCatalog>(engineeringEnglishCatalog));
    }
}

void MLocale::removeTrCatalog(const QString &name)
{
    Q_D(MLocale);
    MLocalePrivate::CatalogList::iterator it = d->_trTranslations.begin();
    while (it != d->_trTranslations.end()) {
        if ((*it)->_name == name || (*it)->_name == name + ".qm") {
            // Reset to null but first decrement reference count of
            // the shared data object and delete the shared data
            // object if the reference count became 0:
            (*it).reset();
            it = d->_trTranslations.erase(it);
        }
        else
            ++it;
    }
}

bool MLocale::isInstalledTrCatalog(const QString &name) const
{
    Q_D(const MLocale);
    if(name.isEmpty())
        return false;
    MLocalePrivate::CatalogList::const_iterator it = d->_trTranslations.constBegin();
    while (it != d->_trTranslations.constEnd()) {
        if ((*it)->_name == name)
            return true;
        else
            ++it;
    }
    return false;
}

/////////////////////////////
//// translation methods ////

QString MLocale::translate(const char *context, const char *sourceText,
                             const char *comment, int n)
{
    Q_D(MLocale);
    const MLocalePrivate::CatalogList::const_iterator begin = d->_trTranslations.constBegin();
    MLocalePrivate::CatalogList::const_iterator it = d->_trTranslations.constEnd();
    while (it != begin) {
        --it;

        QString translation = (*it)->_translator.translate(context, sourceText,
                              comment, n);

        if (translation.isEmpty() == false) {
            replacePercentN(&translation, n);
            return translation;
        }
    }

    return sourceText;
}

void MLocale::setDataPaths(const QStringList &dataPaths)
{
    MLocalePrivate::dataPaths = dataPaths;
#ifdef HAVE_ICU
    QString pathString;

    foreach(QString string, dataPaths) { // krazy:exclude=foreach
        string.replace('/', U_FILE_SEP_CHAR);
        pathString.append(string);
        // separator gets appended to the end of the list. I hope Icu doesn't mind
        pathString.append(U_PATH_SEP_CHAR);
    }

    u_setDataDirectory(qPrintable(pathString));
#endif
}

// convenience method for just one path
void MLocale::setDataPath(const QString &dataPath)
{
    setDataPaths(QStringList(dataPath));
}

QStringList MLocale::dataPaths()
{
    return MLocalePrivate::dataPaths;
}

QString MLocale::toLocalizedNumbers(const QString &text) const
{
    Q_D(const MLocale);
    QString categoryNameNumeric =
        d->fixCategoryNameForNumbers(d->categoryName(MLocale::MLcNumeric));
    QString targetNumberingSystem = d->numberingSystem(categoryNameNumeric);
    QString targetDigits;
#ifdef HAVE_ICU
    UErrorCode status = U_ZERO_ERROR;
    bool ok = true;
    icu::NumberingSystem * targetNumSys =
            NumberingSystem::createInstanceByName(
                targetNumberingSystem.toLatin1().constData(), status);
    if(U_FAILURE(status)) {
        mDebug("MLocale") << __PRETTY_FUNCTION__
                          << "Error NumberingSystem::createInstanceByName()"
                          << targetNumberingSystem
                          << u_errorName(status);
        ok = false;
    }
    else {
        if(!targetNumSys->isAlgorithmic() && targetNumSys->getRadix() == 10) {
            targetDigits = MIcuConversions::unicodeStringToQString(
                        targetNumSys->getDescription());
            if(targetDigits.size() != 10) {
                mDebug("MLocale")
                        << __PRETTY_FUNCTION__
                        << targetNumberingSystem
                        << "number of digits is not 10, should not happen";
                ok = false;
            }
        }
        else {
            mDebug("MLocale")
                    << __PRETTY_FUNCTION__
                    << targetNumberingSystem
                    << "not algorithmic or radix not 10, should not happen";
            ok = false;
        }
    }
    delete targetNumSys;
    if (!ok)
        return text;
#else
    if(targetNumberingSystem == "arab")
        targetDigits = QString::fromUtf8("٠١٢٣٤٥٦٧٨٩");
    else if(targetNumberingSystem == "arabext")
        targetDigits = QString::fromUtf8("۰۱۲۳۴۵۶۷۸۹");
    else if(targetNumberingSystem == "beng")
        targetDigits = QString::fromUtf8("০১২৩৪৫৬৭৮৯");
    else if(targetNumberingSystem == "deva")
        targetDigits = QString::fromUtf8("०१२३४५६७८९");
    else if(targetNumberingSystem == "fullwide")
        targetDigits = QString::fromUtf8("０１２３４５６７８９");
    else if(targetNumberingSystem == "gujr")
        targetDigits = QString::fromUtf8("૦૧૨૩૪૫૬૭૮૯");
    else if(targetNumberingSystem == "guru")
        targetDigits = QString::fromUtf8("੦੧੨੩੪੫੬੭੮੯");
    else if(targetNumberingSystem == "hanidec")
        targetDigits = QString::fromUtf8("〇一二三四五六七八九");
    else if(targetNumberingSystem == "khmr")
        targetDigits = QString::fromUtf8("០១២៣៤៥៦៧៨៩");
    else if(targetNumberingSystem == "knda")
        targetDigits = QString::fromUtf8("೦೧೨೩೪೫೬೭೮೯");
    else if(targetNumberingSystem == "laoo")
        targetDigits = QString::fromUtf8("໐໑໒໓໔໕໖໗໘໙");
    else if(targetNumberingSystem == "latn")
        targetDigits = QString::fromUtf8("0123456789");
    else if(targetNumberingSystem == "mlym")
        targetDigits = QString::fromUtf8("൦൧൨൩൪൫൬൭൮൯");
    else if(targetNumberingSystem == "mong")
        targetDigits = QString::fromUtf8("᠐᠑᠒᠓᠔᠕᠖᠗᠘᠙");
    else if(targetNumberingSystem == "mymr")
        targetDigits = QString::fromUtf8("၀၁၂၃၄၅၆၇၈၉");
    else if(targetNumberingSystem == "orya")
        targetDigits = QString::fromUtf8("୦୧୨୩୪୫୬୭୮୯");
    else if(targetNumberingSystem == "telu")
        targetDigits = QString::fromUtf8("౦౧౨౩౪౫౬౭౮౯");
    else if(targetNumberingSystem == "thai")
        targetDigits = QString::fromUtf8("๐๑๒๓๔๕๖๗๘๙");
    else if(targetNumberingSystem == "tibt")
        targetDigits = QString::fromUtf8("༠༡༢༣༤༥༦༧༨༩");
    else
        targetDigits = QString::fromUtf8("0123456789");
#endif
    return MLocale::toLocalizedNumbers(text, targetDigits);
}

QString MLocale::toLocalizedNumbers(const QString &text, const QString &targetDigits)
{
    QString result = text;
    if(targetDigits.size() != 10)
        return text;
    if(targetDigits == QLatin1String("0123456789")) {
        bool isLatin1 = true;
        for(int i = 0; i < text.size(); ++i) {
            if(!text.at(i).toLatin1()) {
                isLatin1 = false;
                break;
            }
        }
        if(isLatin1)
            return text;
        result.remove(QChar(0x200F)); // RIGHT-TO-LEFT MARK
        result.remove(QChar(0x200E)); // LEFT-TO-RIGHT MARK
        result.remove(QChar(0x202D)); // LEFT-TO-RIGHT OVERRIDE
        result.remove(QChar(0x202E)); // RIGHT-TO-LEFT OVERRIDE
        result.remove(QChar(0x202A)); // LEFT-TO-RIGHT EMBEDDING
        result.remove(QChar(0x202B)); // RIGHT-TO-LEFT EMBEDDING
        result.remove(QChar(0x202C)); // POP DIRECTIONAL FORMATTING
    }
    QStringList sourceDigitsList;
    sourceDigitsList << QString::fromUtf8("〇一二三四五六七八九");
    foreach(const QString &sourceDigits, sourceDigitsList)
        for(int i = 0; i <= 9; ++i)
            result.replace(sourceDigits.at(i), targetDigits.at(i));
    for(int i = 0; i < result.size(); ++i)
        if(result[i].isNumber() && result[i].digitValue() >= 0)
            result[i] = targetDigits[result[i].digitValue()];
    return result;
}

QString MLocale::toLatinNumbers(const QString &text)
{
    return MLocale::toLocalizedNumbers(text, QLatin1String("0123456789"));
}

#ifdef HAVE_ICU
QString MLocale::localeScript(const QString &locale)
{
    QString s = MLocalePrivate::parseScript(locale);

    if(!s.isEmpty())
        return s;

    UErrorCode status = U_ZERO_ERROR;
    UResourceBundle *res = ures_open(NULL, qPrintable(locale), &status);
    if(U_FAILURE(status)) // TODO: error handling++
        return QString();

    res = ures_getByKey(res, "LocaleScript", res, &status);
    if(U_FAILURE(status)) {
        ures_close(res);
        return QString();
    }

    QString ret("Zyyy");

    qint32 len;
    const UChar *v;
    v = ures_getNextString(res, &len, NULL, &status);
    if(v && U_SUCCESS(status))
        ret = QString::fromUtf16(v, len);

    ures_close(res);

    return ret;
}

QString MLocale::languageEndonym(const QString &locale)
{
    QString resourceBundleLocaleName = locale;

    do {
        // Trying several resource bundles is a workaround for
        // http://site.icu-project.org/design/resbund/issues
        UErrorCode status = U_ZERO_ERROR;
        UResourceBundle *res = ures_open(U_ICUDATA_NAME "-lang",
                                         qPrintable(resourceBundleLocaleName),
                                         &status);
        if (U_FAILURE(status)) {
            mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_open"
                              << u_errorName(status);
            ures_close(res);
            return locale;
        }
        res = ures_getByKey(res, Languages, res, &status);
        if (U_FAILURE(status)) {
            mDebug("MLocale") << __PRETTY_FUNCTION__ << "Error ures_getByKey"
                              << u_errorName(status);
            ures_close(res);
            return locale;
        }
        QString keyLocaleName = locale;
        // it’s not nice if “zh_CN”, “zh_HK”, “zh_MO”, “zh_TW” all fall back to
        // “zh” for the language endonym and display only “中文”.
        // To make the fallbacks work better, insert the script:
        if (keyLocaleName.startsWith(QLatin1String("zh_CN")))
            keyLocaleName = "zh_Hans_CN";
        else if (keyLocaleName.startsWith(QLatin1String("zh_SG")))
            keyLocaleName = "zh_Hans_SG";
        else if (keyLocaleName.startsWith(QLatin1String("zh_HK")))
            keyLocaleName = "zh_Hant_HK";
        else if (keyLocaleName.startsWith(QLatin1String("zh_MO")))
            keyLocaleName = "zh_Hant_MO";
        else if (keyLocaleName.startsWith(QLatin1String("zh_TW")))
            keyLocaleName = "zh_Hant_TW";
        do { // FIXME: this loop should probably be somewhere else
            int len;
            status = U_ZERO_ERROR;
            const UChar *val = ures_getStringByKey(res,
                                                   qPrintable(keyLocaleName),
                                                   &len,
                                                   &status);
            if (U_SUCCESS(status)) {
                // found language endonym, return it:
                ures_close(res);
                return QString::fromUtf16(val, len);
            }
        } while (MLocalePrivate::truncateLocaleName(&keyLocaleName));
        // no language endonym found in this resource bundle and there
        // is no way to shorten keyLocaleName, try the next resource
        // bundle:
        ures_close(res);
    } while (MLocalePrivate::truncateLocaleName(&resourceBundleLocaleName));
    // no language endonym found at all, no other keys or resource
    // bundles left to try, return the full locale name as a fallback:
    return locale;
}
#endif

///////
// the static convenience methods for translation

void MLocale::setTranslationPaths(const QStringList &paths)
{
    MLocalePrivate::translationPaths = paths;
}

void MLocale::addTranslationPath(const QString &path)
{
    if (MLocalePrivate::translationPaths.contains(path) == false) {
        MLocalePrivate::translationPaths << path;
    }
}

void MLocale::removeTranslationPath(const QString &path)
{
    MLocalePrivate::translationPaths.removeOne(path);
}

QStringList MLocale::translationPaths()
{
    return MLocalePrivate::translationPaths;
}

Qt::LayoutDirection MLocale::defaultLayoutDirection()
{
    return _defaultLayoutDirection;
}

Qt::LayoutDirection MLocale::textDirection() const
{
#ifdef HAVE_ICU
    Qt::LayoutDirection layoutDirectionOption =
        MIcuConversions::parseLayoutDirectionOption(this->name());
#else
    Qt::LayoutDirection layoutDirectionOption = Qt::LeftToRight;
#endif

    if (layoutDirectionOption == Qt::LayoutDirectionAuto) {
        // choose the layout direction automatically depending on the
        // script used by the locale (old behaviour of this function):
        //
        // Checking for the script "arab" is needed for
        // locales where the language can be written in several scripts.
        // Eg the Uyghur language can be written in Chinese, Cyrillic,
        // or Arabic script.
        if (script().contains("arab", Qt::CaseInsensitive))
            layoutDirectionOption = Qt::RightToLeft;
        else if (!language().isEmpty()
                 && RtlLanguages.contains(language() + ':'))
            layoutDirectionOption = Qt::RightToLeft;
        else
            layoutDirectionOption = Qt::LeftToRight;
    }
    return layoutDirectionOption;
}

Qt::LayoutDirection MLocale::directionForText(const QString & text)
{
    QString::const_iterator textEnd = text.constEnd();

    for ( QString::const_iterator it = text.constBegin(); it != textEnd; ++it ) {
        // Taken from qtextobject.cpp
        switch(QChar::direction(it->unicode()))
        {
            case QChar::DirL:
                return Qt::LeftToRight;
            case QChar::DirR:
            case QChar::DirAL:
                return Qt::RightToLeft;
            default:
                break;
        }
    }
    return Qt::LeftToRight;
}

void MLocale::refreshSettings()
{
    Q_D(MLocale);

    bool settingsHaveReallyChanged = false;

    QString localeName      = d->pCurrentLanguage->value();
    QString lcTime          = d->pCurrentLcTime->value();
    QString lcTimeFormat24h = d->pCurrentLcTimeFormat24h->value();
    QString lcCollate       = d->pCurrentLcCollate->value();
    QString lcNumeric       = d->pCurrentLcNumeric->value();
    QString lcMonetary      = d->pCurrentLcMonetary->value();
    QString lcTelephone     = d->pCurrentLcTelephone->value();

    if ( !d->pCurrentLanguage->isValid() )        localeName = "en_GB";
    if ( !d->pCurrentLcTime->isValid() )          lcTime = "en_GB";
    if ( !d->pCurrentLcTimeFormat24h->isValid() ) lcTimeFormat24h = "12";
    if ( !d->pCurrentLcCollate->isValid() )       lcCollate = "en_GB";
    if ( !d->pCurrentLcNumeric->isValid() )       lcNumeric = "en_GB";
    if ( !d->pCurrentLcMonetary->isValid() )      lcMonetary = "en_GB";
    // no default for lcTelephone

    if (localeName != d->_defaultLocale) {
        settingsHaveReallyChanged = true;
        d->_defaultLocale = localeName;
        // force recreation of the number formatter if
        // the numeric locale inherits from the default locale:
        if(d->_numericLocale.isEmpty())
            setCategoryLocale(MLcNumeric, "");
    }
    if (lcTime != d->_calendarLocale) {
        settingsHaveReallyChanged = true;
        setCategoryLocale(MLcTime, lcTime);
    }
    MLocale::TimeFormat24h timeFormat24h;
    if (lcTimeFormat24h == "24")
        timeFormat24h = MLocale::TwentyFourHourTimeFormat24h;
    else if (lcTimeFormat24h == "12")
        timeFormat24h = MLocale::TwelveHourTimeFormat24h;
    else
        timeFormat24h = MLocale::LocaleDefaultTimeFormat24h;

    if (timeFormat24h != d->_timeFormat24h) {
        settingsHaveReallyChanged = true;
        d->_timeFormat24h = timeFormat24h;
    }
    if (lcCollate != d->_collationLocale) {
        settingsHaveReallyChanged = true;
        setCategoryLocale(MLcCollate, lcCollate);
    }
    if (lcNumeric != d->_numericLocale) {
        settingsHaveReallyChanged = true;
        setCategoryLocale(MLcNumeric, lcNumeric);
    }
    if (lcMonetary != d->_monetaryLocale) {
        settingsHaveReallyChanged = true;
        setCategoryLocale(MLcMonetary, lcMonetary);
    }
    if (lcTelephone != d->_telephoneLocale) {
        settingsHaveReallyChanged = true;
        setCategoryLocale(MLcTelephone, lcTelephone);
    }

    if (settingsHaveReallyChanged) {
        if (this == s_systemDefault) {
            d->insertDirectionTrToQCoreApp();
            d->removeTrFromQCoreApp();
            d->loadTrCatalogs();
            // sends QEvent::LanguageChange to qApp:
            d->insertTrToQCoreApp();
            // Setting the default QLocale is needed to get localized number
            // support in translations via %Ln, %L1, %L2, ...:
            QLocale::setDefault(d->createQLocale(MLcNumeric));
            setApplicationLayoutDirection(this->textDirection());
#ifdef HAVE_ICU
            _defaultLayoutDirection = MIcuConversions::parseLayoutDirectionOption(s_systemDefault->name());
#else
            _defaultLayoutDirection = Qt::LeftToRight;
#endif
        }
        else {
            d->loadTrCatalogs();
        }

        emit settingsChanged();
    }

    d->dropCaches();
}

QString MLocale::formatPhoneNumber( const QString& phoneNumber,
                                    PhoneNumberGrouping grouping ) const
{
    Q_D(const MLocale);

    PhoneNumberGrouping tmpGrouping( grouping );

    // when called with default grouping, use the
    // system setting for the grouping
    if ( tmpGrouping == DefaultPhoneNumberGrouping )
    {
        if ( d->_telephoneLocale.startsWith( QLatin1String( "en_US" ) ) ) {
            tmpGrouping = NorthAmericanPhoneNumberGrouping;
        } else {
            tmpGrouping = NoPhoneNumberGrouping;
	}
    }

    return d->formatPhoneNumber( phoneNumber, tmpGrouping );
}

// when string starts with numbers 2 to 9
QString groupedNormalString( const QString& phoneNumber )
{
  QString result;
  QString remaining( phoneNumber );

    // for remaining number length 1 (also 0) to 3 return number unchanged
    if ( remaining.length() < 4 )
    {
      result.append( remaining );
      return result;
    }
    else if ( remaining.length() < 8 )
    {
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( '-' );
      result.append( remaining );
      return result;
    }
    else if ( remaining.length() < 11 )
    {
      result.append( '(' );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( ") " );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( '-' );
      result.append( remaining );
      return result;
    }
    else
    {
      result.append( remaining );
      return result;
    }
}

// when string starts with number 1
QString groupedOneString( const QString& phoneNumber )
{
    QString result;
    QString remaining( phoneNumber );

    // for remaining number length 1 (also 0) to 3 return number unchanged
    if ( remaining.length() < 2 )
    {
      result.append( remaining );
      return result;
    }
    else if ( remaining.length() < 3 )
    {
      result.append( remaining.left( 1 ) );
      remaining.remove( 0, 1 );
      result.append( " (" );
      result.append( remaining );
      result.append( "  )" );
      return result;
    }
    else if ( remaining.length() < 4 )
    {
      result.append( remaining.left( 1 ) );
      remaining.remove( 0, 1 );
      result.append( " (" );
      result.append( remaining );
      result.append( " )" );
      return result;
    }
    else if ( remaining.length() < 5 )
    {
      result.append( remaining.left( 1 ) );
      remaining.remove( 0, 1 );
      result.append( " (" );
      result.append( remaining );
      result.append( ')' );
      return result;
    }
    else if ( remaining.length() < 8 )
    {
      result.append( remaining.left( 1 ) );
      remaining.remove( 0, 1 );
      result.append( " (" );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( ") " );
      result.append( remaining );
      return result;
    }
    else if ( remaining.length() < 12 )
    {
      result.append( remaining.left( 1 ) );
      remaining.remove( 0, 1 );
      result.append( " (" );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( ") " );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( '-' );
      result.append( remaining );
      return result;
    }
    else
    {
      result.append( remaining );
      return result;
    }
}

// when string starts with numbers 2 to 9
QString groupedInternationalString( const QString& phoneNumber )
{
    QString result;
    QString remaining( phoneNumber );

    // for remaining number length 1 (also 0) to 3 return number unchanged
    if ( remaining.length() < 4 )
    {
      result.append( '(' );
      result.append( remaining );
      result.append( ')' );
      return result;
    }
    else if ( remaining.length() < 7 )
    {
      result.append( '(' );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( ") " );
      result.append( remaining );
      return result;
    }
    else if ( remaining.length() < 11 )
    {
      result.append( '(' );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( ") " );
      result.append( remaining.left( 3 ) );
      remaining.remove( 0, 3 );
      result.append( '-' );
      result.append( remaining );
      return result;
    }
    else
    {
      result.append( remaining );
      return result;
    }
}

QString MLocalePrivate::formatPhoneNumber( const QString& phoneNumber,
    MLocale::PhoneNumberGrouping grouping ) const
{
  // first do sanity check of the input string
  QRegularExpression rx( QRegularExpression::anchoredPattern("\\+?\\d*") );
  QRegularExpressionMatch match = rx.match(phoneNumber);
  if (!match.hasMatch())
  {
    qWarning( "MLocale::formatPhoneNumber: cannot understand number: %s",
	      qPrintable( phoneNumber ) );
    return phoneNumber;
  }

  // 00 is not a valid country calling code in north america
  // -> do not do grouping in this case at all
  if ( ( grouping == MLocale::NorthAmericanPhoneNumberGrouping )
       && phoneNumber.startsWith( QLatin1String( "00" ) ) )
  {
    return phoneNumber;
  }

  QString remaining( phoneNumber );
  QString result;

  // if we find any error, we will return the string unchanged
  //  bool error = false;

  // first extract the country code

  bool foundCountryCodeIndicator = false;

  // valid beginnings for a country code are "+", "00" or "011"
  if ( remaining.startsWith( '+' ) )
  {
    foundCountryCodeIndicator = true;
    result.append( '+' );
    remaining.remove( 0, 1 );
  }
  else if ( remaining.startsWith( QLatin1String( "00" ) ) )
  {
    foundCountryCodeIndicator = true;
    result.append( "00 " );
    remaining.remove( 0, 2 );
  }
  else if ( remaining.startsWith( QLatin1String( "011" ) ) )
  {
    foundCountryCodeIndicator = true;
    result.append( "011 " );
    remaining.remove( 0, 3 );
  }


  // now check for valid country code
  if ( foundCountryCodeIndicator )
  {
    int length = 1;
    QString code;

    code = remaining.left( length );

    if ( isValidCountryCode( code ) )
    {
      result.append( code );
      result.append( ' ' );
      remaining.remove( 0, code.length() );
    }
    else
    {
      length = 2;
      code = remaining.left( length );
      if ( isValidCountryCode( code ) )
      {
	result.append( code );
	result.append( ' ' );
	remaining.remove( 0, code.length() );
      }
      else
      {
	length = 3;
	code = remaining.left( length );
	if ( isValidCountryCode( code ) )
	{
	  result.append( code );
	  result.append( ' ' );
	  remaining.remove( 0, code.length() );
	}
	else
	{
	  // no valid country code -> error -> return string
	  return phoneNumber;
	}
      }
    }
  } // found country code indicator

  // if it exists, the country code is split off now
  if ( grouping != MLocale::NorthAmericanPhoneNumberGrouping )
  {
    result.append( remaining );
    return result;
  }
  else
  {
    // has country code -> do not handle one special.
    if ( foundCountryCodeIndicator )
    {
      result.append( groupedInternationalString( remaining ) );
      return result;
    }
    // 11 is an invalid code, so disable grouping for this case
    else if ( remaining.startsWith( QLatin1String( "11" ) ) )
    {
      result.append( remaining );
      return result;
    }
    else if ( remaining.startsWith( '1' ) )
    {
      result.append( groupedOneString( remaining ) );
      return result;
    }
    else
    {
      result.append( groupedNormalString( remaining ) );
      return result;
    }
  }
}

}
