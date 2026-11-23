/* bftvbench — mede a decodificação de um TS 1seg no FOSTON (vídeo H.264 + áudio HE-AAC). */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv)
{
    AVFormatContext *fc = NULL;
    if(getenv("BFTV_DEBUG")) av_log_set_level(AV_LOG_DEBUG);
    if(argc < 2 || avformat_open_input(&fc, argv[1], NULL, NULL) < 0) return 1;
    avformat_find_stream_info(fc, NULL);
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, vi, NULL, 0);
    AVCodecContext *vc = NULL, *ac = NULL;
    if(vi >= 0) { const AVCodec *c = avcodec_find_decoder(fc->streams[vi]->codecpar->codec_id); vc = avcodec_alloc_context3(c); avcodec_parameters_to_context(vc, fc->streams[vi]->codecpar);
        if(getenv("SKIPLF")) { vc->skip_loop_filter = AVDISCARD_ALL; vc->flags2 |= AV_CODEC_FLAG2_FAST; }
        avcodec_open2(vc, c, NULL); }
    if(getenv("NOAUDIO")) ai = -1;
    if(ai >= 0 && !getenv("FDK")) { const AVCodec *c = avcodec_find_decoder(fc->streams[ai]->codecpar->codec_id); ac = avcodec_alloc_context3(c); avcodec_parameters_to_context(ac, fc->streams[ai]->codecpar); avcodec_open2(ac, c, NULL); }
    int scale = argc > 2;
    struct SwsContext *sws = NULL;
    static uint8_t rgb[800 * 480 * 2];
    AVPacket *p = av_packet_alloc(); AVFrame *f = av_frame_alloc();
    HANDLE_AACDECODER fdk = getenv("FDK") ? aacDecoder_Open(TT_MP4_LOAS, 1) : NULL;
    static INT_PCM pcm[8192];
    long nv = 0, na = 0; double t0 = now(), c0 = cpu(), tv = 0, ta = 0, ts = 0;
    while(av_read_frame(fc, p) >= 0) {
        if(fdk && p->stream_index == ai) {
            double a = cpu();
            UCHAR *b = p->data; UINT sz = (UINT)p->size, valid = sz;
            while(valid > 0) {
                if(aacDecoder_Fill(fdk, &b, &sz, &valid) != AAC_DEC_OK) break;
                b = p->data + (p->size - valid); sz = valid;
                while(aacDecoder_DecodeFrame(fdk, pcm, 8192, 0) == AAC_DEC_OK) na++;
            }
            ta += cpu() - a;
            av_packet_unref(p);
            continue;
        }
        AVCodecContext *cc = p->stream_index == vi ? vc : p->stream_index == ai ? ac : NULL;
        if(cc) {
            double a = cpu();
            avcodec_send_packet(cc, p);
            while(avcodec_receive_frame(cc, f) == 0) {
                if(cc == vc) {
                    nv++;
                    if(scale) {
                        double b = cpu();
                        if(!sws) sws = sws_getContext(f->width, f->height, f->format, 800, 450, AV_PIX_FMT_RGB565, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        uint8_t *d[1] = { rgb }; int ls[1] = { 1600 };
                        sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, d, ls);
                        ts += cpu() - b;
                    }
                } else na++;
            }
            if(cc == vc) tv += cpu() - a; else ta += cpu() - a;
        }
        av_packet_unref(p);
    }
    double wall = now() - t0, c = cpu() - c0;
    printf("vídeo %ld quadros %dx%d, áudio %ld quadros; parede %.2f s, CPU %.2f s (vídeo %.2f, escala %.2f, áudio %.2f)\n",
           nv, vc ? vc->width : 0, vc ? vc->height : 0, na, wall, c, tv - ts, ts, ta);
    if(fdk) { CStreamInfo *si = aacDecoder_GetStreamInfo(fdk); if(si) printf("fdk: %d Hz, %d canais, AOT %d, quadro %d\n", si->sampleRate, si->numChannels, si->aot, si->frameSize); }
    return 0;
}
