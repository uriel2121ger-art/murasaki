#include "buildinfo.h"
#include "constants.h"
#include "daemonclient.h"
#include "librarymodel.h"
#include "shortcut/shortcutcatalog.h"
#include "overlayclient.h"
#include "propertymodel.h"
#include "libraryfiltermodel.h"
#include "steamlocator.h"
#include "surfaceclient.h"
#include "systemcontrol.h"
#include "usenowflow.h"
#include "workshopstatus.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTimer>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

#include <QFile>
#include <QLibraryInfo>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

std::atomic<int> g_qmlProblems{0};
QtMessageHandler g_previousHandler = nullptr;

// --smoke-test fails on any warning that comes from QML: loading errors, binding errors, console.warn().
void countingHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        const QString category = QString::fromLatin1(context.category ? context.category : "");
        static const QRegularExpression qmlLocation(u"(qrc:/|file:).*\\.qml:"_s);
        if (category.startsWith(u"qt.qml"_s) || category.startsWith(u"qt.quick"_s) || category.startsWith(u"qml"_s)
            || category == u"js"_s || message.contains(qmlLocation))
            ++g_qmlProblems;
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// The KDE desktop style (qqc2-desktop-style) is optional: without it Quick Controls falls back to its default.
bool hasDesktopStyle()
{
    const QQmlEngine probe;
    const QStringList paths = probe.importPathList();
    return std::any_of(paths.cbegin(), paths.cend(),
                       [](const QString &dir) { return QFile::exists(dir + u"/org/kde/desktop/qmldir"_s); });
}

// Kirigami installs its translations as locale/<lang>/LC_MESSAGES/libkirigami6_qt.qm in the data dirs ("es_MX", then "es").
QString kirigamiTranslation()
{
    for (QString lang : QLocale::system().uiLanguages()) {
        lang.replace(u'-', u'_');
        const QStringList candidates{lang, lang.section(u'_', 0, 0)};
        for (const QString &candidate : candidates) {
            const QString file = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                        u"locale/%1/LC_MESSAGES/libkirigami6_qt.qm"_s.arg(candidate));
            if (!file.isEmpty())
                return file;
        }
    }
    return {};
}

} // namespace

