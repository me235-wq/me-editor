#include "ui/MainWindow.h"
#include <QApplication>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QPalette>
#include <QSurfaceFormat>

static void applyDarkTheme(QApplication& app) {
    app.setStyle("Fusion");   // identical look on Windows and Linux
    QPalette p;
    const QColor bg(32, 32, 35), base(24, 24, 26), text(228, 228, 232), accent(74, 144, 226);
    p.setColor(QPalette::Window, bg);          p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);          p.setColor(QPalette::AlternateBase, bg);
    p.setColor(QPalette::Text, text);          p.setColor(QPalette::Button, QColor(46, 46, 50));
    p.setColor(QPalette::ButtonText, text);    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);   p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 115));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 115));
    app.setPalette(p);
}

int main(int argc, char** argv) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("VideoEditor");
    app.setOrganizationName("VideoEditor");
    applyDarkTheme(app);

    // GPU preview needs OpenGL 3.3; otherwise fall back to the CPU preview.
    QOpenGLContext probe;
    probe.setFormat(QSurfaceFormat::defaultFormat());
    const bool gl = probe.create() && probe.format().version() >= qMakePair(3, 3) && qEnvironmentVariableIsEmpty("VE_SOFTWARE_PREVIEW");

    ve::MainWindow w(gl);
    w.show();
    if (argc > 1 && QFileInfo::exists(argv[1])) w.openProject(argv[1]);
    return app.exec();
}
