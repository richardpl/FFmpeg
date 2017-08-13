#ifndef PREVIEW_H
#define PREVIEW_H

#include <mpv/client.h>
#include <QWidget>

namespace Ui {
class Preview;
}

class Preview : public QWidget
{
    Q_OBJECT

public:
    explicit Preview(QWidget *parent = 0);
    void previewScript(void);
    ~Preview();
    QString filter_graph_str;

private slots:
    void my_mpv_events();

    void on_doubleSpinBox_valueChanged(double arg1);

    void on_horizontalSlider_valueChanged(int value);

    void on_pushButton_clicked(bool checked);

    void on_pushButton_2_clicked(bool checked);

    void on_window_scale_valueChanged(double arg1);

    void on_window_scale_valueChanged(const QString &arg1);

signals:
    void mpv_events();

private:
    void handle_mpv_event(mpv_event *event);
    Ui::Preview *ui;

    mpv_handle *mpv = NULL;
    QString videoFileName;
};

#endif // PREVIEW_H
