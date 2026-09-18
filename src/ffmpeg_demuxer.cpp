#include "mkvsplit/ffmpeg_demuxer.hpp"
#include <iostream>
#include <vector>
#include <map>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/timestamp.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
}

namespace mkvsplit {

static std::string AvErr2Str(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

bool FFmpegDemuxer::ReadMetadata(const std::filesystem::path& file_path, MKVMetadata& out_metadata, std::string* error_msg) {
    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext* fmt_ctx = nullptr;
    std::string path_str = file_path.string();

    int ret = avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not open input file: " + AvErr2Str(ret);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not find stream info: " + AvErr2Str(ret);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    out_metadata.file_path = file_path;
    out_metadata.duration_sec = (fmt_ctx->duration != AV_NOPTS_VALUE) ? (static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE) : 0.0;
    
    std::error_code ec;
    out_metadata.file_size_bytes = std::filesystem::file_size(file_path, ec);

    // Title tag
    AVDictionaryEntry* tag = av_dict_get(fmt_ctx->metadata, "title", nullptr, 0);
    if (tag && tag->value) {
        out_metadata.title = tag->value;
    } else {
        out_metadata.title = file_path.stem().string();
    }

    // Inspect streams
    int video_stream_idx = -1;
    out_metadata.tracks.clear();
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* st = fmt_ctx->streams[i];
        AVCodecParameters* codecpar = st->codecpar;

        TrackInfo track;
        track.index = static_cast<int>(i);
        
        const char* codec_name = avcodec_get_name(codecpar->codec_id);
        track.codec_name = codec_name ? codec_name : "unknown";

        AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
        if (lang && lang->value) {
            track.language = lang->value;
        } else {
            track.language = "und";
        }

        AVDictionaryEntry* tr_title = av_dict_get(st->metadata, "title", nullptr, 0);
        if (!tr_title) {
            tr_title = av_dict_get(st->metadata, "name", nullptr, 0);
        }
        if (tr_title && tr_title->value) {
            track.title = tr_title->value;
        }

        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                track.media_type = "video";
                track.width = codecpar->width;
                track.height = codecpar->height;
                if (video_stream_idx < 0) video_stream_idx = static_cast<int>(i);
                break;
            case AVMEDIA_TYPE_AUDIO:
                track.media_type = "audio";
                track.sample_rate = codecpar->sample_rate;
                track.channels = codecpar->ch_layout.nb_channels;
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                track.media_type = "subtitle";
                break;
            default:
                track.media_type = "other";
                break;
        }

        out_metadata.tracks.push_back(track);
    }

    // Extract Chapters
    out_metadata.chapters.clear();
    for (unsigned int i = 0; i < fmt_ctx->nb_chapters; i++) {
        AVChapter* ch = fmt_ctx->chapters[i];
        ChapterInfo cinfo;
        cinfo.id = static_cast<int>(i);
        cinfo.start_time_sec = static_cast<double>(ch->start) * av_q2d(ch->time_base);
        cinfo.end_time_sec = static_cast<double>(ch->end) * av_q2d(ch->time_base);

        AVDictionaryEntry* ctitle = av_dict_get(ch->metadata, "title", nullptr, 0);
        if (ctitle && ctitle->value) {
            cinfo.title = ctitle->value;
        } else {
            cinfo.title = "Chapter " + std::to_string(i + 1);
        }

        out_metadata.chapters.push_back(cinfo);
    }

    // Clear keyframes index for instant metadata inspection
    out_metadata.keyframes_sec.clear();

    avformat_close_input(&fmt_ctx);
    return true;
}

