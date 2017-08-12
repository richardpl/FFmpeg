extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

#include <QFileDialog>

#include "preview.h"
#include "ui_preview.h"

Preview::Preview(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Preview)
{
    ui->setupUi(this);
}

void Preview::getSingleFrame(double frameTime, int is_relative, int step_forward)
{
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    AVPacket packet;
    int got_frame = 0;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t framePts;
    int ret;

    if (!step_forward) {
        if (!is_relative) {
            framePts = frameTime * AV_TIME_BASE;
            ui->horizontalSlider->blockSignals(1);
            ui->horizontalSlider->setValue(double(framePts) / fmt_ctx->duration * ui->horizontalSlider->maximum());
            ui->horizontalSlider->blockSignals(0);
        } else {
            framePts = frameTime * fmt_ctx->duration;
            ui->doubleSpinBox->blockSignals(1);
            ui->doubleSpinBox->setValue(framePts / double(AV_TIME_BASE));
            ui->doubleSpinBox->blockSignals(0);
        }
        avformat_seek_file(fmt_ctx, -1, INT64_MIN, framePts, framePts, 0);
    }

    while (!got_frame) {
        if ((ret = av_read_frame(fmt_ctx, &packet)) < 0)
            break;

        if (packet.stream_index == video_stream_index) {
            ret = avcodec_send_packet(dec_ctx, &packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0 && !got_frame) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    goto end;
                }

                if (ret >= 0) {
                    frame->pts = frame->best_effort_timestamp;

                    /* push the decoded frame into the filtergraph */
                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                        break;
                    }

                    /* pull filtered frames from the filtergraph */
                    while (!got_frame) {
                        ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        }
                        if (ret < 0) {
                            av_log(NULL, AV_LOG_ERROR, "Error filtering frame\n");
                            goto end;
                        }
                        QImage frameImage((const uchar *)filt_frame->data[0], filt_frame->width, filt_frame->height, QImage::Format_RGB32);
                        QPixmap framePixmap = QPixmap::fromImage(frameImage).copy();
                        ui->label->setPixmap(framePixmap);
                        pts = av_rescale_q(packet.pts, fmt_ctx->streams[video_stream_index]->time_base, AV_TIME_BASE_Q);
                        if (pts >= framePts || step_forward)
                            got_frame = 1;
                        av_frame_unref(filt_frame);
                    }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(&packet);
    }

    if (step_forward) {
        ui->horizontalSlider->blockSignals(1);
        ui->horizontalSlider->setValue(double(pts) / fmt_ctx->duration * ui->horizontalSlider->maximum());
        ui->horizontalSlider->blockSignals(0);
        ui->doubleSpinBox->blockSignals(1);
        ui->doubleSpinBox->setValue(pts / double(AV_TIME_BASE));
        ui->doubleSpinBox->blockSignals(0);
    }

end:
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
}

void Preview::previewScript()
{
    AVCodec *dec;
    int ret;
    char args[512];

    this->show();

    videoFileName = QFileDialog::getOpenFileName(this,tr("Open Video file"), "", tr("All files (*)"));

    if ((ret = avformat_open_input(&fmt_ctx, videoFileName.toLocal8Bit().constData(), NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return;
    }

    /* select the video stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return;
    }
    video_stream_index = ret;

    /* create decoding context */
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return;
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);
    av_opt_set_int(dec_ctx, "refcounted_frames", 1, 0);

    /* init the video decoder */
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return;
    }

    AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE };

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto enda;
    }

    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto enda;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto enda;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
        goto enda;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_graph_str.toLocal8Bit().constData(),
                                        &inputs, &outputs, NULL)) < 0)
        goto enda;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto enda;

enda:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (ret < 0)
        return;

    Preview::getSingleFrame(0, 0, 0);
}

Preview::~Preview()
{
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    delete ui;
}

void Preview::on_doubleSpinBox_valueChanged(double frameTime)
{
    Preview::getSingleFrame(frameTime, 0, 0);
}

void Preview::on_horizontalSlider_valueChanged(int value)
{
    Preview::getSingleFrame(double(value) / ui->horizontalSlider->maximum(), 1, 0);
}

void Preview::on_pushButton_clicked(bool checked)
{
    Preview::getSingleFrame(0, 0, 1);
}
