#include "mkvsplit/core.hpp"
#include "mkvsplit/ffmpeg_demuxer.hpp"
#include "mkvsplit/utils.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <regex>

namespace mkvsplit {

static bool ParseSeasonEpisodeFromFilename(const std::string& filename, int& season, int& start_ep, std::string& show_name) {
    std::regex se_regex("(.*?)[ ._-]*[Ss](\\d+)[ ._-]*[Ee](\\d+)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(filename, match, se_regex)) {
        show_name = match[1].str();
        season = std::stoi(match[2].str());
        start_ep = std::stoi(match[3].str());
        while (!show_name.empty() && (show_name.back() == ' ' || show_name.back() == '-' || show_name.back() == '_')) {
            show_name.pop_back();
        }
        return true;
    }
    std::regex ep_regex("(.*?)[ ._-]*[Ee][Pp]?(\\d+)", std::regex::icase);
    if (std::regex_search(filename, match, ep_regex)) {
        show_name = match[1].str();
        season = 1;
        start_ep = std::stoi(match[2].str());
        while (!show_name.empty() && (show_name.back() == ' ' || show_name.back() == '-' || show_name.back() == '_')) {
            show_name.pop_back();
        }
        return true;
    }
    return false;
}

MKVMetadata MKVSplitter::InspectFile(const std::filesystem::path& path, std::string* error_msg) {
    MKVMetadata meta;
    FFmpegDemuxer::ReadMetadata(path, meta, error_msg);
    return meta;
}

AutoGuessResult MKVSplitter::GuessSplitConfig(const MKVMetadata& metadata) {
    AutoGuessResult result;
    result.recommended_config.input_file = metadata.file_path;
    result.recommended_config.output_directory = metadata.file_path.parent_path() / "output";

    double duration = metadata.duration_sec;
    size_t num_ch = metadata.chapters.size();

    if (num_ch > 0) {
        double avg_ch_len = duration / static_cast<double>(num_ch);

        // Case 1: 1 Chapter per Episode (average chapter ~18m to 32m)
        if (avg_ch_len >= 1080.0 && avg_ch_len <= 1920.0) {
            result.recommended_config.mode = SplitMode::Chapters;
            result.recommended_config.chapter_group_count = 1;
            result.confidence_percent = 98;
            result.reasoning = "Detected " + std::to_string(num_ch) + " chapters with average duration of " +
                               std::to_string(static_cast<int>(avg_ch_len / 60.0)) +
                               " mins per chapter. Recommends 1 chapter per episode.";
            return result;
        }

        // Case 2: Group K chapters per episode (e.g. 5 chapters per episode)
        int est_eps = std::max(1, static_cast<int>(std::round(duration / 1440.0))); // ~24 mins per episode
        int k = std::max(1, static_cast<int>(std::round(static_cast<double>(num_ch) / est_eps)));

        if (k >= 2 && k <= 10) {
            result.recommended_config.mode = SplitMode::Chapters;
            result.recommended_config.chapter_group_count = k;
            result.confidence_percent = 95;
            result.reasoning = "Detected " + std::to_string(num_ch) + " total chapters. Grouping every " +
                               std::to_string(k) + " chapters yields " + std::to_string(num_ch / k) +
                               " episodes (~" + std::to_string(static_cast<int>((duration / (num_ch / k)) / 60.0)) +
                               " mins each).";
            return result;
        }
    }

    // Fallback: Auto Gap detection with guessed episode count
    int est_eps = std::max(1, static_cast<int>(std::round(duration / 1440.0)));
    result.recommended_config.mode = SplitMode::AutoGap;
    result.recommended_config.gap_config.type = GapType::BlackFrame;
    result.recommended_config.gap_config.expected_episodes = est_eps;
    result.recommended_config.gap_config.min_gap_duration_sec = 1.0;
    result.recommended_config.gap_config.min_segment_duration_sec = 600.0;
    result.confidence_percent = 80;
    result.reasoning = "Guessed " + std::to_string(est_eps) + " episodes based on total duration of " +
                       FormatDuration(duration) + ". Uses black frame gap detection around expected episode boundaries.";
    return result;
}

std::vector<double> MKVSplitter::DetectGaps(const std::filesystem::path& path, const GapDetectionConfig& gap_config, std::string* error_msg) {
    return FFmpegDemuxer::DetectSilenceGaps(path, gap_config, error_msg);
}

std::vector<SplitSegment> MKVSplitter::PlanSegments(const MKVMetadata& metadata, const SplitConfig& config) {
    std::vector<SplitSegment> segments;

    double file_duration = metadata.duration_sec;
    if (file_duration <= 0.0) {
        file_duration = 360000.0; // fallback if duration unknown
    }

    int season = (config.season_number > 0) ? config.season_number : 1;
    int start_ep = (config.start_episode_number > 0) ? config.start_episode_number : 1;
    std::string parsed_show_name;

    int auto_s = 0, auto_e = 0;
    if (ParseSeasonEpisodeFromFilename(metadata.file_path.stem().string(), auto_s, auto_e, parsed_show_name)) {
        if (config.season_number <= 0) season = auto_s;
        if (config.start_episode_number <= 0) start_ep = auto_e;
    }

    auto FormatSE = [&](int ep_offset) {
        std::ostringstream ss;
        ss << "S" << (season < 10 ? "0" : "") << season
           << "E" << (start_ep + ep_offset < 10 ? "0" : "") << (start_ep + ep_offset);
        return ss.str();
    };

    switch (config.mode) {
        case SplitMode::TimeRange: {
            SplitSegment seg;
            seg.start_time_sec = std::max(0.0, config.range_start_sec);
            if (config.range_end_sec > seg.start_time_sec && config.range_end_sec <= file_duration) {
                seg.end_time_sec = config.range_end_sec;
            } else {
                seg.end_time_sec = file_duration;
            }
            seg.name_tag = "part1";
            segments.push_back(seg);
            break;
        }

        case SplitMode::FixedDuration: {
            double step = (config.chunk_duration_sec > 1.0) ? config.chunk_duration_sec : 600.0;
            int part = 1;
            for (double t = 0.0; t < file_duration; t += step) {
                SplitSegment seg;
                seg.start_time_sec = t;
                seg.end_time_sec = std::min(t + step, file_duration);
                seg.name_tag = "part" + std::to_string(part++);
                segments.push_back(seg);
            }
            break;
        }

        case SplitMode::FileSize: {
            uint64_t max_bytes = (config.chunk_max_bytes > 1024 * 1024) ? config.chunk_max_bytes : (100 * 1024 * 1024);
            uint64_t total_bytes = metadata.file_size_bytes;
            if (total_bytes == 0 || file_duration <= 0.0) {
                SplitSegment seg;
                seg.start_time_sec = 0.0;
                seg.end_time_sec = file_duration;
                seg.name_tag = "part1";
                segments.push_back(seg);
            } else {
                int num_chunks = static_cast<int>(std::ceil(static_cast<double>(total_bytes) / static_cast<double>(max_bytes)));
                if (num_chunks < 1) num_chunks = 1;
                double chunk_sec = file_duration / num_chunks;
                int part = 1;
                for (double t = 0.0; t < file_duration; t += chunk_sec) {
                    SplitSegment seg;
                    seg.start_time_sec = t;
                    seg.end_time_sec = std::min(t + chunk_sec, file_duration);
                    seg.name_tag = "part" + std::to_string(part++);
                    segments.push_back(seg);
                }
            }
            break;
        }

        case SplitMode::Chapters: {
            if (metadata.chapters.empty()) {
                // Fallback to full file if no chapters exist
                SplitSegment seg;
                seg.start_time_sec = 0.0;
                seg.end_time_sec = file_duration;
                seg.name_tag = "full";
                segments.push_back(seg);
            } else if (config.chapter_group_count > 0) {
                int group_size = config.chapter_group_count;
                int total_ch = static_cast<int>(metadata.chapters.size());
                int ep_idx = 0;
                double min_seg = (config.gap_config.min_segment_duration_sec > 10.0) ? config.gap_config.min_segment_duration_sec : 300.0;

                for (int i = 0; i < total_ch; i += group_size) {
                    int end_idx = std::min(i + group_size - 1, total_ch - 1);
                    double start_t = metadata.chapters[i].start_time_sec;
                    double end_t = metadata.chapters[end_idx].end_time_sec;

                    // Check if remaining fragment is shorter than min_seg (e.g. disc logo/outro)
                    if (end_t - start_t < min_seg && !segments.empty()) {
                        if (config.bumper_action == BumperAction::Merge) {
                            segments.back().end_time_sec = file_duration;
                        } else if (config.bumper_action == BumperAction::Separate) {
                            SplitSegment seg;
                            seg.start_time_sec = start_t;
                            seg.end_time_sec = file_duration;
                            seg.name_tag = "disc_outro";
                            segments.push_back(seg);
                        }
                        // If Discard: omit it
                        break;
                    }

                    SplitSegment seg;
                    seg.start_time_sec = start_t;
                    seg.end_time_sec = end_t;
                    seg.name_tag = FormatSE(ep_idx++);
                    segments.push_back(seg);
                }
            } else {
                for (const auto& ch : metadata.chapters) {
                    if (!config.selected_chapter_ids.empty()) {
                        auto it = std::find(config.selected_chapter_ids.begin(), config.selected_chapter_ids.end(), ch.id);
                        if (it == config.selected_chapter_ids.end()) continue;
                    }
                    SplitSegment seg;
                    seg.start_time_sec = ch.start_time_sec;
                    seg.end_time_sec = ch.end_time_sec;
                    std::string clean_title = ch.title;
                    std::replace(clean_title.begin(), clean_title.end(), ' ', '_');
                    seg.name_tag = "ch" + std::to_string(ch.id + 1) + "_" + clean_title;
                    segments.push_back(seg);
                }
            }
            break;
        }

        case SplitMode::AutoGap: {
            std::vector<double> detected_gaps = DetectGaps(metadata.file_path, config.gap_config);
            
            std::vector<double> selected_gaps;
            int num_eps = config.gap_config.expected_episodes;

            if (num_eps > 1 && !detected_gaps.empty()) {
                // Select N-1 gaps closest to ideal equal segment boundaries (k * duration / N)
                double ideal_len = file_duration / static_cast<double>(num_eps);
                for (int k = 1; k < num_eps; k++) {
                    double target_t = k * ideal_len;
                    double best_gap = -1.0;
                    double min_diff = 1e9;
                    for (double g : detected_gaps) {
                        double diff = std::abs(g - target_t);
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_gap = g;
                        }
                    }
                    if (best_gap > 0.0) {
                        selected_gaps.push_back(best_gap);
                    }
                }
                std::sort(selected_gaps.begin(), selected_gaps.end());
                selected_gaps.erase(std::unique(selected_gaps.begin(), selected_gaps.end()), selected_gaps.end());
            } else {
                selected_gaps = detected_gaps;
            }

            // Build cut boundary points [0, gap1, gap2, ..., file_duration]
            std::vector<double> boundaries;
            boundaries.push_back(0.0);
            double last_b = 0.0;

            for (double gap_t : selected_gaps) {
                if (gap_t - last_b >= config.gap_config.min_segment_duration_sec && file_duration - gap_t >= config.gap_config.min_segment_duration_sec) {
                    boundaries.push_back(gap_t);
                    last_b = gap_t;
                }
            }
            boundaries.push_back(file_duration);

            int ep_idx = 0;
            for (size_t i = 0; i + 1 < boundaries.size(); i++) {
                SplitSegment seg;
                seg.start_time_sec = boundaries[i];
                seg.end_time_sec = boundaries[i + 1];
                seg.name_tag = FormatSE(ep_idx++);
                segments.push_back(seg);
            }
            break;
        }
    }