std::vector<double> FFmpegDemuxer::DetectSilenceGaps(const std::filesystem::path& input_path,
                                                     const GapDetectionConfig& gap_config,
                                                     std::string* error_msg) {
    std::vector<double> gap_timestamps;
    AVFormatContext* fmt_ctx = nullptr;
    std::string path_str = input_path.string();

    int ret = avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not open file for gap detection: " + AvErr2Str(ret);
        return gap_timestamps;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not find stream info: " + AvErr2Str(ret);
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    int audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (audio_stream_idx < 0) {
        if (error_msg) *error_msg = "No audio stream found for silence gap detection";
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    AVStream* ast = fmt_ctx->streams[audio_stream_idx];
    const AVCodec* decoder = avcodec_find_decoder(ast->codecpar->codec_id);
    if (!decoder) {
        if (error_msg) *error_msg = "Audio decoder not found";
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codec_ctx, ast->codecpar);
    ret = avcodec_open2(codec_ctx, decoder, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not open audio decoder: " + AvErr2Str(ret);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    bool silence_active = false;
    double silence_start_sec = 0.0;
    double last_frame_sec = 0.0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != audio_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            continue;
        }

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            double frame_sec = 0.0;
            if (frame->pts != AV_NOPTS_VALUE) {
                frame_sec = static_cast<double>(frame->pts) * av_q2d(ast->time_base);
            } else {
                frame_sec = last_frame_sec + (static_cast<double>(frame->nb_samples) / codec_ctx->sample_rate);
            }
            last_frame_sec = frame_sec;

            // Calculate RMS audio volume
            double sum_sq = 0.0;
            int total_samples = frame->nb_samples * codec_ctx->ch_layout.nb_channels;
            if (total_samples > 0) {
                if (frame->format == AV_SAMPLE_FMT_FLT || frame->format == AV_SAMPLE_FMT_FLTP) {
                    float* data = reinterpret_cast<float*>(frame->data[0]);
                    for (int s = 0; s < total_samples; s++) {
                        double sample = static_cast<double>(data[s]);
                        sum_sq += sample * sample;
                    }
                } else if (frame->format == AV_SAMPLE_FMT_S16 || frame->format == AV_SAMPLE_FMT_S16P) {
                    int16_t* data = reinterpret_cast<int16_t*>(frame->data[0]);
                    for (int s = 0; s < total_samples; s++) {
                        double sample = static_cast<double>(data[s]) / 32768.0;
                        sum_sq += sample * sample;
                    }
                } else if (frame->format == AV_SAMPLE_FMT_S32 || frame->format == AV_SAMPLE_FMT_S32P) {
                    int32_t* data = reinterpret_cast<int32_t*>(frame->data[0]);
                    for (int s = 0; s < total_samples; s++) {
                        double sample = static_cast<double>(data[s]) / 2147483648.0;
                        sum_sq += sample * sample;
                    }
                }
            }

            double rms = (total_samples > 0) ? std::sqrt(sum_sq / total_samples) : 0.0;
            double db = (rms > 1e-7) ? (20.0 * std::log10(rms)) : -100.0;

            if (db < gap_config.noise_threshold_db) {
                if (!silence_active) {
                    silence_active = true;
                    silence_start_sec = frame_sec;
                }
            } else {
                if (silence_active) {
                    double dur = frame_sec - silence_start_sec;
                    if (dur >= gap_config.min_gap_duration_sec) {
                        double gap_mid = (silence_start_sec + frame_sec) / 2.0;
                        gap_timestamps.push_back(gap_mid);
                    }
                    silence_active = false;
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (silence_active) {
        double dur = last_frame_sec - silence_start_sec;
        if (dur >= gap_config.min_gap_duration_sec) {
            gap_timestamps.push_back((silence_start_sec + last_frame_sec) / 2.0);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return gap_timestamps;
}

std::vector<double> FFmpegDemuxer::DetectGaps(const std::filesystem::path& input_path,
                                         const GapDetectionConfig& gap_config,
                                         std::string* error_msg) {
    if (gap_config.type == GapType::AudioSilence) {
        return DetectSilenceGaps(input_path, gap_config, error_msg);
    } else if (gap_config.type == GapType::BlackFrame) {
        return DetectBlackGaps(input_path, gap_config, error_msg);
    } else { // Combined
        std::vector<double> black_gaps = DetectBlackGaps(input_path, gap_config, error_msg);
        std::vector<double> silence_gaps = DetectSilenceGaps(input_path, gap_config, error_msg);
        
        std::vector<double> combined;
        for (double bg : black_gaps) {
            for (double sg : silence_gaps) {
                if (std::abs(bg - sg) <= 1.5) {
                    combined.push_back((bg + sg) / 2.0);
                    break;
                }
            }
        }
        return combined.empty() ? black_gaps : combined;
    }
}

std::vector<double> FFmpegDemuxer::DetectBlackGaps(const std::filesystem::path& input_path,
                                                   const GapDetectionConfig& gap_config,
                                                   std::string* error_msg) {
    std::vector<double> gap_timestamps;
    AVFormatContext* fmt_ctx = nullptr;
    std::string path_str = input_path.string();

    int ret = avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not open file for black gap detection: " + AvErr2Str(ret);
        return gap_timestamps;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not find stream info: " + AvErr2Str(ret);
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_idx < 0) {
        if (error_msg) *error_msg = "No video stream found for black frame gap detection";
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    AVStream* vst = fmt_ctx->streams[video_stream_idx];
    const AVCodec* decoder = avcodec_find_decoder(vst->codecpar->codec_id);
    if (!decoder) {
        if (error_msg) *error_msg = "Video decoder not found";
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(codec_ctx, vst->codecpar);
    ret = avcodec_open2(codec_ctx, decoder, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not open video decoder: " + AvErr2Str(ret);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return gap_timestamps;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    double file_duration = (fmt_ctx->duration != AV_NOPTS_VALUE) ? (static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE) : 0.0;
    int max_y = static_cast<int>(std::round(gap_config.black_pixel_threshold * 255.0));
    int num_eps = gap_config.expected_episodes;

    // Helper lambda to scan a time range for black gaps
    auto ScanWindow = [&](double window_start, double window_end) {
        if (window_start > 0.0) {
            int64_t seek_pts = static_cast<int64_t>(window_start / av_q2d(vst->time_base));
            av_seek_frame(fmt_ctx, video_stream_idx, seek_pts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codec_ctx);
        }

        bool black_active = false;
        double black_start_sec = 0.0;
        double last_frame_sec = window_start;
        int frame_count = 0;

        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index != video_stream_idx) {
                av_packet_unref(pkt);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx, pkt);
            if (ret < 0) {
                av_packet_unref(pkt);
                continue;
            }

            bool stop_window = false;
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                frame_count++;
                double frame_sec = 0.0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    frame_sec = static_cast<double>(frame->pts) * av_q2d(vst->time_base);
                } else {
                    frame_sec = last_frame_sec + 0.033;
                }
                last_frame_sec = frame_sec;

                if (window_end > 0.0 && frame_sec > window_end) {
                    stop_window = true;
                    break;
                }

                // Sample 1 out of every 2 frames for 2x decoding speedup
                if (frame_count % 2 != 0 && !black_active) {
                    continue;
                }

                uint64_t black_count = 0;
                uint64_t total_samples = 0;
                if (frame->data[0] && frame->linesize[0] > 0) {
                    int w = frame->width;
                    int h = frame->height;
                    int stride = frame->linesize[0];
                    int step = (w * h > 640 * 480) ? 4 : 1;

                    for (int y = 0; y < h; y += step) {
                        const uint8_t* row = frame->data[0] + (y * stride);
                        for (int x = 0; x < w; x += step) {
                            total_samples++;
                            if (row[x] <= max_y) {
                                black_count++;
                            }
                        }
                    }
                }

                double black_ratio = (total_samples > 0) ? (static_cast<double>(black_count) / total_samples) : 0.0;
                bool is_black = (black_ratio >= gap_config.black_ratio_threshold);

                if (is_black) {
                    if (!black_active) {
                        black_active = true;
                        black_start_sec = frame_sec;
                    }
                } else {
                    if (black_active) {
                        double dur = frame_sec - black_start_sec;
                        if (dur >= gap_config.min_gap_duration_sec) {
                            double gap_mid = (black_start_sec + frame_sec) / 2.0;
                            gap_timestamps.push_back(gap_mid);
                        }
                        black_active = false;
                    }
                }
            }
            av_packet_unref(pkt);
            if (stop_window) break;
        }

        if (black_active) {
            double dur = last_frame_sec - black_start_sec;
            if (dur >= gap_config.min_gap_duration_sec) {
                gap_timestamps.push_back((black_start_sec + last_frame_sec) / 2.0);
            }
        }
    };

    if (num_eps > 1 && file_duration > 60.0) {
        // Targeted window seeking: Scan +/- 180s around each expected episode boundary
        double ep_len = file_duration / static_cast<double>(num_eps);
        for (int k = 1; k < num_eps; k++) {
            double target_sec = k * ep_len;
            double w_start = std::max(0.0, target_sec - 180.0);
            double w_end = std::min(file_duration, target_sec + 180.0);
            ScanWindow(w_start, w_end);
        }
    } else {
        // Full file scan
        ScanWindow(0.0, 0.0);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return gap_timestamps;
}

bool FFmpegDemuxer::WriteSegment(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 double start_sec,
                                 double end_sec,
                                 std::function<void(double current_sec, uint64_t bytes)> progress_cb,
                                 std::string* error_msg) {
    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext* ifmt_ctx = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;
    std::string in_str = input_path.string();
    std::string out_str = output_path.string();

    int ret = avformat_open_input(&ifmt_ctx, in_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not open input: " + AvErr2Str(ret);
        return false;
    }

    ret = avformat_find_stream_info(ifmt_ctx, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Could not find stream info: " + AvErr2Str(ret);
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    ret = avformat_alloc_output_context2(&ofmt_ctx, nullptr, "matroska", out_str.c_str());
    if (!ofmt_ctx) {
        if (error_msg) *error_msg = "Could not create output context for Matroska format";
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    std::map<int, int> stream_map;
    int video_stream_idx = -1;

    // Copy global container metadata
    av_dict_copy(&ofmt_ctx->metadata, ifmt_ctx->metadata, 0);

    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }

        if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
            video_stream_idx = static_cast<int>(i);
        }

        AVStream* out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (!out_stream) {
            if (error_msg) *error_msg = "Failed to allocate output stream";
            avformat_free_context(ofmt_ctx);
            avformat_close_input(&ifmt_ctx);
            return false;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            if (error_msg) *error_msg = "Failed to copy codec parameters: " + AvErr2Str(ret);
            avformat_free_context(ofmt_ctx);
            avformat_close_input(&ifmt_ctx);
            return false;
        }
        out_stream->codecpar->codec_tag = 0;

        // Copy stream metadata tags (track title/description, language, etc.)
        av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);

        // Copy stream disposition flags (default, forced, commentary, etc.)
        out_stream->disposition = in_stream->disposition;

        stream_map[i] = out_stream->index;
    }

    // Copy and adjust embedded chapters for this segment window [start_sec, end_sec]
    for (unsigned int i = 0; i < ifmt_ctx->nb_chapters; i++) {
        AVChapter* in_ch = ifmt_ctx->chapters[i];
        double ch_start_sec = static_cast<double>(in_ch->start) * av_q2d(in_ch->time_base);
        double ch_end_sec = static_cast<double>(in_ch->end) * av_q2d(in_ch->time_base);

        if (ch_end_sec > start_sec && (end_sec <= 0.0 || ch_start_sec < end_sec)) {
            double rel_start_sec = std::max(0.0, ch_start_sec - start_sec);
            double rel_end_sec = (end_sec > 0.0) ? std::min(end_sec - start_sec, ch_end_sec - start_sec) : (ch_end_sec - start_sec);

            if (rel_end_sec > rel_start_sec) {
                AVRational tb = {1, 1000}; // Millisecond time base
                AVChapter* out_ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
                out_ch->id = in_ch->id;
                out_ch->time_base = tb;
                out_ch->start = static_cast<int64_t>(rel_start_sec * 1000.0);
                out_ch->end = static_cast<int64_t>(rel_end_sec * 1000.0);
                av_dict_copy(&out_ch->metadata, in_ch->metadata, 0);

                ofmt_ctx->chapters = static_cast<AVChapter**>(av_realloc_array(ofmt_ctx->chapters, ofmt_ctx->nb_chapters + 1, sizeof(AVChapter*)));
                ofmt_ctx->chapters[ofmt_ctx->nb_chapters] = out_ch;
                ofmt_ctx->nb_chapters++;
            }
        }
    }

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_str.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            if (error_msg) *error_msg = "Could not open output file '" + out_str + "': " + AvErr2Str(ret);
            avformat_free_context(ofmt_ctx);
            avformat_close_input(&ifmt_ctx);
            return false;
        }
    }

    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0) {
        if (error_msg) *error_msg = "Error occurred when writing output header: " + AvErr2Str(ret);
        if (ofmt_ctx->pb) avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
        avformat_close_input(&ifmt_ctx);
        return false;
    }

    // Seek to start position
    if (start_sec > 0.0) {
        if (video_stream_idx >= 0) {
            AVStream* vst = ifmt_ctx->streams[video_stream_idx];
            // Seek at the requested boundary. Subtracting a preroll here skips
            // an exact boundary keyframe and can repeat an entire previous GOP.
            int64_t vst_ts = static_cast<int64_t>(std::llround(start_sec / av_q2d(vst->time_base)));
            if (vst_ts < 0) vst_ts = 0;
            av_seek_frame(ifmt_ctx, video_stream_idx, vst_ts, AVSEEK_FLAG_BACKWARD);
        } else {
            int64_t target_ts = static_cast<int64_t>(start_sec * AV_TIME_BASE);
            av_seek_frame(ifmt_ctx, -1, target_ts, AVSEEK_FLAG_BACKWARD);
        }
    }

    AVPacket* pkt = av_packet_alloc();
    uint64_t total_bytes_written = 0;

    double base_time_sec = -1.0;
    bool found_first_keyframe = (start_sec <= 0.0 || video_stream_idx < 0);
    std::map<int, int64_t> last_written_dts;

    while (av_read_frame(ifmt_ctx, pkt) >= 0) {
        int in_idx = pkt->stream_index;
        if (stream_map.find(in_idx) == stream_map.end()) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* in_stream = ifmt_ctx->streams[in_idx];
        AVStream* out_stream = ofmt_ctx->streams[stream_map[in_idx]];

        double packet_pts_sec = (pkt->pts != AV_NOPTS_VALUE) ? (static_cast<double>(pkt->pts) * av_q2d(in_stream->time_base)) : 0.0;
        double packet_dts_sec = (pkt->dts != AV_NOPTS_VALUE) ? (static_cast<double>(pkt->dts) * av_q2d(in_stream->time_base)) : packet_pts_sec;

        // Skip packets until we reach the target start time
        if (!found_first_keyframe) {
            if (in_idx == video_stream_idx && (pkt->flags & AV_PKT_FLAG_KEY)) {
                found_first_keyframe = true;
                base_time_sec = packet_pts_sec;
            } else {
                av_packet_unref(pkt);
                continue;
            }
        }

        if (base_time_sec < 0.0) {
            base_time_sec = (start_sec > 0.0) ? packet_pts_sec : 0.0;
        }

        if (packet_pts_sec < base_time_sec - 0.01) {
            av_packet_unref(pkt);
            continue;
        }

        // Stop if past end time
        if (end_sec > start_sec && packet_pts_sec >= end_sec) {
            if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                break;
            } else if (packet_pts_sec >= end_sec + 0.5) {
                av_packet_unref(pkt);
                break;
            }
        }

        // Calculate unified relative timestamps in seconds from base_time_sec
        double rel_pts_sec = std::max(0.0, packet_pts_sec - base_time_sec);
        double rel_dts_sec = std::max(0.0, packet_dts_sec - base_time_sec);
        if (rel_pts_sec < rel_dts_sec) rel_pts_sec = rel_dts_sec;

        // Convert relative seconds to output stream time_base
        pkt->pts = static_cast<int64_t>(std::round(rel_pts_sec / av_q2d(out_stream->time_base)));
        pkt->dts = static_cast<int64_t>(std::round(rel_dts_sec / av_q2d(out_stream->time_base)));
        if (pkt->pts < pkt->dts) pkt->pts = pkt->dts;

        if (last_written_dts.find(in_idx) != last_written_dts.end()) {
            if (pkt->dts <= last_written_dts[in_idx]) {
                pkt->dts = last_written_dts[in_idx] + 1;
            }
        }
        if (pkt->pts < pkt->dts) pkt->pts = pkt->dts;
        last_written_dts[in_idx] = pkt->dts;

        if (pkt->duration > 0) {
            pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        }
        pkt->stream_index = stream_map[in_idx];
        pkt->pos = -1;

        total_bytes_written += pkt->size;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);

        if (ret < 0) {
            // Log error but attempt to continue
        }

        if (progress_cb) {
            progress_cb(packet_pts_sec, total_bytes_written);
        }
    }

    av_packet_free(&pkt);

    av_write_trailer(ofmt_ctx);

    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&ofmt_ctx->pb);
    }
    avformat_free_context(ofmt_ctx);
    avformat_close_input(&ifmt_ctx);

    return true;
}

} // namespace mkvsplit
