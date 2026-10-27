#include "mkvsplit/core.hpp"
#include "mkvsplit/ffmpeg_demuxer.hpp"
#include "mkvsplit/utils.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <regex>
#include <numeric>

namespace mkvsplit {

static int EpisodeCountFromFilename(const std::string& filename) {
    // Only trust an explicit consecutive list, e.g. E01E02 or S02E03-E04.
    static const std::regex list_regex(
        R"((^|[ ._-])(?:S[0-9]{1,6}[ ._-]*)?(EP?[0-9]{1,6}(?:[ ._-]*EP?[0-9]{1,6})+)(?![A-Za-z0-9]))",
        std::regex::icase);
    static const std::regex episode_regex(R"(EP?([0-9]{1,6}))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(filename, match, list_regex)) return 0;
    std::string list = match[2].str();
    int count = 0;
    int previous = -1;
    for (std::sregex_iterator it(list.begin(), list.end(), episode_regex), end; it != end; ++it) {
        int episode = std::stoi((*it)[1].str());
        if (count > 0 && episode != previous + 1) return 0;
        previous = episode;
        ++count;
    }
    return count;
}

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

static bool GuessPreviewChapterGroups(const std::vector<double>& durations, int expected_episodes,
                                     AutoGuessResult& result) {
    // Some discs chapter credits, next-episode previews, and a brief separator
    // individually. That repeated ending is stronger evidence than a fixed
    // chapter count, especially when a separator is missing or a finale is long.
    std::vector<int> groups;
    std::vector<double> runtimes, credits, previews;
    size_t start = 0;
    for (size_t i = 1; i + 1 < durations.size(); ++i) {
        double credit = durations[i], preview = durations[i + 1];
        if (i - start + 1 < 3 || credit < 60.0 || credit > 300.0 ||
            preview < 10.0 || preview > 60.0 || preview > credit * 0.6 ||
            durations[i - 1] < std::max(300.0, credit * 2.0)) continue;
        size_t end = i + 2;
        if (end < durations.size() && durations[end] <= 10.0) ++end;
        double runtime = std::accumulate(durations.begin() + start, durations.begin() + end, 0.0);
        if (runtime < 600.0 || runtime > 7200.0) return false;
        groups.push_back(static_cast<int>(end - start));
        runtimes.push_back(runtime);
        credits.push_back(credit);
        previews.push_back(preview);
        start = end;
        i = end - 1;
    }
    if (groups.size() < 3) return false;
    auto consistent = [](const std::vector<double>& values) {
        double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        double variance = 0.0;
        for (double value : values) variance += (value - mean) * (value - mean);
        return std::sqrt(variance / values.size()) / mean <= 0.20;
    };
    if (!consistent(credits) || !consistent(previews)) return false;
    auto sorted_runtimes = runtimes;
    std::sort(sorted_runtimes.begin(), sorted_runtimes.end());
    double typical_runtime = sorted_runtimes[sorted_runtimes.size() / 2];
    for (double runtime : runtimes) {
        if (std::abs(runtime - typical_runtime) > typical_runtime * 0.25) return false;
    }

    int tail = 0;
    bool longer_finale = false;
    if (start < durations.size()) {
        double remaining = std::accumulate(durations.begin() + start, durations.end(), 0.0);
        if (durations.size() - start <= 2 && remaining <= std::min(300.0, typical_runtime * 0.2)) {
            tail = static_cast<int>(durations.size() - start);
        } else {
            // Final episodes often omit a preview and may occupy a double slot.
            // Require substantial story content, not just trailing bonus chapters.
            int story_chapters = static_cast<int>(std::count_if(durations.begin() + start, durations.end(),
                [](double duration) { return duration >= 300.0; }));
            if (durations.size() - start < 3 || story_chapters < 2 ||
                remaining < typical_runtime * 0.65 || remaining > typical_runtime * 2.25) return false;
            groups.push_back(static_cast<int>(durations.size() - start));
            longer_finale = remaining > typical_runtime * 1.25;
        }
    }
    if (expected_episodes > 1 && groups.size() != static_cast<size_t>(expected_episodes)) return false;
    result.recommended_config.mode = SplitMode::Chapters;
    result.recommended_config.chapter_group_sizes = groups;
    result.recommended_config.tail_bumper_chapters = tail;
    result.confidence_percent = 90;
    std::ostringstream reasoning;
    reasoning << "Repeated credits/preview chapter pattern suggests " << groups.size() << " episodes (";
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i > 0) reasoning << ", ";
        reasoning << groups[i];
    }
    reasoning << " chapters per episode; typical runtime ~" << FormatDuration(typical_runtime) << ")";
    if (longer_finale) reasoning << ", including a longer final episode";
    if (tail > 0) reasoning << " + " << tail << " trailing extra chapter(s)";
    reasoning << ".";
    result.reasoning = reasoning.str();
    return true;
}

