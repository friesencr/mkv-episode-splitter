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

    std::cout << "[Test 7] Testing Cyclical Pattern Auto-Guessing (MKVSplitter::GuessSplitConfig)...\n";
    mkvsplit::MKVMetadata cyclic_meta;
    cyclic_meta.file_path = "synthetic_multi_episode.mkv";
    
    // Simulate 20 chapters over 4 episodes (5 chapters per episode)
    // Structure per episode: OP (90s), Act 1 (500s), Act 2 (500s), Act 3 (300s), ED (90s) -> total 1480s (~24.6m) per episode
    double cur_t = 0.0;
    double ep_pattern[5] = {90.0, 500.0, 500.0, 300.0, 90.0};
    for (int ep = 0; ep < 4; ep++) {
        for (int ch = 0; ch < 5; ch++) {
            mkvsplit::ChapterInfo cinfo;
            cinfo.id = ep * 5 + ch;
            cinfo.start_time_sec = cur_t;
            cur_t += ep_pattern[ch];
            cinfo.end_time_sec = cur_t;
            cinfo.title = "Chapter " + std::to_string(cinfo.id + 1);
            cyclic_meta.chapters.push_back(cinfo);
        }
    }
    cyclic_meta.duration_sec = cur_t;

    mkvsplit::AutoGuessResult guess = mkvsplit::MKVSplitter::GuessSplitConfig(cyclic_meta);
    assert(guess.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    assert(guess.recommended_config.chapter_group_count == 5);
    assert(guess.confidence_percent >= 90);
    std::cout << "  - Recommended mode: Chapters (Group count: " << guess.recommended_config.chapter_group_count << ")\n";
    std::cout << "  - Confidence: " << guess.confidence_percent << "%\n";
    std::cout << "  - Reasoning: " << guess.reasoning << "\n";

    // Test Case 7b: 1-chapter per episode cyclic detection (6 episodes of 24m)
    mkvsplit::MKVMetadata single_ch_meta;
    single_ch_meta.file_path = "synthetic_single_ch.mkv";
    double cur_t2 = 0.0;
    for (int ch = 0; ch < 6; ch++) {
        mkvsplit::ChapterInfo cinfo;
        cinfo.id = ch;
        cinfo.start_time_sec = cur_t2;
        cur_t2 += 1440.0; // 24 mins
        cinfo.end_time_sec = cur_t2;
        cinfo.title = "Episode " + std::to_string(ch + 1);
        single_ch_meta.chapters.push_back(cinfo);
    }
    single_ch_meta.duration_sec = cur_t2;

    mkvsplit::AutoGuessResult guess2 = mkvsplit::MKVSplitter::GuessSplitConfig(single_ch_meta);
    assert(guess2.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    assert(guess2.recommended_config.chapter_group_count == 1);
    assert(guess2.confidence_percent == 98);
    std::cout << "  - Single chapter per ep detected: Group count = " << guess2.recommended_config.chapter_group_count << " (Confidence: " << guess2.confidence_percent << "%)\n";

    std::cout << "PASSED: Cyclical pattern auto-guessing correctly identified 5 chapters per episode and single-chapter episode structures.\n\n";

    std::cout << "[Test 8] Testing Brute-Force Chapter Symmetry with Lead & Tail Bumpers...\n";
    mkvsplit::MKVMetadata bumper_meta;
    bumper_meta.file_path = "synthetic_bumper_disc.mkv";
    double cur_t3 = 0.0;

    // Lead bumper: 15s studio logo
    mkvsplit::ChapterInfo lead_ch;
    lead_ch.id = 0;
    lead_ch.start_time_sec = cur_t3;
    cur_t3 += 15.0;
    lead_ch.end_time_sec = cur_t3;
    lead_ch.title = "Studio Logo";
    bumper_meta.chapters.push_back(lead_ch);

    // 4 episodes of 4 chapters each: OP(90s), Act 1(600s), Act 2(600s), ED(90s) = 1380s (~23m)
    double b_ep_pattern[4] = {90.0, 600.0, 600.0, 90.0};
    for (int ep = 0; ep < 4; ep++) {
        for (int ch = 0; ch < 4; ch++) {
            mkvsplit::ChapterInfo cinfo;
            cinfo.id = 1 + ep * 4 + ch;
            cinfo.start_time_sec = cur_t3;
            cur_t3 += b_ep_pattern[ch];
            cinfo.end_time_sec = cur_t3;
            cinfo.title = "Ep " + std::to_string(ep + 1) + " Part " + std::to_string(ch + 1);
            bumper_meta.chapters.push_back(cinfo);
        }
    }

    // Tail bumper: 25s copyright screen
    mkvsplit::ChapterInfo tail_ch;
    tail_ch.id = 17;
    tail_ch.start_time_sec = cur_t3;
    cur_t3 += 25.0;
    tail_ch.end_time_sec = cur_t3;
    tail_ch.title = "Disc Outro";
    bumper_meta.chapters.push_back(tail_ch);
    bumper_meta.duration_sec = cur_t3;

    mkvsplit::AutoGuessResult guess3 = mkvsplit::MKVSplitter::GuessSplitConfig(bumper_meta);
    assert(guess3.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    assert(guess3.recommended_config.chapter_group_count == 4);
    assert(guess3.recommended_config.lead_bumper_chapters == 1);
    assert(guess3.recommended_config.tail_bumper_chapters == 1);
    assert(guess3.confidence_percent >= 90);
    std::cout << "  - Auto-detected: " << guess3.recommended_config.lead_bumper_chapters << " lead bumper, "
              << guess3.recommended_config.tail_bumper_chapters << " tail bumper, "
              << guess3.recommended_config.chapter_group_count << " chapters/episode\n";
    std::cout << "  - Confidence: " << guess3.confidence_percent << "%\n";
    std::cout << "  - Reasoning: " << guess3.reasoning << "\n";
    std::cout << "PASSED: Lead and tail bumpers successfully identified by brute force search.\n\n";

    std::cout << "[Test 9] Testing Short-Format / Mrs. Pepperpot Symmetry with Duration Jitter (Wiggle Room)...\n";
    mkvsplit::MKVMetadata pepperpot_meta;
    pepperpot_meta.file_path = "mrs_pepperpot_disc.mkv";
    double cur_t4 = 0.0;
    // 8 episodes of ~7.5 minutes each with realistic duration jitter (+/- 20s)
    double ep_stories[8] = {390.0, 415.0, 395.0, 410.0, 403.0, 407.0, 394.0, 420.0};
    for (int ep = 0; ep < 8; ep++) {
        // Ch 1: Theme song (45s)
        mkvsplit::ChapterInfo op_ch;
        op_ch.id = ep * 2;
        op_ch.start_time_sec = cur_t4;
        cur_t4 += 45.0;
        op_ch.end_time_sec = cur_t4;
        op_ch.title = "Episode " + std::to_string(ep + 1) + " OP";
        pepperpot_meta.chapters.push_back(op_ch);

        // Ch 2: Story
        mkvsplit::ChapterInfo st_ch;
        st_ch.id = ep * 2 + 1;
        st_ch.start_time_sec = cur_t4;
        cur_t4 += ep_stories[ep];
        st_ch.end_time_sec = cur_t4;
        st_ch.title = "Episode " + std::to_string(ep + 1) + " Story";
        pepperpot_meta.chapters.push_back(st_ch);
    }
    pepperpot_meta.duration_sec = cur_t4;

    mkvsplit::AutoGuessResult guess4 = mkvsplit::MKVSplitter::GuessSplitConfig(pepperpot_meta);
    assert(guess4.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    assert(guess4.recommended_config.chapter_group_count == 2);
    assert(guess4.recommended_config.lead_bumper_chapters == 0);
    assert(guess4.recommended_config.tail_bumper_chapters == 0);
    assert(guess4.confidence_percent >= 90);
    std::cout << "  - Auto-detected: " << guess4.recommended_config.chapter_group_count << " chapters/episode across 8 episodes\n";
    std::cout << "  - Confidence: " << guess4.confidence_percent << "%\n";
    std::cout << "  - Reasoning: " << guess4.reasoning << "\n";
    std::cout << "PASSED: Short-format show (Mrs. Pepperpot style) detected with high confidence despite duration jitter.\n\n";

    std::cout << "[Test 10] Testing Segment Planning with Bumper Actions (Separate, Merge, Discard)...\n";
    mkvsplit::SplitConfig plan_cfg = guess3.recommended_config;
    plan_cfg.input_file = "synthetic_bumper_disc.mkv";

    // 10a: Separate bumpers
    plan_cfg.bumper_action = mkvsplit::BumperAction::Separate;
    std::vector<mkvsplit::SplitSegment> plan_sep = mkvsplit::MKVSplitter::PlanSegments(bumper_meta, plan_cfg);
    assert(plan_sep.size() == 6); // 1 lead + 4 eps + 1 tail
    assert(plan_sep[0].name_tag == "disc_intro");
    assert(plan_sep[0].end_time_sec == 15.0);
    assert(plan_sep[1].name_tag == "S01E01");
    assert(plan_sep[4].name_tag == "S01E04");
    assert(plan_sep[5].name_tag == "disc_outro");
    std::cout << "  - Separate bumpers: produced " << plan_sep.size() << " segments (disc_intro, 4 episodes, disc_outro)\n";

    // 10b: Merge bumpers
    plan_cfg.bumper_action = mkvsplit::BumperAction::Merge;
    std::vector<mkvsplit::SplitSegment> plan_merge = mkvsplit::MKVSplitter::PlanSegments(bumper_meta, plan_cfg);
    assert(plan_merge.size() == 4); // 4 episodes
    assert(plan_merge[0].name_tag == "S01E01" && plan_merge[0].start_time_sec == 0.0);
    assert(plan_merge[3].name_tag == "S01E04" && plan_merge[3].end_time_sec == bumper_meta.duration_sec);
    std::cout << "  - Merge bumpers: produced " << plan_merge.size() << " episodes with bumper timestamps merged\n";

    // 10c: Discard bumpers
    plan_cfg.bumper_action = mkvsplit::BumperAction::Discard;
    std::vector<mkvsplit::SplitSegment> plan_disc = mkvsplit::MKVSplitter::PlanSegments(bumper_meta, plan_cfg);
    assert(plan_disc.size() == 4); // 4 episodes
    assert(plan_disc[0].name_tag == "S01E01" && plan_disc[0].start_time_sec == 15.0);
    assert(plan_disc[3].name_tag == "S01E04" && plan_disc[3].end_time_sec == bumper_meta.duration_sec - 25.0);
    std::cout << "  - Discard bumpers: produced " << plan_disc.size() << " episodes with bumpers omitted\n";

    std::cout << "PASSED: Bumper actions correctly executed in PlanSegments.\n\n";

    std::cout << "[Test 11] Exact keyframe boundaries must not repeat the previous GOP...\n";
    auto long_gop = test_dir / "long_gop.mkv";
    std::string long_gop_cmd = "ffmpeg -y -v error -f lavfi -i testsrc2=duration=12:size=160x90:rate=24 "
                               "-c:v libx264 -g 120 -keyint_min 120 -sc_threshold 0 "
                               "\"" + long_gop.string() + "\"";
    assert(std::system(long_gop_cmd.c_str()) == 0);
    mkvsplit::SplitConfig exact_cfg;
    exact_cfg.input_file = long_gop;
    exact_cfg.output_directory = test_dir / "out_exact";
    exact_cfg.mode = mkvsplit::SplitMode::TimeRange;
    exact_cfg.range_start_sec = 5.0;
    exact_cfg.range_end_sec = 10.0;
    err.clear();
    bool exact_ok = mkvsplit::MKVSplitter::Split(exact_cfg, nullptr, &err);
    assert(exact_ok && err.empty());
    auto exact_meta = mkvsplit::MKVSplitter::InspectFile(exact_cfg.output_directory / "split_long_gop_part1.mkv");
    assert(exact_meta.duration_sec >= 4.9 && exact_meta.duration_sec < 5.2);
    std::cout << "PASSED: Exact boundary starts at the requested keyframe.\n\n";

    std::cout << "========================================================\n";
    std::cout << " ALL AUTOMATED TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
