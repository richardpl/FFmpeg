#ifndef MAINWINDOW_H
#define MAINWINDOW_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
}

#include <QMainWindow>

#include "preview.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();
    Ui::MainWindow *ui;

private slots:
    void on_actionExit_triggered();

    void on_actionOpen_script_triggered();

    void on_actionSave_script_triggered();

    void on_actionNew_script_triggered();

    void on_actionPreview_triggered();

private:
    Preview preview;
};

#endif // MAINWINDOW_H
