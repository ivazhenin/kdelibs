/* This file is part of the KDE libraries
   Copyright (C) 2000 David Faure <faure@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "global.h"

#include <kprotocolinfo.h>
#include <kconfiggroup.h>
#include <klocale.h>
#include <qmimedatabase.h>
#include <QtDBus/QtDBus>
#include <QHash>

// If someone wants the SI-standard prefixes kB/MB/GB/TB, I would recommend
// a hidden kconfig option and getting the code from #57240 into the same
// method, so that all KDE apps use the same unit, instead of letting each app decide.

KIOCORE_EXPORT QString KIO::convertSize( KIO::filesize_t size )
{
    return KLocale::global()->formatByteSize(size);
}

KIOCORE_EXPORT QString KIO::convertSizeFromKiB( KIO::filesize_t kibSize )
{
    return KLocale::global()->formatByteSize(kibSize * 1024);
}

KIOCORE_EXPORT QString KIO::number( KIO::filesize_t size )
{
    char charbuf[256];
    sprintf(charbuf, "%lld", size);
    return QLatin1String(charbuf);
}

KIOCORE_EXPORT unsigned int KIO::calculateRemainingSeconds( KIO::filesize_t totalSize,
                                                        KIO::filesize_t processedSize, KIO::filesize_t speed )
{
  if ( (speed != 0) && (totalSize != 0) )
    return ( totalSize - processedSize ) / speed;
  else
    return 0;
}

KIOCORE_EXPORT QString KIO::convertSeconds( unsigned int seconds )
{
  unsigned int days  = seconds / 86400;
  unsigned int hours = (seconds - (days * 86400)) / 3600;
  unsigned int mins  = (seconds - (days * 86400) - (hours * 3600)) / 60;
  seconds            = (seconds - (days * 86400) - (hours * 3600) - (mins * 60));

  const QTime time(hours, mins, seconds);
  const QString timeStr( KLocale::global()->formatTime(time, true /*with seconds*/, true /*duration*/) );
  if ( days > 0 )
    return i18np("1 day %2", "%1 days %2", days, timeStr);
  else
    return timeStr;
}

#ifndef KDE_NO_DEPRECATED
KIOCORE_EXPORT QTime KIO::calculateRemaining( KIO::filesize_t totalSize, KIO::filesize_t processedSize, KIO::filesize_t speed )
{
  QTime remainingTime;

  if ( speed != 0 ) {
    KIO::filesize_t secs;
    if ( totalSize == 0 ) {
      secs = 0;
    } else {
      secs = ( totalSize - processedSize ) / speed;
    }
    if (secs >= (24*60*60)) // Limit to 23:59:59
       secs = (24*60*60)-1;
    int hr = secs / ( 60 * 60 );
    int mn = ( secs - hr * 60 * 60 ) / 60;
    int sc = ( secs - hr * 60 * 60 - mn * 60 );

    remainingTime.setHMS( hr, mn, sc );
  }

  return remainingTime;
}
#endif

KIOCORE_EXPORT QString KIO::itemsSummaryString(uint items, uint files, uint dirs, KIO::filesize_t size, bool showSize)
{
    if ( files == 0 && dirs == 0 && items == 0 ) {
        return i18np( "%1 Item", "%1 Items", 0 );
    }

    QString summary;
    const QString foldersText = i18np( "1 Folder", "%1 Folders", dirs );
    const QString filesText = i18np( "1 File", "%1 Files", files );
    if ( files > 0 && dirs > 0 ) {
        summary = showSize ?
                  i18nc( "folders, files (size)", "%1, %2 (%3)", foldersText, filesText, KIO::convertSize( size ) ) :
                  i18nc( "folders, files", "%1, %2", foldersText, filesText );
    } else if ( files > 0 ) {
        summary = showSize ? i18nc( "files (size)", "%1 (%2)", filesText, KIO::convertSize( size ) ) : filesText;
    } else if ( dirs > 0 ) {
        summary = foldersText;
    }

    if ( items > dirs + files ) {
        const QString itemsText = i18np( "%1 Item", "%1 Items", items );
        summary = summary.isEmpty() ? itemsText : i18nc( "items: folders, files (size)", "%1: %2", itemsText, summary );
    }

    return summary;
}

