// vim: expandtab sw=4 ts=4
/*  This file is part of the KDE libraries
 *  Copyright (C) 1999 David Faure <faure@kde.org>
 *  Copyright (C) 2000 Waldo Bastian <bastian@kde.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License version 2 as published by the Free Software Foundation;
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 **/

#include "kded.h"
#include "kdedadaptor.h"
#include "kded_version.h"

#include <kcrash.h>

#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include <QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTimer>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>

#include <kdbusservice.h>
#include <qapplication.h>
#include <kconfiggroup.h>
#include <kdirwatch.h>
#include <kservicetypetrader.h>
#include <ktoolinvocation.h>


#define KDED_EXENAME "kded5"

#define MODULES_PATH "/modules/"

Kded *Kded::_self = 0;

static bool checkStamps = true;
static bool delayedCheck = false;
static int HostnamePollInterval;
static bool bCheckSycoca;
static bool bCheckUpdates;
static bool bCheckHostname;

#ifdef Q_DBUS_EXPORT
extern Q_DBUS_EXPORT void qDBusAddSpyHook(void (*)(const QDBusMessage&));
#else
extern QDBUS_EXPORT void qDBusAddSpyHook(void (*)(const QDBusMessage&));
#endif

static void runBuildSycoca(QObject *callBackObj=0, const char *callBackSlot=0)
{
   const QString exe = QStandardPaths::findExecutable(KBUILDSYCOCA_EXENAME);
   Q_ASSERT(!exe.isEmpty());
   QStringList args;
    if (QStandardPaths::isTestModeEnabled()) {
        args.append("--testmode");
    }
   if(checkStamps)
      args.append("--checkstamps");
   if(delayedCheck)
      args.append("--nocheckfiles");
   else
      checkStamps = false; // useful only during kded startup
   if (callBackObj)
   {
      QVariantList argList;
      argList << exe << args << QStringList() << QString();
      KToolInvocation::ensureKdeinitRunning();
      QDBusInterface klauncher(QStringLiteral("org.kde.klauncher5"), QStringLiteral("/KLauncher"), QStringLiteral("org.kde.KLauncher"), QDBusConnection::sessionBus());
      klauncher.callWithCallback("kdeinit_exec_wait", argList, callBackObj, callBackSlot);
   }
   else
   {
      KToolInvocation::kdeinitExecWait( exe, args );
   }
}

static void runKonfUpdate()
{
   KToolInvocation::kdeinitExecWait( "kconf_update", QStringList(), 0, 0, "0" /*no startup notification*/ );
}

static void runDontChangeHostname(const QByteArray &oldName, const QByteArray &newName)
{
   QStringList args;
   args.append(QFile::decodeName(oldName));
   args.append(QFile::decodeName(newName));
   KToolInvocation::kdeinitExecWait( "kdontchangethehostname", args );
}

Kded::Kded()
    : m_needDelayedCheck(false)
{
  _self = this;

  m_serviceWatcher = new QDBusServiceWatcher(this);
  m_serviceWatcher->setConnection(QDBusConnection::sessionBus());
  m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
  QObject::connect(m_serviceWatcher, SIGNAL(serviceUnregistered(QString)),
                   this, SLOT(slotApplicationRemoved(QString)));

  new KBuildsycocaAdaptor(this);
  new KdedAdaptor(this);

  QDBusConnection session = QDBusConnection::sessionBus();
  session.registerObject("/kbuildsycoca", this);
  session.registerObject("/kded", this);

  qDBusAddSpyHook(messageFilter);

  m_pTimer = new QTimer(this);
  m_pTimer->setSingleShot( true );
  connect(m_pTimer, SIGNAL(timeout()), this, SLOT(recreate()));

  m_pDirWatch = 0;

  m_recreateCount = 0;
  m_recreateBusy = false;
}

Kded::~Kded()
{
  _self = 0;
  m_pTimer->stop();
  delete m_pTimer;
  delete m_pDirWatch;

  for (QHash<QString,KDEDModule*>::iterator
           it(m_modules.begin()), itEnd(m_modules.end());
       it != itEnd; ++it)
  {
      KDEDModule* module(it.value());

      // first disconnect otherwise slotKDEDModuleRemoved() is called
      // and changes m_modules while we're iterating over it
      disconnect(module, SIGNAL(moduleDeleted(KDEDModule*)),
                 this, SLOT(slotKDEDModuleRemoved(KDEDModule*)));

      delete module;
  }
}

