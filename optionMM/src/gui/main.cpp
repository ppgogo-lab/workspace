#include "gui/trader_main_window.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("optionMM");
    QApplication::setApplicationName("TraderDashboard");

    std::string endpoint = "127.0.0.1:50051";
    if (argc > 1) endpoint = argv[1];

    omm::gui::TraderMainWindow window(endpoint);
    if (!window.initialize_session()) return 0;
    window.resize(1800, 1100);
    window.show();
    return app.exec();
}
