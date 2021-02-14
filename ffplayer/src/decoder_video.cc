//
// Created by boyan on 2021/2/14.
//

#include "decoder_video.h"

int VideoDecoder::GetVideoFrame(AVFrame *frame) {
    auto got_picture = DecodeFrame(frame, nullptr);
    if (got_picture < 0) {
        return -1;
    }
    return got_picture;
}

const char *VideoDecoder::debug_label() {
    return "video_decoder";
}

int VideoDecoder::DecodeThread() {
    if (*decode_params->format_ctx == nullptr) {
        return -1;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    int ret;
    AVRational tb = decode_params->stream()->time_base;
    AVRational frame_rate = av_guess_frame_rate(*decode_params->format_ctx, decode_params->stream(), nullptr);
    for (;;) {
        ret = GetVideoFrame(frame);
        if (ret < 0) {
            break;
        }
        if (!ret) {
            continue;
        }
        auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
        auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);

        ret = render->PushFrame(frame, pts, duration, pkt_serial);
        av_frame_unref(frame);
        if (ret < 0) {
            break;
        }
    }
    av_frame_free(&frame);
    return 0;
}

VideoDecoder::VideoDecoder(unique_ptr_d<AVCodecContext> codecContext, std::unique_ptr<DecodeParams> decodeParams,
                           std::shared_ptr<VideoRender> render)
        : Decoder(std::move(codecContext), std::move(decodeParams), std::move(render)) {
    Start();
}