// on-demand module loading
// this function is called by the D-Bus message processing function before
// calls are delivered to objects
void Kded::messageFilter(const QDBusMessage &message)
{
  // This happens when kded goes down and some modules try to clean up.
  if (!self())
     return;

  if (message.type() != QDBusMessage::MethodCallMessage)
     return;

  QString obj = message.path();
  if (!obj.startsWith(MODULES_PATH))
     return;

  // Remove the <MODULES_PATH> part
  obj = obj.mid(strlen(MODULES_PATH));
  if (obj == "ksycoca")
     return; // Ignore this one.

  // Remove the part after the modules name
  int index = obj.indexOf('/');
  if (index!=-1) {
      obj = obj.left(index);
  }

  if (self()->m_dontLoad.value(obj, 0))
     return;

  KDEDModule *module = self()->loadModule(obj, true);
  if (!module) {
      qWarning() << "Failed to load module for " << obj;
  }
  Q_UNUSED(module);
}

static int phaseForModule(const KService::Ptr& service)
{
    const QVariant phasev = service->property("X-KDE-Kded-phase", QVariant::Int );
    return phasev.isValid() ? phasev.toInt() : 2;
}

void Kded::initModules()
{
    m_dontLoad.clear();
    bool kde_running = !qgetenv( "KDE_FULL_SESSION" ).isEmpty();
    if (kde_running) {
        // not the same user like the one running the session (most likely we're run via sudo or something)
        const QByteArray sessionUID = qgetenv( "KDE_SESSION_UID" );
        if( !sessionUID.isEmpty() && uid_t( sessionUID.toInt() ) != getuid())
            kde_running = false;

        // not the same kde version as the current desktop
        const QByteArray kdeSession = qgetenv("KDE_SESSION_VERSION");
        if (kdeSession.toInt() != KDED_VERSION_MAJOR)
            kde_running = false;
    }

    // There will be a "phase 2" only if we're in the KDE startup.
    // If kded is restarted by its crashhandled or by hand,
    // then there will be no second phase autoload, so load
    // these modules now, if in a KDE session.
    const bool loadPhase2Now = (kde_running && qgetenv("KDED_STARTED_BY_KDEINIT").toInt() == 0);

     // Preload kded modules.
     const KService::List kdedModules = KServiceTypeTrader::self()->query("KDEDModule");
     for(KService::List::ConstIterator it = kdedModules.begin(); it != kdedModules.end(); ++it)
     {
         KService::Ptr service = *it;
         // Should the service load on startup?
         const bool autoload = isModuleAutoloaded(service);

         // see ksmserver's README for description of the phases
         bool prevent_autoload = false;
         switch( phaseForModule(service) )
         {
             case 0: // always autoload
                 break;
             case 1: // autoload only in KDE
                 if (!kde_running) {
                     prevent_autoload = true;
                 }
                 break;
             case 2: // autoload delayed, only in KDE
             default:
                 if (!loadPhase2Now) {
                     prevent_autoload = true;
                 }
                 break;
         }

        // Load the module if necessary and allowed
         if (autoload && !prevent_autoload) {
            if (!loadModule(service, false)) {
                continue;
            }
         }

         // Remember if the module is allowed to load on demand
         bool loadOnDemand = isModuleLoadedOnDemand(service);
         if (!loadOnDemand)
            noDemandLoad(service->desktopEntryName());

         // In case of reloading the configuration it is possible for a module
         // to run even if it is now allowed to. Stop it then.
         if (!loadOnDemand && !autoload)
            unloadModule(service->desktopEntryName().toLatin1());
     }
}

void Kded::loadSecondPhase()
{
    //qDebug() << "Loading second phase autoload";
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    KService::List kdedModules = KServiceTypeTrader::self()->query("KDEDModule");
    for(KService::List::ConstIterator it = kdedModules.constBegin(); it != kdedModules.constEnd(); ++it) {
        const KService::Ptr service = *it;
        const bool autoload = isModuleAutoloaded(service);
        if (autoload && phaseForModule(service) == 2) {
            //qDebug() << "2nd phase: loading" << service->desktopEntryName();
            loadModule(service, false);
        }
    }
}

