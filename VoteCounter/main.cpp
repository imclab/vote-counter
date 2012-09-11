#include <LoggingHub.hpp>

QSettings * g_projectSettings = 0;

QSettings& projectSettings() {
    return *g_projectSettings;
}

int
main(int argc, char * argv[])
{
    QApplication app(argc, argv);

    // this is necessary for settings
    app.setOrganizationDomain("thepeoplespeak.com");
    app.setApplicationName("Vote Counter");
    app.setQuitOnLastWindowClosed(true);

    g_projectSettings = new QSettings;

    QMainWindow mainw;
    mainw.setWindowTitle( app.applicationName() );
    mainw.showMaximized();
    mainw.raise();

    QArtm::LoggingHub::setup(&mainw);

    qDebug() << "Program started";
    return app.exec();
}