    // Bumper Post-Processing (Intros & Outros)
    if (!segments.empty() && (config.mode == SplitMode::AutoGap || (config.mode == SplitMode::Chapters && config.chapter_group_count > 0))) {
        double min_seg = (config.gap_config.min_segment_duration_sec > 10.0) ? config.gap_config.min_segment_duration_sec : 300.0;

        // Leading Intro Bumper
        if (segments[0].start_time_sec > 0.1 && segments[0].start_time_sec < min_seg) {
            if (config.bumper_action == BumperAction::Merge) {
                segments[0].start_time_sec = 0.0;
            } else if (config.bumper_action == BumperAction::Separate) {
                SplitSegment intro_seg;
                intro_seg.start_time_sec = 0.0;
                intro_seg.end_time_sec = segments[0].start_time_sec;
                intro_seg.name_tag = "disc_intro";
                segments.insert(segments.begin(), intro_seg);
            }
        } else if (config.bumper_action == BumperAction::Merge && segments[0].start_time_sec <= 0.1) {
            segments[0].start_time_sec = 0.0;
        }

        // Trailing Outro Bumper
        if (segments.back().end_time_sec < file_duration - 0.1) {
            double trailing_dur = file_duration - segments.back().end_time_sec;
            if (trailing_dur < min_seg) {
                if (config.bumper_action == BumperAction::Merge) {
                    segments.back().end_time_sec = file_duration;
                } else if (config.bumper_action == BumperAction::Separate) {
                    SplitSegment outro_seg;
                    outro_seg.start_time_sec = segments.back().end_time_sec;
                    outro_seg.end_time_sec = file_duration;
                    outro_seg.name_tag = "disc_outro";
                    segments.push_back(outro_seg);
                }
            }
        } else if (config.bumper_action == BumperAction::Merge) {
            segments.back().end_time_sec = file_duration;
        }
    }

