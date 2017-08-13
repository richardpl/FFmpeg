#include <mpv/qthelper.hpp>

#include <QFileDialog>

#include "preview.h"
#include "ui_preview.h"

Preview::Preview(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Preview)
{
    ui->setupUi(this);
}

static void wakeup(void *ctx)
{
    // This callback is invoked from any mpv thread (but possibly also
    // recursively from a thread that is calling the mpv API). Just notify
    // the Qt GUI thread to wake up (so that it can process events with
    // mpv_wait_event()), and return as quickly as possible.
    Preview *preview = (Preview *)ctx;
    emit preview->mpv_events();
}

void Preview::handle_mpv_event(mpv_event *event)
{
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = (mpv_event_property *)event->data;
        if (strcmp(prop->name, "time-pos") == 0) {
            if (prop->format == MPV_FORMAT_DOUBLE) {
                double time = *(double *)prop->data;
                ui->doubleSpinBox->blockSignals(true);
                ui->doubleSpinBox->setValue(time);
                ui->doubleSpinBox->blockSignals(false);
            } else if (prop->format == MPV_FORMAT_NONE) {
                // The property is unavailable, which probably means playback
                // was stopped.
            }
        }
        if (strcmp(prop->name, "percent-pos") == 0) {
            if (prop->format == MPV_FORMAT_DOUBLE) {
                double percent = *(double *)prop->data;
                ui->horizontalSlider->blockSignals(true);
                ui->horizontalSlider->setValue(percent * 10);
                ui->horizontalSlider->blockSignals(false);
            } else if (prop->format == MPV_FORMAT_NONE) {
                // The property is unavailable, which probably means playback
                // was stopped.
            }
        }
        break;
    }
    }
}

void Preview::my_mpv_events()
{
    // Process all events, until the event queue is empty.
    while (mpv) {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        handle_mpv_event(event);
    }
}

void Preview::previewScript()
{
    this->show();

    videoFileName = QFileDialog::getOpenFileName(this,tr("Open Video file"), "", tr("All files (*)"));

    mpv = mpv_create();
    if (!mpv)
        return;
    int64_t winId = ui->displayArea->winId();
    mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &winId);
    mpv_set_option_string(mpv, "vo", "direct3d");
    mpv_set_option_string(mpv, "pause", "");
    mpv_set_option_string(mpv, "keep-open", "yes");
    //mpv_set_option_string(mpv, "video-unscaled", "yes");
    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "percent-pos", MPV_FORMAT_DOUBLE);

    connect(this, SIGNAL(mpv_events()), this, SLOT(my_mpv_events()), Qt::QueuedConnection);
    mpv_set_wakeup_callback(mpv, wakeup, this);

    if (mpv_initialize(mpv) < 0)
        return;

    const QByteArray c_filename = videoFileName.toUtf8();
    const char *args[] = {"loadfile", c_filename.data(), NULL};

    mpv_command_async(mpv, 0, args);

    const char *vfargs[] = {"vf", "set", filter_graph_str.toUtf8().constData(), NULL};

    mpv_command(mpv, vfargs);
}

Preview::~Preview()
{
    if (mpv)
        mpv_terminate_destroy(mpv);
    delete ui;
}

void Preview::on_doubleSpinBox_valueChanged(double frameTime)
{
    const char *seekargs[] = {"seek", QString("%1").number(frameTime).toUtf8().constData(), "absolute+exact", NULL};
    mpv_command_async(mpv, 0, seekargs);
}

void Preview::on_horizontalSlider_valueChanged(int value)
{
    const char *seekargs[] = {"seek", QString("%1").number(double(value) / 10).toUtf8().constData(), "absolute-percent+exact", NULL};
    mpv_command_async(mpv, 0, seekargs);
}

void Preview::on_pushButton_clicked(bool checked)
{
    const char *stepargs[] = {"frame-step", NULL};

    mpv_command_async(mpv, 0, stepargs);
}

void Preview::on_pushButton_2_clicked(bool checked)
{
    const char *stepargs[] = {"frame-back-step", NULL};

    mpv_command_async(mpv, 0, stepargs);
}

void Preview::on_window_scale_valueChanged(const QString &arg1)
{
    const char *zoomargs[] = {"video-zoom", QString("%1").toUtf8().constData(), NULL};
    mpv_command_async(mpv, 0, zoomargs);
}
