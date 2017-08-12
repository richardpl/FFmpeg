#ifndef PREVIEW_H
#define PREVIEW_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

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
    void on_doubleSpinBox_valueChanged(double arg1);

    void on_horizontalSlider_valueChanged(int value);

    void on_pushButton_clicked(bool checked);

private:
    void getSingleFrame(double frameTime, int is_relative, int step_forward);
    Ui::Preview *ui;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterGraph *filter_graph = NULL;
    int video_stream_index = -1;
    int64_t last_pts = AV_NOPTS_VALUE;
    QString videoFileName;
};

#endif // PREVIEW_H