static bool GuessVariableChapterGroups(const std::vector<double>& durations, int expected_episodes,
                                      AutoGuessResult& result) {
    if (durations.size() < 9) return false;
    auto sorted = durations;
    std::sort(sorted.begin(), sorted.end());
    // A short closing chapter following story chapters can mark credits even when
    // different episodes have different numbers of acts. Require repeated timing
    // agreement before preferring these boundaries over fixed chapter symmetry.
    double short_limit = std::min(300.0, sorted[sorted.size() / 2] * 0.5);
    std::vector<int> groups;
    std::vector<double> episode_durations;
    size_t start = 0;
    double duration = 0.0;
    for (size_t i = 0; i < durations.size(); ++i) {
        duration += durations[i];
        if (i > start && i - start + 1 >= 3 && durations[i] <= short_limit &&
            durations[i - 1] > short_limit) {
            if (duration < 600.0 || duration > 7200.0) return false;
            groups.push_back(static_cast<int>(i - start + 1));
            episode_durations.push_back(duration);
            start = i + 1;
            duration = 0.0;
        }
    }
    if (groups.size() < 3 || (expected_episodes > 1 && groups.size() != static_cast<size_t>(expected_episodes))) return false;
    auto [min_group, max_group] = std::minmax_element(groups.begin(), groups.end());
    if (*min_group == *max_group || *max_group > 2 * *min_group) return false;
    double average = std::accumulate(episode_durations.begin(), episode_durations.end(), 0.0) / groups.size();
    // Keep a small trailing extra separate in the plan; the bumper policy decides
    // whether it is merged, exported separately, or discarded.
    if (durations.size() - start > 2 || duration > std::min(600.0, average * 0.2)) return false;
    double variance = 0.0;
    for (double episode : episode_durations) {
        if (std::abs(episode - average) > average * 0.15) return false;
        variance += (episode - average) * (episode - average);
    }
    double cv = std::sqrt(variance / groups.size()) / average;
    if (cv > 0.08) return false;

    result.recommended_config.mode = SplitMode::Chapters;
    result.recommended_config.chapter_group_sizes = groups;
    result.recommended_config.tail_bumper_chapters = static_cast<int>(durations.size() - start);
    result.confidence_percent = static_cast<int>(95.0 - cv * 50.0);
    std::ostringstream reasoning;
    reasoning << "Repeated short closing chapters suggest " << groups.size() << " episodes (~"
              << FormatDuration(average) << " each), with ";
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i > 0) reasoning << ", ";
        reasoning << groups[i];
    }
    reasoning << " chapters per episode";
    if (start < durations.size()) reasoning << " + " << durations.size() - start << " trailing extra chapter(s)";
    reasoning << ".";
    result.reasoning = reasoning.str();
    return true;
}

ChapterBoundaryValidation MKVSplitter::ValidateChapterBoundaries(const MKVMetadata& metadata, const SplitConfig& config) {
    ChapterBoundaryValidation validation;
    if (config.mode != SplitMode::Chapters) return validation;
    auto plan = PlanSegments(metadata, config);
    bool first_episode = true;
    for (const auto& segment : plan) {
        if (segment.name_tag == "disc_intro" || segment.name_tag == "disc_outro") continue;
        if (!first_episode) validation.boundaries_sec.push_back(segment.start_time_sec);
        first_episode = false;
    }
    if (validation.boundaries_sec.empty()) return validation;
    GapDetectionConfig gaps;
    gaps.type = GapType::BlackFrame;
    gaps.min_gap_duration_sec = 0.25;
    gaps.scan_targets_sec = validation.boundaries_sec;
    auto black_gaps = DetectGaps(metadata.file_path, gaps, &validation.error);
    if (!validation.error.empty()) return validation;
    for (double boundary : validation.boundaries_sec) {
        validation.supported_by_black.push_back(std::any_of(black_gaps.begin(), black_gaps.end(), [&](double gap) {
            return std::abs(gap - boundary) <= gaps.scan_radius_sec;
        }));
    }
    return validation;
}