void Kded::noDemandLoad(const QString &obj)
{
  m_dontLoad.insert(obj.toLatin1(), this);
}

void Kded::setModuleAutoloading(const QString &obj, bool autoload)
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    // Ensure the service exists.
    KService::Ptr service = KService::serviceByDesktopPath("kded/"+obj+".desktop");
    if (!service)
        return;
    KConfigGroup cg(config, QString("Module-%1").arg(service->desktopEntryName()));
    cg.writeEntry("autoload", autoload);
    cg.sync();
}

bool Kded::isModuleAutoloaded(const QString &obj) const
{
    KService::Ptr s = KService::serviceByDesktopPath("kded/"+obj+".desktop");
    if (!s)
        return false;
    return isModuleAutoloaded(s);
}

bool Kded::isModuleAutoloaded(const KService::Ptr &module) const
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    bool autoload = module->property("X-KDE-Kded-autoload", QVariant::Bool).toBool();
    KConfigGroup cg(config, QString("Module-%1").arg(module->desktopEntryName()));
    autoload = cg.readEntry("autoload", autoload);
    return autoload;
}

bool Kded::isModuleLoadedOnDemand(const QString &obj) const
{
    KService::Ptr s = KService::serviceByDesktopPath("kded/"+obj+".desktop");
    if (!s)
        return false;
    return isModuleLoadedOnDemand(s);
}

bool Kded::isModuleLoadedOnDemand(const KService::Ptr &module) const
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    bool loadOnDemand = true;
    QVariant p = module->property("X-KDE-Kded-load-on-demand", QVariant::Bool);
    if (p.isValid() && (p.toBool() == false))
        loadOnDemand = false;
    return loadOnDemand;
}

KDEDModule *Kded::loadModule(const QString &obj, bool onDemand)
{
  // Make sure this method is only called with valid module names.
  Q_ASSERT(obj.indexOf('/')==-1);

  KDEDModule *module = m_modules.value(obj, 0);
  if (module)
     return module;
  KService::Ptr s = KService::serviceByDesktopPath("kded/"+obj+".desktop");
  return loadModule(s, onDemand);
}

KDEDModule *Kded::loadModule(const KService::Ptr& s, bool onDemand)
{
    if (s && !s->library().isEmpty())
    {
        QString obj = s->desktopEntryName();
        KDEDModule *oldModule = m_modules.value(obj, 0);
        if (oldModule)
            return oldModule;

        if (onDemand)
        {
            QVariant p = s->property("X-KDE-Kded-load-on-demand", QVariant::Bool);
            if (p.isValid() && (p.toBool() == false))
            {
                noDemandLoad(s->desktopEntryName());
                return 0;
            }
        }

        KDEDModule *module = 0;
        QString libname = "kded_"+s->library();
        KPluginLoader loader(libname);

        KPluginFactory *factory = loader.factory();
        if (!factory) {
            qWarning() << "Could not load library" << libname << ". ["
                       << loader.errorString() << "]";
        } else {
            // create the module
            module = factory->create<KDEDModule>(this);
        }
        if (module) {
            module->setModuleName(obj);
            m_modules.insert(obj, module);
            //m_libs.insert(obj, lib);
            connect(module, SIGNAL(moduleDeleted(KDEDModule*)), SLOT(slotKDEDModuleRemoved(KDEDModule*)));
            qDebug() << "Successfully loaded module" << obj;
            return module;
        } else {
            //qDebug() << "Could not load module" << obj;
            //loader.unload();
        }
    }
    return 0;
}

bool Kded::unloadModule(const QString &obj)
{
  KDEDModule *module = m_modules.value(obj, 0);
  if (!module)
     return false;
  //qDebug() << "Unloading module" << obj;
  m_modules.remove(obj);
  delete module;
  return true;
}

QStringList Kded::loadedModules()
{
    return m_modules.keys();
}