KIOCORE_EXPORT QString KIO::encodeFileName( const QString & _str )
{
    QString str( _str );
    str.replace('/', QChar(0x2044)); // "Fraction slash"
    return str;
}

KIOCORE_EXPORT QString KIO::decodeFileName( const QString & _str )
{
    // Nothing to decode. "Fraction slash" is fine in filenames.
    return _str;
}

/***************************************************************
 *
 * Utility functions
 *
 ***************************************************************/

KIO::CacheControl KIO::parseCacheControl(const QString &cacheControl)
{
  QString tmp = cacheControl.toLower();

  if (tmp == "cacheonly")
     return KIO::CC_CacheOnly;
  if (tmp == "cache")
     return KIO::CC_Cache;
  if (tmp == "verify")
     return KIO::CC_Verify;
  if (tmp == "refresh")
     return KIO::CC_Refresh;
  if (tmp == "reload")
     return KIO::CC_Reload;

  qDebug() << "unrecognized Cache control option:"<<cacheControl;
  return KIO::CC_Verify;
}

QString KIO::getCacheControlString(KIO::CacheControl cacheControl)
{
    if (cacheControl == KIO::CC_CacheOnly)
	return "CacheOnly";
    if (cacheControl == KIO::CC_Cache)
	return "Cache";
    if (cacheControl == KIO::CC_Verify)
	return "Verify";
    if (cacheControl == KIO::CC_Refresh)
	return "Refresh";
    if (cacheControl == KIO::CC_Reload)
	return "Reload";
    qDebug() << "unrecognized Cache control enum value:"<<cacheControl;
    return QString();
}

static bool useFavIcons()
{
    // this method will be called quite often, so better not read the config
    // again and again.
    static bool s_useFavIconsChecked = false;
    static bool s_useFavIcons = false;
    if (!s_useFavIconsChecked) {
        s_useFavIconsChecked = true;
        KConfigGroup cg( KSharedConfig::openConfig(), "HTML Settings" );
        s_useFavIcons = cg.readEntry("EnableFavicon", true);
    }
    return s_useFavIcons;
}

QString KIO::favIconForUrl(const QUrl& url)
{
    /* The kded module also caches favicons, for one week, without any way
     * to clean up the cache meanwhile.
     * On the other hand, this QHash will get cleaned up after 5000 request
     * (a selection in konsole of 80 chars generates around 500 requests)
     * or by simply restarting the application (or the whole desktop,
     * more likely, for the case of konqueror or konsole).
     */
    static QHash<QUrl, QString> iconNameCache;
    static int autoClearCache = 0;
    const QString notFound = QLatin1String("NOTFOUND");

    if (url.isLocalFile()
        || !url.scheme().startsWith(QLatin1String("http"))
        || !useFavIcons())
        return QString();

    QString iconNameFromCache = iconNameCache.value(url, notFound);
    if (iconNameFromCache != notFound) {
        if ((++autoClearCache) < 5000) {
            return iconNameFromCache;
        } else {
            iconNameCache.clear();
            autoClearCache = 0;
        }
    }

    QDBusInterface kded( QString::fromLatin1("org.kde.kded5"),
                         QString::fromLatin1("/modules/favicons"),
                         QString::fromLatin1("org.kde.FavIcon") );
    QDBusReply<QString> result = kded.call( QString::fromLatin1("iconForUrl"), url.toString() );
    iconNameCache.insert(url, result.value());
    return result;              // default is QString()
}

QString KIO::iconNameForUrl(const QUrl& url)
{
    QMimeDatabase db;
    const QMimeType mt = db.mimeTypeForUrl(url);
    const QLatin1String unknown("unknown");
    const QString mimeTypeIcon = mt.iconName();
    QString i = mimeTypeIcon;

    // if we don't find an icon, maybe we can use the one for the protocol
    if (i == unknown || i.isEmpty() || mt.isDefault()
        // and for the root of the protocol (e.g. trash:/) the protocol icon has priority over the mimetype icon
        || url.path().length() <= 1)
    {
        i = favIconForUrl(url); // maybe there is a favicon?

        if (i.isEmpty())
            i = KProtocolInfo::icon(url.scheme());

        // root of protocol: if we found nothing, revert to mimeTypeIcon (which is usually "folder")
        if (url.path().length() <= 1 && (i == unknown || i.isEmpty()))
            i = mimeTypeIcon;
    }
    return !i.isEmpty() ? i : unknown;
}