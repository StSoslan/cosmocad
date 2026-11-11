// Оконное приложение CosmoCad.
#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QPixmap>
#include <filesystem>
#include <iostream>

#include "AppContext.h"
#include "MainWindow.h"
#include "ScenePainter.h"
#include "cosmocad/core/Version.h"

using namespace cosmocad;

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QString::fromUtf8(core::kAppName));
    QCoreApplication::setApplicationVersion(QString::fromUtf8(core::kAppVersion));

    std::filesystem::path executableDirectory =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdU16String());
    auto created = app::AppContext::create(app::findResourceDirectory(executableDirectory));
    if (!created) {
        QMessageBox::critical(nullptr, QString::fromUtf8(core::kAppName),
                              QString::fromStdString(created.error().message));
        std::cerr << created.error().message << "\n";
        return 2;
    }
    auto context = created.take();
    context->loadPlugins(executableDirectory);

    ui::MainWindow window(*context);

    // Режим самопроверки: --smoke <файл.png> [--pdf <файл.pdf>] — открыть пример, проверить
    // оформление, сохранить снимок окна (и PDF) и выйти. Используется в автоматических тестах.
    QStringList arguments = QCoreApplication::arguments();
    int smokeIndex = arguments.indexOf(QStringLiteral("--smoke"));

    if (arguments.size() >= 2 && smokeIndex < 0 && !arguments.at(1).startsWith('-'))
        window.openProject(std::filesystem::path(arguments.at(1).toStdU16String()));
    else
        window.newProjectWithDemo();

    if (smokeIndex >= 0 && smokeIndex + 1 < arguments.size()) {
        window.show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();  // дать окну разложить панели
        window.view()->fitAll();
        int violations = window.runCheck();
        QCoreApplication::processEvents();
        bool saved = window.grab().save(arguments.at(smokeIndex + 1));
        int pdfIndex = arguments.indexOf(QStringLiteral("--pdf"));
        if (pdfIndex >= 0 && pdfIndex + 1 < arguments.size())
            saved = saved && ui::exportPdf(window.view()->currentScene(), arguments.at(pdfIndex + 1));
        std::cout << "smoke: violations=" << violations << " screenshot=" << (saved ? "ok" : "failed") << "\n";
        return saved ? 0 : 1;
    }

    window.show();
    return application.exec();
}
