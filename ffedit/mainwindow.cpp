extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavutil/opt.h>
}

#include <QFileDialog>
#include <QMessageBox>

#include "mainwindow.h"
#include "preview.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_actionExit_triggered()
{
    this->close();
}

void MainWindow::on_actionOpen_script_triggered()
{
    QString fileName = QFileDialog::getOpenFileName(this,tr("Open script"), "", tr("All files (*)"));

    QFile scriptFile(fileName);
    bool loadSucess = scriptFile.open(QIODevice::ReadOnly|QIODevice::Text);
    if (!loadSucess) {
        QMessageBox::critical(this,
                           QString::fromUtf8("File open error"),
                           QString::fromUtf8("Failed to open the file %1.").arg(fileName));
        return;
    }

    QByteArray utf8Script = scriptFile.readAll();
    QString scriptText = QString::fromUtf8(utf8Script);
    ui->plainTextEdit->setPlainText(scriptText);
}

void MainWindow::on_actionSave_script_triggered()
{
    QString fileName = QFileDialog::getSaveFileName(this,tr("Save script"), "", tr("All files (*)"));

    QFile scriptFile(fileName);
    bool loadSucess = scriptFile.open(QIODevice::WriteOnly|QIODevice::Text);
    if (!loadSucess) {
        QMessageBox::critical(this,
                           QString::fromUtf8("File save error"),
                           QString::fromUtf8("Failed to save the file %1.").arg(fileName));
        return;
    }

    QByteArray utf8Script = ui->plainTextEdit->toPlainText().toUtf8();
    scriptFile.write(utf8Script);
}

void MainWindow::on_actionNew_script_triggered()
{
    ui->plainTextEdit->clear();
}

void MainWindow::on_actionPreview_triggered()
{
    preview.filter_graph_str = ui->plainTextEdit->toPlainText();

    preview.previewScript();
}
