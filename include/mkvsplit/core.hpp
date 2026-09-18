#pragma once
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <cstdint>

namespace mkvsplit {

enum class SplitMode {
    TimeRange,
    FixedDuration,
    FileSize,
    Chapters,
    AutoGap
};

enum class GapType {
    BlackFrame,   // Detect black video frame gaps between episodes
    AudioSilence, // Detect audio silence gaps
    Combined      // Detect gaps matching both black video and silent audio
};

struct GapDetectionConfig {
    GapType type = GapType::BlackFrame;     // Default: black frame gap detection for episode splitting
    int expected_episodes = 0;             // 0 = split on all detected gaps; N > 1 = pick best N-1 gaps near episode boundaries
    double black_pixel_threshold = 0.10;   // Luminance threshold for black (0.10 = 10% brightness)
    double black_ratio_threshold = 0.98;   // Ratio of pixels required to be black (0.98 = 98%)
    double noise_threshold_db = -30.0;     // Noise floor in dB for audio silence
    double min_gap_duration_sec = 2.0;     // Minimum duration of black screen gap between episodes (default 2.0s)
    double min_segment_duration_sec = 10.0;// Minimum episode segment duration
};

struct ChapterInfo {
    int id = 0;
    std::string title;
    double start_time_sec = 0.0;
    double end_time_sec = 0.0;
};

struct TrackInfo {
    int index = 0;
    std::string codec_name;
    std::string media_type; // "video", "audio", "subtitle"
    std::string language;
    std::string title;
    int width = 0;
    int height = 0;
    int sample_rate = 0;
    int channels = 0;
};

struct MKVMetadata {
    std::filesystem::path file_path;
    double duration_sec = 0.0;
    uint64_t file_size_bytes = 0;
    std::string title;
    std::vector<TrackInfo> tracks;
    std::vector<ChapterInfo> chapters;
    std::vector<double> keyframes_sec;
};

struct SplitSegment {
    std::string name_tag;
    double start_time_sec = 0.0;
    double end_time_sec = 0.0;
};

enum class BumperAction {
    Merge,    // Default: Merge short intro/outro bumpers into adjacent episode
    Separate, // Dump short intro/outro bumpers as separate non-episode files (e.g. disc_intro.mkv, disc_outro.mkv)
    Discard   // Omit short intro/outro bumpers entirely
};

struct SplitConfig {
    std::filesystem::path input_file;
    std::filesystem::path output_directory;
    std::string output_prefix = "split_";
    SplitMode mode = SplitMode::TimeRange;
    BumperAction bumper_action = BumperAction::Merge;
    
    // Mode options
    double range_start_sec = 0.0;
    double range_end_sec = 0.0; // 0.0 means until end of file
    double chunk_duration_sec = 600.0; // Default 10 mins
    uint64_t chunk_max_bytes = 500 * 1024 * 1024; // Default 500 MB
    std::vector<int> selected_chapter_ids;
    int chapter_group_count = 0; // If > 0, group K chapters into 1 episode segment (e.g. 5 chapters per episode)
    int lead_bumper_chapters = 0; // Number of leading chapters identified as bumpers (e.g. studio logo)
    int tail_bumper_chapters = 0; // Number of trailing chapters identified as bumpers (e.g. disc outro/copyright)
    int season_number = 0;        // Season number (0 = auto-detect or default S01)
    int start_episode_number = 0; // Starting episode number (0 = auto-detect or default E01)
    GapDetectionConfig gap_config;
};

struct ProgressInfo {
    double overall_progress = 0.0; // 0.0 to 100.0
    int current_segment_index = 0;
    int total_segments = 0;
    std::string current_output_filename;
    double current_segment_progress = 0.0;
    uint64_t bytes_written = 0;
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

struct AutoGuessResult {
    SplitConfig recommended_config;
    int confidence_percent = 0;
    std::string reasoning;
};

class MKVSplitter {
public:
    // Inspect MKV file to retrieve metadata, track list, chapters, and keyframes
    static MKVMetadata InspectFile(const std::filesystem::path& path, std::string* error_msg = nullptr);

    // Automatically analyze metadata to guess the optimal splitting strategy
    static AutoGuessResult GuessSplitConfig(const MKVMetadata& metadata);

    // Detect audio silence gaps in MKV file using FFmpeg filter
    static std::vector<double> DetectGaps(const std::filesystem::path& path, const GapDetectionConfig& gap_config = {}, std::string* error_msg = nullptr);

    // Calculate segment plan without writing files
    static std::vector<SplitSegment> PlanSegments(const MKVMetadata& metadata, const SplitConfig& config);

    // Execute lossless split operation
    static bool Split(const SplitConfig& config, ProgressCallback callback = nullptr, std::string* error_msg = nullptr);
};

} // namespace mkvsplit
