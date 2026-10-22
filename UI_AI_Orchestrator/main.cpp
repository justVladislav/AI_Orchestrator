#include <AppVersion.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <QIcon>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QVariantMap>

int main(int argc, char* argv[])
{
    // The UI provides custom backgrounds/content items, which require a
    // non-native Qt Quick Controls style on Windows.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setDefaultAlphaBuffer(true);

    // BackendBridge derives its default SQLite path from QStandardPaths::
    // AppDataLocation, which folds in these two names. Setting them
    // explicitly (rather than relying on Qt's argv[0]-derived default)
    // keeps the persisted-database location stable across how the
    // executable happens to be renamed or invoked.
    QCoreApplication::setOrganizationName(QStringLiteral("AIOrchestrator"));
    QCoreApplication::setApplicationName(QStringLiteral("AIOrchestrator"));
    // Not part of the data path (only the two names above are); Settings shows it.
    QCoreApplication::setApplicationVersion(QString::fromLatin1(ai::ui::version::full));

    QGuiApplication app(argc, argv);
    // Runtime window/taskbar icon; the executable-file icon comes from the
    // app_icon.rc resource compiled into the binary.
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/navigation/main_icon.png")));

    // The bridge owns the complete Qt-facing controller graph for this process.
    BackendBridge backendBridge;
    QQmlApplicationEngine engine;

    QVariantMap initialProperties;
    initialProperties.insert(
        QStringLiteral("backend"),
        QVariant::fromValue(&backendBridge));
    engine.setInitialProperties(initialProperties);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("UI_AI_Orchestrator", "Main");


    return app.exec();
}