AutoGuessResult MKVSplitter::GuessSplitConfig(const MKVMetadata& metadata, bool validate_video) {
    if (validate_video) {
        auto result = GuessSplitConfig(metadata, false);
        auto validation = ValidateChapterBoundaries(metadata, result.recommended_config);
        if (!validation.boundaries_sec.empty()) {
            if (!validation.error.empty()) {
                result.reasoning += " Black-frame validation unavailable: " + validation.error + ".";
            } else {
                int supported = static_cast<int>(std::count(validation.supported_by_black.begin(), validation.supported_by_black.end(), true));
                int total = static_cast<int>(validation.boundaries_sec.size());
                result.reasoning += " Black-frame check: " + std::to_string(supported) + "/" +
                    std::to_string(total) + " episode boundaries supported by a black transition within 3s.";
                // Missing black frames are weaker evidence, not a reason to merge episodes.
                result.confidence_percent = std::clamp(result.confidence_percent +
                    (supported == total ? 5 : -static_cast<int>(std::ceil(10.0 * (total - supported) / total))), 0, 98);
                if (supported < total) {
                    result.reasoning += " Unconfirmed chapter cuts retained:";
                    for (size_t i = 0; i < validation.boundaries_sec.size(); ++i) {
                        if (!validation.supported_by_black[i]) result.reasoning += " " + SecondsToTimeString(validation.boundaries_sec[i]);
                    }
                    result.reasoning += ". Review these in the preview.";
                }
            }
        }
        return result;
    }
    AutoGuessResult result;
    result.recommended_config.input_file = metadata.file_path;
    result.recommended_config.output_directory = metadata.file_path.parent_path() / "output";

    double total_duration = metadata.duration_sec;
    size_t num_ch = metadata.chapters.size();
    int filename_episodes = EpisodeCountFromFilename(metadata.file_path.stem().string());

    if (num_ch > 0) {
        std::vector<double> ch_durations;
        ch_durations.reserve(num_ch);
        for (size_t i = 0; i < num_ch; i++) {
            double d = metadata.chapters[i].end_time_sec - metadata.chapters[i].start_time_sec;
            if (d <= 0.0 && i + 1 < num_ch) {
                d = metadata.chapters[i + 1].start_time_sec - metadata.chapters[i].start_time_sec;
            }
            if (d <= 0.0 && total_duration > 0.0) {
                d = (total_duration - metadata.chapters[i].start_time_sec) / static_cast<double>(num_ch - i);
            }
            ch_durations.push_back(std::max(0.1, d));
        }

        if (GuessPreviewChapterGroups(ch_durations, filename_episodes, result)) return result;
        if (GuessVariableChapterGroups(ch_durations, filename_episodes, result)) return result;

        // Combinatorial brute-force search over lead/tail bumpers and chapter divisions
        int best_k = -1;
        int best_lead = 0;
        int best_tail = 0;
        int best_num_episodes = 0;
        double best_score = -1.0;
        double best_cv_ep = 1.0;
        double best_cv_intra = 1.0;
        double best_avg_ep_dur = 0.0;
        double best_lead_dur = 0.0;
        double best_tail_dur = 0.0;

        int max_lead = std::min(static_cast<int>(num_ch) - 1, 3);

        for (int lead = 0; lead <= max_lead; lead++) {
            double lead_dur = 0.0;
            for (int i = 0; i < lead; i++) lead_dur += ch_durations[i];

            // If lead bumper exists but is suspiciously long (> 300s / 5m), skip
            if (lead > 0 && lead_dur > 300.0) continue;

            int max_tail = std::min(static_cast<int>(num_ch) - lead - 1, 3);
            for (int tail = 0; tail <= max_tail; tail++) {
                int m = static_cast<int>(num_ch) - lead - tail;
                if (m < 1) continue;

                double tail_dur = 0.0;
                for (int i = 0; i < tail; i++) tail_dur += ch_durations[num_ch - 1 - i];
                if (tail > 0 && tail_dur > 300.0) continue;

                int max_k = std::min(m, 15);
                for (int k = 1; k <= max_k; k++) {
                    int num_groups = m / k;
                    if (num_groups < 1) continue;
                    if (filename_episodes > 1 && (num_groups != filename_episodes || m % k != 0)) continue;
                    
                    // If num_groups == 1 and num_ch > 1, this is not a multi-episode split
                    if (num_groups == 1 && num_ch > 1) continue;

                    std::vector<double> group_durations(num_groups, 0.0);
                    for (int g = 0; g < num_groups; g++) {
                        for (int i = 0; i < k; i++) {
                            group_durations[g] += ch_durations[lead + g * k + i];
                        }
                    }

                    double group_sum = 0.0;
                    for (double gd : group_durations) group_sum += gd;
                    double avg_group_dur = group_sum / num_groups;

                    // Plausibility floor: support short format shows like Mrs. Pepperpot (~7-10m),
                    // Peppa Pig (~5m), animated shorts (down to 120s / 2 mins)
                    if (avg_group_dur < 120.0 || avg_group_dur > 7200.0) {
                        continue;
                    }

                    // Compute Inter-group duration variance (CV_ep)
                    double group_var = 0.0;
                    if (num_groups > 1) {
                        for (double gd : group_durations) {
                            double diff = gd - avg_group_dur;
                            group_var += diff * diff;
                        }
                        group_var /= num_groups;
                    }
                    double cv_ep = (avg_group_dur > 0.0) ? (std::sqrt(group_var) / avg_group_dur) : 1.0;

                    // Compute Intra-group chapter pattern correlation (CV_intra)
                    double sum_pos_cv = 0.0;
                    if (num_groups > 1 && k > 1) {
                        for (int i = 0; i < k; i++) {
                            double pos_sum = 0.0;
                            for (int g = 0; g < num_groups; g++) {
                                pos_sum += ch_durations[lead + g * k + i];
                            }
                            double pos_avg = pos_sum / num_groups;
                            double pos_var = 0.0;
                            for (int g = 0; g < num_groups; g++) {
                                double diff = ch_durations[lead + g * k + i] - pos_avg;
                                pos_var += diff * diff;
                            }
                            pos_var /= num_groups;
                            double pos_cv = (pos_avg > 0.0) ? (std::sqrt(pos_var) / (pos_avg + 1.0)) : 0.0;
                            sum_pos_cv += pos_cv;
                        }
                        sum_pos_cv /= k;
                    }
                    double cv_intra = (k > 1 && num_groups > 1) ? sum_pos_cv : 0.0;

                    // Base score inversely proportional to variance
                    double score = 1.0 / ((1.0 + 8.0 * cv_ep) * (1.0 + (k > 1 ? 4.0 * cv_intra : 0.0)));

                    // Bonus for exact division (no unmapped content chapters)
                    if (m % k == 0) {
                        score *= 1.15;
                    }

                    // Sample size booster: more repeating episodes = stronger statistical certainty
                    if (num_groups >= 3) score *= 1.05;
                    if (num_groups >= 6) score *= 1.10;
                    if (num_groups >= 10) score *= 1.15;

                    // Bayesian Slot Priors: Boost common television broadcast brackets without penalizing others
                    // 20-26 mins (sitcoms / anime / half-hour commercial slots)
                    if (avg_group_dur >= 1200.0 && avg_group_dur <= 1560.0) {
                        score *= 1.15;
                    }
                    // 40-48 mins (1-hour network drama)
                    else if (avg_group_dur >= 2400.0 && avg_group_dur <= 2880.0) {
                        score *= 1.15;
                    }
                    // 10-13 mins (quarter-hour animation like SpongeBob, Adventure Time)
                    else if (avg_group_dur >= 600.0 && avg_group_dur <= 800.0) {
                        score *= 1.10;
                    }
                    // 6-10 mins (classic short-format anime / preschool like Mrs. Pepperpot, Bluey)
                    else if (avg_group_dur >= 360.0 && avg_group_dur <= 600.0) {
                        score *= 1.10;
                    }

                    // Bumper plausibility booster
                    if (lead > 0) {
                        if (lead_dur <= 90.0) score *= 1.08;
                        else if (lead_dur <= 180.0) score *= 1.02;
                    }
                    if (tail > 0) {
                        if (tail_dur <= 90.0) score *= 1.08;
                        else if (tail_dur <= 180.0) score *= 1.02;
                    }

                    // If zero bumpers needed and exact match on entire file, slight simplicity preference
                    if (lead == 0 && tail == 0 && (m % k == 0)) {
                        score *= 1.05;
                    }

                    if (score > best_score) {
                        best_score = score;
                        best_k = k;
                        best_lead = lead;
                        best_tail = tail;
                        best_num_episodes = num_groups;
                        best_cv_ep = cv_ep;
                        best_cv_intra = cv_intra;
                        best_avg_ep_dur = avg_group_dur;
                        best_lead_dur = lead_dur;
                        best_tail_dur = tail_dur;
                    }
                }
            }
        }

        // Evaluate cyclical pattern detection confidence
        if (best_k > 0 && (best_cv_ep < 0.20 || num_ch == static_cast<size_t>(best_k))) {
            result.recommended_config.mode = SplitMode::Chapters;
            result.recommended_config.chapter_group_count = best_k;
            result.recommended_config.lead_bumper_chapters = best_lead;
            result.recommended_config.tail_bumper_chapters = best_tail;

            int conf = std::min(98, std::max(85, static_cast<int>(98.0 - (best_cv_ep * 45.0 + best_cv_intra * 25.0))));
            if (best_k == 1 && best_lead == 0 && best_tail == 0 && best_cv_ep < 0.05) {
                conf = 98;
            }
            result.confidence_percent = conf;

            std::ostringstream ss;
            ss << "Chapter symmetry detected: " << num_ch << " total chapters match ";
            if (best_lead > 0) {
                ss << best_lead << " lead bumper (" << static_cast<int>(best_lead_dur) << "s) + ";
            }
            ss << best_num_episodes << " episodes (" << best_k << " ch/ep, ~" 
               << FormatDuration(best_avg_ep_dur) << " each, duration variance: "
               << std::fixed << std::setprecision(1) << (best_cv_ep * 100.0) << "%)";
            if (best_tail > 0) {
                ss << " + " << best_tail << " tail bumper (" << static_cast<int>(best_tail_dur) << "s)";
            }
            ss << ".";
            result.reasoning = ss.str();
            return result;
        }

        // Fallback: Estimate K from overall duration ratio if no clear periodic pattern emerged
        int est_eps = std::max(1, static_cast<int>(std::round(total_duration / 1440.0))); // ~24 mins per episode
        int k = std::max(1, static_cast<int>(std::round(static_cast<double>(num_ch) / est_eps)));

        if (filename_episodes == 0 && k >= 2 && k <= 10) {
            result.recommended_config.mode = SplitMode::Chapters;
            result.recommended_config.chapter_group_count = k;
            result.confidence_percent = 90;
            result.reasoning = "Detected " + std::to_string(num_ch) + " total chapters. Grouping every " +
                               std::to_string(k) + " chapters yields " + std::to_string(num_ch / k) +
                               " episodes (~" + std::to_string(static_cast<int>((total_duration / (num_ch / k)) / 60.0)) +
                               " mins each).";
            return result;
        }
    }

    // Fallback: Auto Gap detection with guessed episode count
    int est_eps = filename_episodes > 1 ? filename_episodes :
        std::max(1, static_cast<int>(std::round(total_duration / 1440.0)));
    result.recommended_config.mode = SplitMode::AutoGap;
    result.recommended_config.gap_config.type = GapType::BlackFrame;
    result.recommended_config.gap_config.expected_episodes = est_eps;
    result.recommended_config.gap_config.min_gap_duration_sec = 1.0;
    result.recommended_config.gap_config.min_segment_duration_sec = 600.0;
    result.confidence_percent = 80;
    result.reasoning = "Guessed " + std::to_string(est_eps) + " episodes based on total duration of " +
                       FormatDuration(total_duration) + ". Uses black frame gap detection around expected episode boundaries.";
    if (filename_episodes > 1) {
        result.reasoning = "Filename lists " + std::to_string(filename_episodes) +
            " consecutive episodes. Uses black frame gap detection around expected episode boundaries.";
    }
    return result;
}

