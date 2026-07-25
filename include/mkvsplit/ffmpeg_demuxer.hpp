#pragma once
#include "mkvsplit/core.hpp"
#include <string>
#include <vector>

namespace mkvsplit {

class FFmpegDemuxer {
public:
    static bool ReadMetadata(const std::filesystem::path& file_path, MKVMetadata& out_metadata, std::string* error_msg = nullptr);

    static std::vector<double> DetectGaps(const std::filesystem::path& input_path,
                                         const GapDetectionConfig& gap_config,
                                         std::string* error_msg = nullptr);

    static std::vector<double> DetectBlackGaps(const std::filesystem::path& input_path,
                                              const GapDetectionConfig& gap_config,
                                              std::string* error_msg = nullptr);

    static std::vector<double> DetectSilenceGaps(const std::filesystem::path& input_path,
                                                const GapDetectionConfig& gap_config,
                                                std::string* error_msg = nullptr);

    static bool WriteSegment(const std::filesystem::path& input_path,
                             const std::filesystem::path& output_path,
                             double start_sec,
                             double end_sec,
                             std::function<void(double current_sec, uint64_t bytes)> progress_cb,
                             std::string* error_msg = nullptr);
};

} // namespace mkvsplit
