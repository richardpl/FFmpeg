extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
}

#include "mainwindow.h"
#include <QApplication>

using namespace std;
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;

    av_register_all();
    avfilter_register_all();
    w.show();

    return a.exec();
}