void Kded::slotKDEDModuleRemoved(KDEDModule *module)
{
  m_modules.remove(module->moduleName());
  //KLibrary *lib = m_libs.take(module->moduleName());
  //if (lib)
  //   lib->unload();
}

void Kded::slotApplicationRemoved(const QString &name)
{
#if 0 // see kdedmodule.cpp (KDED_OBJECTS)
  foreach( KDEDModule* module, m_modules )
  {
     module->removeAll(appId);
  }
#endif
  m_serviceWatcher->removeWatchedService(name);
  const QList<qlonglong> windowIds = m_windowIdList.value(name);
  for( QList<qlonglong>::ConstIterator it = windowIds.begin();
       it != windowIds.end(); ++it)
  {
      qlonglong windowId = *it;
      m_globalWindowIdList.remove(windowId);
      foreach( KDEDModule* module, m_modules )
      {
          emit module->windowUnregistered(windowId);
      }
  }
  m_windowIdList.remove(name);
}

void Kded::updateDirWatch()
{
  if (!bCheckUpdates) return;

  delete m_pDirWatch;
  m_pDirWatch = new KDirWatch;

  QObject::connect( m_pDirWatch, SIGNAL(dirty(QString)),
           this, SLOT(update(QString)));
  QObject::connect( m_pDirWatch, SIGNAL(created(QString)),
           this, SLOT(update(QString)));
  QObject::connect( m_pDirWatch, SIGNAL(deleted(QString)),
           this, SLOT(dirDeleted(QString)));

  // For each resource
  for( QStringList::ConstIterator it = m_allResourceDirs.constBegin();
       it != m_allResourceDirs.constEnd();
       ++it )
  {
     readDirectory( *it );
  }
}

void Kded::updateResourceList()
{
  KSycoca::clearCaches();

  if (!bCheckUpdates) return;

  if (delayedCheck) return;

  const QStringList dirs = KSycoca::self()->allResourceDirs();
  // For each resource
  for( QStringList::ConstIterator it = dirs.begin();
       it != dirs.end();
       ++it )
  {
     if (!m_allResourceDirs.contains(*it))
     {
        m_allResourceDirs.append(*it);
        readDirectory(*it);
     }
  }
}

void Kded::recreate()
{
   recreate(false);
}

void Kded::runDelayedCheck()
{
   if( m_needDelayedCheck )
      recreate(false);
   m_needDelayedCheck = false;
}

void Kded::recreate(bool initial)
{
   m_recreateBusy = true;
   // Using KLauncher here is difficult since we might not have a
   // database

   if (!initial)
   {
      updateDirWatch(); // Update tree first, to be sure to miss nothing.
      runBuildSycoca(this, SLOT(recreateDone()));
   }
   else
   {
      if(!delayedCheck)
         updateDirWatch(); // this would search all the directories
      if (bCheckSycoca)
         runBuildSycoca();
      recreateDone();
      if(delayedCheck)
      {
         // do a proper ksycoca check after a delay
         QTimer::singleShot( 60000, this, SLOT(runDelayedCheck()));
         m_needDelayedCheck = true;
         delayedCheck = false;
      }
      else
         m_needDelayedCheck = false;
   }
}

void Kded::recreateDone()
{
   updateResourceList();

   for(; m_recreateCount; m_recreateCount--)
   {
      QDBusMessage msg = m_recreateRequests.takeFirst();
      QDBusConnection::sessionBus().send(msg.createReply());
   }
   m_recreateBusy = false;

   // Did a new request come in while building?
   if (!m_recreateRequests.isEmpty())
   {
      m_pTimer->start(2000);
      m_recreateCount = m_recreateRequests.count();
   } else {
       initModules();
   }
}

void Kded::dirDeleted(const QString& path)
{
  update(path);
}

void Kded::update(const QString& )
{
  if (!m_recreateBusy)
  {
    m_pTimer->start( 10000 );
  }
}

void Kded::recreate(const QDBusMessage &msg)
{
   if (!m_recreateBusy)
   {
      if (m_recreateRequests.isEmpty())
      {
         m_pTimer->start(0);
         m_recreateCount = 0;
      }
      m_recreateCount++;
   }
   msg.setDelayedReply(true);
   m_recreateRequests.append(msg);
   return;
}


