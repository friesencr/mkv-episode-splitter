#include "mkvsplit/core.hpp"
#include "mkvsplit/utils.hpp"
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <filesystem>

int main() {
    std::cout << "========================================================\n";
    std::cout << " RUNNING MKV-SPLITTER AUTOMATED TEST SUITE\n";
    std::cout << "========================================================\n\n";

    std::filesystem::path test_dir = "./test_run_dir";
    std::filesystem::create_directories(test_dir);

    std::filesystem::path test_mkv = test_dir / "synthetic_test.mkv";

    std::cout << "[Test 1] Generating synthetic test MKV video with chapters and multi-audio track metadata...\n";
    // Generate 15-second synthetic MKV with video (h264), 2 audio tracks (aac with track titles/languages), and metadata
    std::string gen_cmd = "ffmpeg -y -f lavfi -i testsrc=duration=15:size=640x360:rate=30 "
                          "-f lavfi -i sine=frequency=440:duration=15 "
                          "-f lavfi -i sine=frequency=880:duration=15 "
                          "-map 0:v -map 1:a -map 2:a "
                          "-c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac "
                          "-metadata title=\"Synthetic Test Video\" "
                          "-metadata:s:a:0 title=\"English (Main)\" -metadata:s:a:0 language=eng "
                          "-metadata:s:a:1 title=\"English (Commentary)\" -metadata:s:a:1 language=eng "
                          "\"" + test_mkv.string() + "\" > /dev/null 2>&1";

    int ret = std::system(gen_cmd.c_str());
    if (ret != 0 || !std::filesystem::exists(test_mkv)) {
        std::cerr << "FAILED to generate synthetic test MKV using ffmpeg\n";
        return 1;
    }
    std::cout << "PASSED: Synthetic test file created (" << test_mkv.string() << ").\n\n";

    std::cout << "[Test 2] Testing MKVSplitter::InspectFile()...\n";
    std::string err;
    mkvsplit::MKVMetadata meta = mkvsplit::MKVSplitter::InspectFile(test_mkv, &err);
    assert(err.empty());
    assert(meta.tracks.size() >= 3);
    assert(meta.duration_sec >= 14.0 && meta.duration_sec <= 16.0);
    assert(meta.tracks[1].title == "English (Main)");
    assert(meta.tracks[2].title == "English (Commentary)");
    std::cout << "  - Tracks found: " << meta.tracks.size() << "\n";
    std::cout << "  - Track 1 title: " << meta.tracks[1].title << "\n";
    std::cout << "  - Track 2 title: " << meta.tracks[2].title << "\n";
    std::cout << "  - Duration: " << meta.duration_sec << " s\n";
    std::cout << "PASSED: Metadata inspection verified.\n\n";

    std::cout << "[Test 3] Testing MKVSplitter::PlanSegments() in FixedDuration mode...\n";
    mkvsplit::SplitConfig config;
    config.input_file = test_mkv;
    config.output_directory = test_dir / "out_duration";
    config.mode = mkvsplit::SplitMode::FixedDuration;
    config.chunk_duration_sec = 6.0; // 15 sec total -> 3 chunks (0-6, 6-12, 12-15)

    std::vector<mkvsplit::SplitSegment> plan = mkvsplit::MKVSplitter::PlanSegments(meta, config);
    assert(plan.size() == 3);
    assert(plan[0].start_time_sec == 0.0 && plan[0].end_time_sec == 6.0);
    assert(plan[1].start_time_sec == 6.0 && plan[1].end_time_sec == 12.0);
    assert(plan[2].start_time_sec == 12.0);
    std::cout << "PASSED: Segment plan created 3 segments as expected.\n\n";

    std::cout << "[Test 4] Executing MKVSplitter::Split() on test file...\n";
    bool split_ok = mkvsplit::MKVSplitter::Split(config, [](const mkvsplit::ProgressInfo& info) {
        std::cout << "  - Split progress: " << static_cast<int>(info.overall_progress) << "%\n";
    }, &err);

    assert(split_ok);
    assert(err.empty());

    // Verify generated segment files and preserved audio track title metadata
    for (int i = 1; i <= 3; i++) {
        std::filesystem::path seg_file = config.output_directory / ("split_synthetic_test_part" + std::to_string(i) + ".mkv");
        assert(std::filesystem::exists(seg_file));
        mkvsplit::MKVMetadata seg_meta = mkvsplit::MKVSplitter::InspectFile(seg_file);
        assert(seg_meta.tracks.size() >= 3);
        assert(seg_meta.tracks[1].title == "English (Main)");
        assert(seg_meta.tracks[2].title == "English (Commentary)");
        assert(seg_meta.duration_sec > 2.0 && seg_meta.duration_sec < 7.5);
    }
    std::cout << "PASSED: All 3 split segment files created and demuxed cleanly with preserved track metadata.\n\n";

    std::cout << "[Test 5] Testing Auto Gap Detection (MKVSplitter::DetectGaps)...\n";
    std::filesystem::path silence_mkv = test_dir / "silence_gap_test.mkv";
    std::string gen_gap_cmd = "ffmpeg -y -f lavfi -i testsrc=duration=10:size=320x240:rate=30 "
                              "-f lavfi -t 10 -i anullsrc=r=44100:cl=mono "
                              "-c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac "
                              "\"" + silence_mkv.string() + "\" > /dev/null 2>&1";
    int r2 = std::system(gen_gap_cmd.c_str());
    assert(r2 == 0 && std::filesystem::exists(silence_mkv));

    mkvsplit::GapDetectionConfig gcfg;
    gcfg.type = mkvsplit::GapType::AudioSilence;
    gcfg.noise_threshold_db = -30.0;
    gcfg.min_gap_duration_sec = 0.5;
    gcfg.min_segment_duration_sec = 1.0;

    std::vector<double> detected_gaps = mkvsplit::MKVSplitter::DetectGaps(silence_mkv, gcfg, &err);
    assert(err.empty());
    assert(!detected_gaps.empty());
    std::cout << "  - Silence gaps detected: " << detected_gaps.size() << "\n";
    std::cout << "PASSED: Audio silence gap detection verified.\n\n";

    std::cout << "[Test 6] Testing Black Frame Episode Gap Detection (GapType::BlackFrame)...\n";
    std::filesystem::path black_gap_mkv = test_dir / "black_gap_episode_test.mkv";
    std::string gen_black_cmd = "ffmpeg -y -f lavfi -i testsrc=duration=4:size=320x240:rate=30 "
                                "-f lavfi -i color=c=black:duration=1:size=320x240:rate=30 "
                                "-f lavfi -i testsrc=duration=4:size=320x240:rate=30 "
                                "-filter_complex \"[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]\" "
                                "-f lavfi -t 9 -i anullsrc=r=44100:cl=mono "
                                "-map \"[v]\" -map 3:a -c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac "
                                "\"" + black_gap_mkv.string() + "\" > /dev/null 2>&1";
    int r3 = std::system(gen_black_cmd.c_str());
    assert(r3 == 0 && std::filesystem::exists(black_gap_mkv));

    mkvsplit::GapDetectionConfig bgcfg;
    bgcfg.type = mkvsplit::GapType::BlackFrame;
    bgcfg.min_gap_duration_sec = 0.3;
    bgcfg.min_segment_duration_sec = 1.0;

    std::vector<double> black_gaps = mkvsplit::MKVSplitter::DetectGaps(black_gap_mkv, bgcfg, &err);
    assert(err.empty());
    assert(!black_gaps.empty());
    std::cout << "  - Black frame episode gap detected at: " << black_gaps[0] << " s\n";
    std::cout << "PASSED: Black frame episode gap detection verified.\n\n";

    std::cout << "========================================================\n";
    std::cout << " ALL AUTOMATED TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
