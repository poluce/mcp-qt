#include <QApplication>
#include "ChatWindow.h"
#include "AgentController.h"
#include "ToolManager.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    ToolManager toolManager;
    AgentController controller(&toolManager);

    ChatWindow w(&controller, &toolManager);
    w.show();

    return a.exec();
}
