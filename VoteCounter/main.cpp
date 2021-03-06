#include <LoggingHub.hpp>
#include "VoteCounterShell.hpp"

int
main(int argc, char * argv[])
{
    QApplication app(argc, argv);

    // this is necessary for settings
    app.setOrganizationDomain("thepeoplespeak.com");
    app.setApplicationName("Vote Counter");
    app.setQuitOnLastWindowClosed(true);

    VoteCounterShell mainw;

    mainw.setWindowTitle( app.applicationName() );

    // load the actual UI
    QUiLoader loader;
    QFile file(":/forms/VoteCounter.ui");
    file.open(QFile::ReadOnly);
    QWidget * vote_counter_ui = loader.load(&file);
    file.close();
    mainw.setCentralWidget(  vote_counter_ui );
    QMetaObject::connectSlotsByName( &mainw );
    mainw.loadSettings();

    mainw.showMaximized();
    mainw.raise();

    QArtm::LoggingHub::setup(&mainw);

    qDebug() << qPrintable(app.applicationName()) << "started";
    return app.exec();
}
