#include "h264.h"
#include "../../client/_client.h"
#include "../../util.h"
#include <string.h>
#include <assert.h>
#include <libswscale/swscale.h>


ENetPacket *allo_video_write_h264(allo_media_track *track, allopixel *pixels, int32_t pixels_wide, int32_t pixels_high)
{

    if (track->info.video.encoder.codec == NULL) {
        track->info.video.encoder.codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (track->info.video.encoder.codec == NULL) {
            fprintf(stderr, "No encoder\n");
            return NULL;
        }
        track->info.video.encoder.context = avcodec_alloc_context3(track->info.video.encoder.codec);
        track->info.video.picture = av_frame_alloc();
        
        track->info.video.encoder.context->width = track->info.video.width;
        track->info.video.encoder.context->height = track->info.video.height;
        track->info.video.encoder.context->time_base = av_d2q(1.0, 10);
        track->info.video.encoder.context->pix_fmt = AV_PIX_FMT_YUV420P;
        
        int ret = avcodec_open2(track->info.video.encoder.context, track->info.video.encoder.codec, NULL);
        if (ret != 0) {
            fprintf(stderr, "avcodec_open2 return %d when opening encoder\n", ret);
            return NULL;
        }
    }
    
    int ret = 0;
    
    AVFrame *frame = av_frame_alloc();
    frame->format = track->info.video.encoder.context->pix_fmt;
    frame->width = track->info.video.encoder.context->width;
    frame->height = track->info.video.encoder.context->height;
    frame->pts = ++track->info.video.framenr;
    ret = av_frame_get_buffer(frame, 0);
    assert(ret == 0);
    ret = av_frame_make_writable(frame);
    assert(ret == 0);
    
    // Convert from pixels RGBA into frame->data YUV
    struct SwsContext *sws_ctx = sws_getContext(
        pixels_wide,
        pixels_high,
        AV_PIX_FMT_RGBA,
        frame->width,
        frame->height,
        frame->format,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    uint8_t *srcData[3] = { (uint8_t*)pixels, NULL, NULL };
    int srcStrides[3] = { pixels_wide * sizeof(allopixel), 0, 0 };
    int height = sws_scale(sws_ctx, srcData, srcStrides, 0, pixels_high, frame->data, frame->linesize);
    
    
    ret = avcodec_send_frame(track->info.video.encoder.context, frame);
    assert(ret == 0);
    
    AVPacket *avpacket = av_packet_alloc();
    // TODO: Might need to loop and receive multiple packets (because encoded frames can come out of order)
    ret = avcodec_receive_packet(track->info.video.encoder.context, avpacket);
    
    ENetPacket *packet = NULL;
    if (ret >= 0) {
        packet = enet_packet_create(NULL, avpacket->size + 4, 0);
        memcpy(packet->data + 4, avpacket->data, avpacket->size);
    } else {
        fprintf(stderr, "alloclient: Something went wrong in the h264 encoding: %d\n", ret);
    }
    
    av_packet_free(&avpacket);
    av_frame_free(&frame);
    
    return packet;
}

allopixel *allo_video_parse_h264(alloclient *client, allo_media_track *track, unsigned char *data, size_t length, int32_t *pixels_wide, int32_t *pixels_high)
{
    if (track->info.video.decoder.codec == NULL) {
        track->info.video.decoder.codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (track->info.video.decoder.codec == NULL) {
            fprintf(stderr, "No decoder\n");
        }
        track->info.video.decoder.context = avcodec_alloc_context3(track->info.video.decoder.codec);
        track->info.video.picture = av_frame_alloc();
        
        track->info.video.decoder.context->width = track->info.video.width;
        track->info.video.decoder.context->height = track->info.video.height;
        int ret = avcodec_open2(track->info.video.decoder.context, track->info.video.decoder.codec, NULL);
        if (ret != 0) {
            fprintf(stderr, "avcodec_open2 return %d when opening decoder\n", ret);
        }
    }
    
    AVPacket *avpacket = av_packet_alloc();
    avpacket->size = length;
    avpacket->data = data;
    avpacket->pts = avpacket->dts = ++track->info.video.framenr;
    int ret = avcodec_send_packet(track->info.video.decoder.context, avpacket);
    
    if (ret != 0) {
        fprintf(stderr, "avcodec_send_packet return %d\n", ret);
        av_packet_free(&avpacket);
        return NULL;
    }
    ret = avcodec_receive_frame(track->info.video.decoder.context, track->info.video.picture);
    if (ret != 0) {
        fprintf(stderr, "avcodec_receive_frame return %d\n", ret);
        av_packet_free(&avpacket);
        return NULL;
    }
    
    *pixels_wide = track->info.video.picture->width;
    *pixels_high = track->info.video.picture->height;
    
    AVFrame *frame = track->info.video.picture;
    // Convert from frame->data YUV into pixels RGBA
    struct SwsContext *sws_ctx = sws_getContext(
                *pixels_wide,
                *pixels_high,
                AV_PIX_FMT_YUV420P,
                frame->width,
                frame->height,
                AV_PIX_FMT_RGBA,
                SWS_BILINEAR, NULL, NULL, NULL
                );
    
    
    
    allopixel *pixels = (allopixel*)malloc(track->info.video.picture->width * track->info.video.picture->height * sizeof(allopixel));
    uint8_t *dstData[1] = { (uint8_t*)pixels };
    int dstStrides[1] = { (*pixels_high) * 4 };
    
    int height = sws_scale(
       sws_ctx,
       frame->data,
       frame->linesize,
       0,
       frame->height,
       dstData,
       dstStrides
   );

    
//                yuv420p_to_rgb32(
//                    track->info.video.picture->data[0],
//                    track->info.video.picture->data[1],
//                    track->info.video.picture->data[2],
//                    (uint8_t*)pixels,
//                    track->info.video.picture->width,
//                    track->info.video.picture->height
//                );
    av_packet_free(&avpacket);
    return pixels;
}