void Kded::readDirectory( const QString& _path )
{
  QString path( _path );
  if ( !path.endsWith( '/' ) )
    path += '/';

  if ( m_pDirWatch->contains( path ) ) // Already seen this one?
     return;

  m_pDirWatch->addDir(path,KDirWatch::WatchFiles|KDirWatch::WatchSubDirs);          // add watch on this dir
  return; // KDirWatch now claims to also support recursive watching
#if 0
  QDir d( _path, QString(), QDir::Unsorted, QDir::Readable | QDir::Executable | QDir::Dirs | QDir::Hidden );
  // set QDir ...


  //************************************************************************
  //                           Setting dirs
  //************************************************************************

  if ( !d.exists() )                            // exists&isdir?
  {
    //qDebug() << "Does not exist:" << _path;
    return;                             // return false
  }

  // Note: If some directory is gone, dirwatch will delete it from the list.

  //************************************************************************
  //                               Reading
  //************************************************************************
  QString file;
  unsigned int i;                           // counter and string length.
  unsigned int count = d.count();
  for( i = 0; i < count; i++ )                        // check all entries
  {
     if (d[i] == "." || d[i] == ".." || d[i] == "magic")
       continue;                          // discard those ".", "..", "magic"...

     file = path;                           // set full path
     file += d[i];                          // and add the file name.

     readDirectory( file );      // yes, dive into it.
  }
#endif
}

/*
bool Kded::isWindowRegistered(long windowId) const
{
  return m_globalWindowIdList.contains(windowId);

}
*/

void Kded::registerWindowId(qlonglong windowId, const QString &sender)
{
  if (!m_windowIdList.contains(sender)) {
      m_serviceWatcher->addWatchedService(sender);
  }

  m_globalWindowIdList.insert(windowId);
  QList<qlonglong> windowIds = m_windowIdList.value(sender);
  windowIds.append(windowId);
  m_windowIdList.insert(sender, windowIds);

  foreach( KDEDModule* module, m_modules )
  {
     //qDebug() << module->moduleName();
     emit module->windowRegistered(windowId);
  }
}

void Kded::unregisterWindowId(qlonglong windowId, const QString &sender)
{
  m_globalWindowIdList.remove(windowId);
  QList<qlonglong> windowIds = m_windowIdList.value(sender);
  if (!windowIds.isEmpty())
  {
     windowIds.removeAll(windowId);
     if (windowIds.isEmpty()) {
        m_serviceWatcher->removeWatchedService(sender);
        m_windowIdList.remove(sender);
     } else {
        m_windowIdList.insert(sender, windowIds);
     }
  }

  foreach( KDEDModule* module, m_modules )
  {
    //qDebug() << module->moduleName();
    emit module->windowUnregistered(windowId);
  }
}


static void sighandler(int /*sig*/)
{
    if (qApp)
       qApp->quit();
}

KUpdateD::KUpdateD()
{
    m_pDirWatch = new KDirWatch(this);
    m_pTimer = new QTimer(this);
    m_pTimer->setSingleShot( true );
    connect(m_pTimer, SIGNAL(timeout()), this, SLOT(runKonfUpdate()));
    QObject::connect( m_pDirWatch, SIGNAL(dirty(QString)),
           this, SLOT(slotNewUpdateFile(QString)));

    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "kconf_update", QStandardPaths::LocateDirectory);
    for( QStringList::ConstIterator it = dirs.begin();
         it != dirs.end();
         ++it )
    {
       QString path = *it;
       if (path[path.length()-1] != '/')
          path += '/';

       if (!m_pDirWatch->contains(path))
          m_pDirWatch->addDir(path,KDirWatch::WatchFiles);
    }
}

KUpdateD::~KUpdateD()
{
}

void KUpdateD::runKonfUpdate()
{
    ::runKonfUpdate();
}

void KUpdateD::slotNewUpdateFile(const QString& dirty)
{
    Q_UNUSED(dirty);
    //qDebug() << dirty;
    m_pTimer->start( 500 );
}

KHostnameD::KHostnameD(int pollInterval)
{
    m_Timer.start(pollInterval); // repetitive timer (not single-shot)
    connect(&m_Timer, SIGNAL(timeout()), this, SLOT(checkHostname()));
    checkHostname();
}