int main(int argc, char *argv[])
{
    QtWebEngineQuick::initialize(); // before the application object exists

    // Kirigami's desktop style draws through QStyle, so this needs QApplication rather than QGuiApplication.
    QApplication app(argc, argv);
    QApplication::setApplicationName(u"murasaki"_s);
    QApplication::setApplicationVersion(murasaki::versionString());
    QApplication::setOrganizationName(u"uriel2121ger-art"_s);
    QApplication::setOrganizationDomain(u"github.com/uriel2121ger-art"_s);
    QApplication::setDesktopFileName(u"io.github.uriel2121ger_art.Murasaki"_s);

    // The UI text is English in the sources; a translation matching the system language is embedded (":/i18n").
    QTranslator translator;
    if (translator.load(QLocale::system(), u"murasaki"_s, u"_"_s, u":/i18n"_s))
        QApplication::installTranslator(&translator);
    // Kirigami's own text ("Close Sidebar", ...) and Qt's (dialog buttons) come from the system's translations.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale::system(), u"qt"_s, u"_"_s, QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);
    QTranslator kirigamiTranslator;
    if (const QString kirigamiQm = kirigamiTranslation(); !kirigamiQm.isEmpty() && kirigamiTranslator.load(kirigamiQm))
        QApplication::installTranslator(&kirigamiTranslator);

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Murasaki wallpaper manager"_s);
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeOption(u"smoke-test"_s, u"Load the UI for two seconds and exit 1 on any QML warning."_s);
    parser.addOption(smokeOption);
    QCommandLineOption openItemOption(u"open-first-item"_s, u"Open the page of the first library item once the library has loaded."_s);
    QCommandLineOption screenshotOption(u"screenshot"_s, u"Save a screenshot of the window to <file> after 5 seconds and exit."_s, u"file"_s);
    QCommandLineOption openIdOption(u"open-item"_s, u"Open the page of the library item <id> once the library has loaded."_s, u"id"_s);
    openIdOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(openIdOption);
    QCommandLineOption pageOption(u"page"_s, u"Start on the page <name>: library, playlists, workshop, screens or settings."_s, u"name"_s);
    pageOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(pageOption);
    QCommandLineOption kindOption(u"kind"_s, u"Start with the library filtered to the kind <kind> (video, web, scene or pet)."_s, u"kind"_s);
    kindOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(kindOption);
    QCommandLineOption sizeOption(u"window-size"_s, u"Open the window with the size <WIDTHxHEIGHT> (for screenshots)."_s, u"size"_s);
    sizeOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(sizeOption);
    openItemOption.setFlags(QCommandLineOption::HiddenFromHelp);
    screenshotOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(openItemOption);
    parser.addOption(screenshotOption);
    parser.process(app);
    const bool smoke = parser.isSet(smokeOption);

    if (smoke)
        g_previousHandler = qInstallMessageHandler(countingHandler);

    if (hasDesktopStyle())
        QQuickStyle::setStyle(u"org.kde.desktop"_s);

    murasaki::DaemonClient daemon;
    murasaki::SurfaceClient surface;
    murasaki::OverlayClient overlay;
    murasaki::SystemControl system;
    murasaki::LibraryModel library(&daemon);
    murasaki::UseNowFlow useNow(&daemon, &library);
    murasaki::LibraryFilterModel filter;
    filter.setSourceModel(&library);
    // The state of an item also comes from what SteamCMD downloaded for Murasaki (one more Workshop root).
    QStringList workshopRoots = murasaki::SteamLocator(murasaki::SteamLocator::defaultRootCandidates()).workshopRoots();
    workshopRoots.append(murasaki::SteamLocator::steamCmdWorkshopRoot(murasaki::SteamLocator::steamCmdInstallDir()));
    murasaki::WorkshopStatus workshop(workshopRoots, murasaki::kWallpaperEngineAppId);
    murasaki::shortcut::ShortcutCatalog shortcutCatalog;
    shortcutCatalog.refresh();

    if (parser.isSet(kindOption))
        filter.setKindFilter(parser.value(kindOption));

    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Daemon", &daemon);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Surface", &surface);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Overlay", &overlay);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Library", &library);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "LibraryFilter", &filter);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "Workshop", &workshop);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "UseNow", &useNow);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "SystemControl", &system);
    qmlRegisterSingletonInstance("Murasaki.App", 1, 0, "ShortcutCatalog", &shortcutCatalog);
    qmlRegisterType<murasaki::PropertyModel>("Murasaki.App", 1, 0, "PropertyModel");

    QQmlApplicationEngine engine;
    bool loadFailed = false;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [&loadFailed] { loadFailed = true; },
        Qt::QueuedConnection);
    engine.loadFromModule("Murasaki.App", u"Main"_s);
    if (engine.rootObjects().isEmpty())
        return 1;

    if (parser.isSet(sizeOption)) {
        const QStringList size = parser.value(sizeOption).split(u'x');
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (window && size.size() == 2 && size[0].toInt() > 0 && size[1].toInt() > 0)
            window->resize(size[0].toInt(), size[1].toInt());
    }

    daemon.start();
    surface.start();
    overlay.start();

    QObject *root = engine.rootObjects().constFirst();
    if (parser.isSet(pageOption)) {
        const QString name = parser.value(pageOption);
        QTimer::singleShot(0, root, [root, name] { QMetaObject::invokeMethod(root, "showPage", Q_ARG(QVariant, name)); });
    }
    // The smoke test always exercises the item page too, once the library had a second to load.
    if (parser.isSet(openIdOption)) {
        const QString id = parser.value(openIdOption);
        QTimer::singleShot(1000, root, [root, id] { QMetaObject::invokeMethod(root, "openItem", Q_ARG(QVariant, id)); });
    } else if (smoke || parser.isSet(openItemOption))
        QTimer::singleShot(1000, root, [root] { QMetaObject::invokeMethod(root, "openFirstItem"); });
    if (smoke)
        QTimer::singleShot(2300, root, [root] { QMetaObject::invokeMethod(root, "smokeTour"); });

    if (parser.isSet(screenshotOption)) {
        const QString file = parser.value(screenshotOption);
        QTimer::singleShot(5000, &app, [&app, root, file] {
            auto *window = qobject_cast<QQuickWindow *>(root);
            const bool saved = window && window->grabWindow().save(file);
            if (!saved)
                std::fprintf(stderr, "screenshot: could not save %s\n", qPrintable(file));
            app.exit(saved ? 0 : 1);
        });
        return app.exec();
    }

    if (smoke) {
        QTimer::singleShot(3500, &app, &QCoreApplication::quit);
        app.exec();
        std::printf("smoke-test: daemon %s (api %d), library count %d, qml problems %d\n",
                    daemon.available() ? "available" : "unavailable", daemon.apiVersion(), int(library.rowCount()), g_qmlProblems.load());
        // A smoke test that saw no data proves little: it needs a daemon and a non-empty library unless allowed.
        const bool allowEmpty = qEnvironmentVariableIntValue("MURASAKI_SMOKE_ALLOW_EMPTY") != 0;
        const bool noData = !daemon.available() || library.rowCount() == 0;
        if (noData && !allowEmpty)
            std::fprintf(stderr, "smoke-test: no daemon or empty library (set MURASAKI_SMOKE_ALLOW_EMPTY=1 to accept)\n");
        return (loadFailed || g_qmlProblems > 0 || (noData && !allowEmpty)) ? 1 : 0;
    }
    return app.exec();
}
