#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    // QApplication 是整个 Qt GUI 程序的事件循环入口。
    QApplication a(argc, argv);
    a.setStyle(QStringLiteral("Fusion"));
    a.setApplicationName(QStringLiteral("医学图像软件"));

    // 主窗口统一负责各个硬件线程与页面状态的调度。
    MainWindow w;
    w.show();

    // 进入主事件循环，直到用户退出程序。
    return a.exec();
}