KHostnameD::~KHostnameD()
{
    // Empty
}

void KHostnameD::checkHostname()
{
    char buf[1024+1];
    if (gethostname(buf, 1024) != 0)
       return;
    buf[sizeof(buf)-1] = '\0';

    if (m_hostname.isEmpty())
    {
       m_hostname = buf;
       return;
    }

    if (m_hostname == buf)
       return;

    QByteArray newHostname = buf;

    runDontChangeHostname(m_hostname, newHostname);
    m_hostname = newHostname;
}


KBuildsycocaAdaptor::KBuildsycocaAdaptor(QObject *parent)
   : QDBusAbstractAdaptor(parent)
{
}

void KBuildsycocaAdaptor::recreate(const QDBusMessage &msg)
{
   Kded::self()->recreate(msg);
}

bool KBuildsycocaAdaptor::isTestModeEnabled()
{
    return QStandardPaths::isTestModeEnabled();
}

void KBuildsycocaAdaptor::enableTestMode()
{
    QStandardPaths::enableTestMode(true);
}

extern "C" Q_DECL_EXPORT int kdemain(int argc, char *argv[])
{
    //options.add("check", qi18n("Check Sycoca database only once"));

     // WABA: Make sure not to enable session management.
     putenv(qstrdup("SESSION_MANAGER="));

    // Parse command line before checking D-Bus
    KSharedConfig::Ptr config = KSharedConfig::openConfig("kdedrc");

     KConfigGroup cg(config, "General");
     if (argc > 1 && QByteArray(argv[1]) == "--check") {
        // KDBusService not wanted here.
        QCoreApplication app(argc, argv);
        checkStamps = cg.readEntry("CheckFileStamps", true);
        runBuildSycoca();
        runKonfUpdate();
        return 0;
     }

     QApplication app(argc, argv);
     app.setQuitOnLastWindowClosed(false);
     app.setApplicationName("kded5");
     app.setOrganizationDomain("kde.org");
     app.setApplicationDisplayName("KDE Daemon");

     KDBusService service(KDBusService::Unique);

     HostnamePollInterval = cg.readEntry("HostnamePollInterval", 5000);
     bCheckSycoca = cg.readEntry("CheckSycoca", true);
     bCheckUpdates = cg.readEntry("CheckUpdates", true);
     bCheckHostname = cg.readEntry("CheckHostname", true);
     checkStamps = cg.readEntry("CheckFileStamps", true);
     delayedCheck = cg.readEntry("DelayedCheck", false);

#ifndef _WIN32_WCE
     signal(SIGTERM, sighandler);
#endif
     signal(SIGHUP, sighandler);

     KCrash::setFlags(KCrash::AutoRestart);

     Kded *kded = new Kded();

     kded->recreate(true); // initial

     if (bCheckUpdates)
         (void) new KUpdateD; // Watch for updates

//NOTE: We are going to change how KDE starts and this certanly doesn't fit on the new design.
#ifdef Q_OS_LINUX
    // Tell KSplash that KDED has started
    QDBusMessage ksplashProgressMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KSplash"),
                                                                        QStringLiteral("/KSplash"),
                                                                        QStringLiteral("org.kde.KSplash"),
                                                                        QStringLiteral("setStage"));
    ksplashProgressMessage.setArguments(QList<QVariant>() << QStringLiteral("kded"));
    QDBusConnection::sessionBus().asyncCall(ksplashProgressMessage);
#endif

     runKonfUpdate(); // Run it once.

#ifdef Q_OS_LINUX
     ksplashProgressMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KSplash"),
                                                             QStringLiteral("/KSplash"),
                                                             QStringLiteral("org.kde.KSplash"),
                                                             QStringLiteral("setStage"));
     ksplashProgressMessage.setArguments(QList<QVariant>() << QStringLiteral("confupdate"));
     QDBusConnection::sessionBus().asyncCall(ksplashProgressMessage);
#endif

     //if (bCheckHostname)
     //    (void) new KHostnameD(HostnamePollInterval); // Watch for hostname changes

     int result = app.exec(); // keep running

     delete kded;

     return result;
}