std::vector<double> MKVSplitter::DetectGaps(const std::filesystem::path& path, const GapDetectionConfig& gap_config, std::string* error_msg) {
    return FFmpegDemuxer::DetectGaps(path, gap_config, error_msg);
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
            } else if (config.chapter_group_count > 0 || !config.chapter_group_sizes.empty()) {
                int total_ch = static_cast<int>(metadata.chapters.size());
                int lead = std::clamp(config.lead_bumper_chapters, 0, std::max(0, total_ch - 1));
                int tail = std::clamp(config.tail_bumper_chapters, 0, std::max(0, total_ch - lead - 1));
                int content_ch = total_ch - lead - tail;
                std::vector<int> group_sizes = config.chapter_group_sizes;
                if (config.chapter_group_count > 0) {
                    group_sizes.assign(content_ch / config.chapter_group_count, config.chapter_group_count);
                }
                int grouped_ch = 0;
                for (int size : group_sizes) {
                    if (size <= 0 || size > content_ch - grouped_ch) return {};
                    grouped_ch += size;
                }
                int ep_idx = 0;
                double min_seg = (config.gap_config.min_segment_duration_sec > 10.0) ? config.gap_config.min_segment_duration_sec : 120.0;

                // 1. Lead bumper handling
                if (lead > 0) {
                    double lead_start = metadata.chapters[0].start_time_sec;
                    double lead_end = metadata.chapters[lead - 1].end_time_sec;
                    if (config.bumper_action == BumperAction::Separate) {
                        SplitSegment bseg;
                        bseg.start_time_sec = lead_start;
                        bseg.end_time_sec = lead_end;
                        bseg.name_tag = "disc_intro";
                        segments.push_back(bseg);
                    }
                }

                // 2. Main episode segments
                int num_episodes = static_cast<int>(group_sizes.size());
                int start_ch_idx = lead;
                for (int e = 0; e < num_episodes; e++) {
                    int end_ch_idx = start_ch_idx + group_sizes[e] - 1;

                    double start_t = metadata.chapters[start_ch_idx].start_time_sec;
                    double end_t = metadata.chapters[end_ch_idx].end_time_sec;

                    // If first episode and lead bumper should be merged into it
                    if (e == 0 && lead > 0 && config.bumper_action == BumperAction::Merge) {
                        start_t = 0.0;
                    }
                    // If last episode and tail bumper should be merged into it
                    if (e == num_episodes - 1) {
                        if (tail > 0 && config.bumper_action == BumperAction::Merge) {
                            end_t = file_duration;
                        } else if (tail == 0 && end_t < file_duration && (file_duration - end_t) < min_seg) {
                            // Merge tiny residual tail into last episode
                            end_t = file_duration;
                        }
                    }

                    SplitSegment seg;
                    seg.start_time_sec = start_t;
                    seg.end_time_sec = end_t;
                    seg.name_tag = FormatSE(ep_idx++);
                    segments.push_back(seg);
                    start_ch_idx = end_ch_idx + 1;
                }

                // 3. Tail bumper / unmapped trailing fragment handling
                if (tail > 0) {
                    double tail_start = metadata.chapters[total_ch - tail].start_time_sec;
                    double tail_end = file_duration;
                    if (config.bumper_action == BumperAction::Separate) {
                        SplitSegment bseg;
                        bseg.start_time_sec = tail_start;
                        bseg.end_time_sec = tail_end;
                        bseg.name_tag = "disc_outro";
                        segments.push_back(bseg);
                    }
                } else if (grouped_ch < content_ch) {
                    // Leftover chapters at the end when tail was 0
                    int rem_start_ch = lead + grouped_ch;
                    double rem_start = metadata.chapters[rem_start_ch].start_time_sec;
                    double rem_end = file_duration;
                    if (config.bumper_action == BumperAction::Merge && !segments.empty()) {
                        segments.back().end_time_sec = rem_end;
                    } else if (config.bumper_action == BumperAction::Separate) {
                        SplitSegment bseg;
                        bseg.start_time_sec = rem_start;
                        bseg.end_time_sec = rem_end;
                        bseg.name_tag = "disc_outro";
                        segments.push_back(bseg);
                    }
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
    if (!segments.empty() && (config.mode == SplitMode::AutoGap || (config.mode == SplitMode::Chapters &&
        (config.chapter_group_count > 0 || !config.chapter_group_sizes.empty())))) {
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