    return segments;
}

bool MKVSplitter::Split(const SplitConfig& config, ProgressCallback callback, std::string* error_msg) {
    if (!std::filesystem::exists(config.input_file)) {
        if (error_msg) *error_msg = "Input file does not exist: " + config.input_file.string();
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.output_directory, ec);

    MKVMetadata metadata = InspectFile(config.input_file, error_msg);
    if (metadata.tracks.empty()) {
        if (error_msg) *error_msg = "Failed to demux or find tracks in input file";
        return false;
    }

    std::vector<SplitSegment> segments = PlanSegments(metadata, config);
    if (segments.empty()) {
        if (error_msg) *error_msg = "No valid split segments planned";
        return false;
    }

    int total_segments = static_cast<int>(segments.size());
    std::string stem = config.input_file.stem().string();

    for (int i = 0; i < total_segments; i++) {
        const auto& seg = segments[i];

        std::ostringstream filename_ss;
        if (!config.output_prefix.empty()) {
            filename_ss << config.output_prefix;
        }
        if (seg.name_tag.rfind("S", 0) == 0 && seg.name_tag.find("E") != std::string::npos) {
            int auto_s = 0, auto_e = 0;
            std::string show_name;
            if (ParseSeasonEpisodeFromFilename(stem, auto_s, auto_e, show_name) && !show_name.empty()) {
                filename_ss << show_name << " - " << seg.name_tag << ".mkv";
            } else {
                filename_ss << stem << " - " << seg.name_tag << ".mkv";
            }
        } else {
            filename_ss << stem << "_" << seg.name_tag << ".mkv";
        }

        std::filesystem::path out_file = config.output_directory / filename_ss.str();

        ProgressInfo info;
        info.current_segment_index = i + 1;
        info.total_segments = total_segments;
        info.current_output_filename = out_file.filename().string();
        info.overall_progress = (static_cast<double>(i) / total_segments) * 100.0;

        if (callback) callback(info);

        double last_cb_sec = -1.0;
        uint64_t last_cb_bytes = 0;

        auto inner_cb = [&](double current_sec, uint64_t bytes) {
            if (callback && (current_sec - last_cb_sec >= 0.25 || bytes - last_cb_bytes >= 10 * 1024 * 1024)) {
                last_cb_sec = current_sec;
                last_cb_bytes = bytes;
                double seg_len = seg.end_time_sec - seg.start_time_sec;
                double seg_done = current_sec - seg.start_time_sec;
                info.current_segment_progress = (seg_len > 0) ? std::clamp((seg_done / seg_len) * 100.0, 0.0, 100.0) : 100.0;
                info.overall_progress = ((static_cast<double>(i) + (info.current_segment_progress / 100.0)) / total_segments) * 100.0;
                info.bytes_written = bytes;
                callback(info);
            }
        };

        bool ok = FFmpegDemuxer::WriteSegment(config.input_file, out_file, seg.start_time_sec, seg.end_time_sec, inner_cb, error_msg);
        if (!ok) return false;
    }

    if (callback) {
        ProgressInfo info;
        info.overall_progress = 100.0;
        info.current_segment_index = total_segments;
        info.total_segments = total_segments;
        info.current_segment_progress = 100.0;
        callback(info);
    }

    return true;
}

} // namespace mkvsplit
