#include "mkvsplit/core.hpp"
#include "mkvsplit/utils.hpp"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <filesystem>
#include <cmath>

// Unlike assert, CHECK evaluates its expression in every build configuration.
#define CHECK(expression) do { \
    if (!(expression)) { \
        throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                 ": check failed: " #expression); \
    } \
} while (false)

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
    CHECK(err.empty());
    CHECK(meta.tracks.size() >= 3);
    CHECK(meta.duration_sec >= 14.0 && meta.duration_sec <= 16.0);
    CHECK(meta.tracks[1].title == "English (Main)");
    CHECK(meta.tracks[2].title == "English (Commentary)");
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
    CHECK(plan.size() == 3);
    CHECK(plan[0].start_time_sec == 0.0 && plan[0].end_time_sec == 6.0);
    CHECK(plan[1].start_time_sec == 6.0 && plan[1].end_time_sec == 12.0);
    CHECK(plan[2].start_time_sec == 12.0);
    std::cout << "PASSED: Segment plan created 3 segments as expected.\n\n";

    std::cout << "[Test 4] Executing MKVSplitter::Split() on test file...\n";
    bool split_ok = mkvsplit::MKVSplitter::Split(config, [](const mkvsplit::ProgressInfo& info) {
        std::cout << "  - Split progress: " << static_cast<int>(info.overall_progress) << "%\n";
    }, &err);

    CHECK(split_ok);
    CHECK(err.empty());

    // Verify generated segment files and preserved audio track title metadata
    for (int i = 1; i <= 3; i++) {
        std::filesystem::path seg_file = config.output_directory / ("split_synthetic_test_part" + std::to_string(i) + ".mkv");
        CHECK(std::filesystem::exists(seg_file));
        mkvsplit::MKVMetadata seg_meta = mkvsplit::MKVSplitter::InspectFile(seg_file);
        CHECK(seg_meta.tracks.size() >= 3);
        CHECK(seg_meta.tracks[1].title == "English (Main)");
        CHECK(seg_meta.tracks[2].title == "English (Commentary)");
        CHECK(seg_meta.duration_sec > 2.0 && seg_meta.duration_sec < 7.5);
    }
    std::cout << "PASSED: All 3 split segment files created and demuxed cleanly with preserved track metadata.\n\n";

    std::cout << "[Test 5] Testing Auto Gap Detection (MKVSplitter::DetectGaps)...\n";
    std::filesystem::path silence_mkv = test_dir / "silence_gap_test.mkv";
    std::string gen_gap_cmd = "ffmpeg -y -f lavfi -i testsrc=duration=10:size=320x240:rate=30 "
                              "-f lavfi -t 10 -i anullsrc=r=44100:cl=mono "
                              "-c:v libx264 -pix_fmt yuv420p -g 30 -c:a aac "
                              "\"" + silence_mkv.string() + "\" > /dev/null 2>&1";
    int r2 = std::system(gen_gap_cmd.c_str());
    CHECK(r2 == 0 && std::filesystem::exists(silence_mkv));

    mkvsplit::GapDetectionConfig gcfg;
    gcfg.type = mkvsplit::GapType::AudioSilence;
    gcfg.noise_threshold_db = -30.0;
    gcfg.min_gap_duration_sec = 0.5;
    gcfg.min_segment_duration_sec = 1.0;

    std::vector<double> detected_gaps = mkvsplit::MKVSplitter::DetectGaps(silence_mkv, gcfg, &err);
    CHECK(err.empty());
    CHECK(!detected_gaps.empty());
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
    CHECK(r3 == 0 && std::filesystem::exists(black_gap_mkv));

    mkvsplit::GapDetectionConfig bgcfg;
    bgcfg.type = mkvsplit::GapType::BlackFrame;
    bgcfg.min_gap_duration_sec = 0.3;
    bgcfg.min_segment_duration_sec = 1.0;

    std::vector<double> black_gaps = mkvsplit::MKVSplitter::DetectGaps(black_gap_mkv, bgcfg, &err);
    CHECK(err.empty());
    CHECK(!black_gaps.empty());
    CHECK(black_gaps.size() == 1);
    CHECK(black_gaps[0] > 4.0 && black_gaps[0] < 5.0);
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
    CHECK(guess.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    CHECK(guess.recommended_config.chapter_group_count == 5);
    CHECK(guess.confidence_percent >= 90);
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
    CHECK(guess2.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    CHECK(guess2.recommended_config.chapter_group_count == 1);
    CHECK(guess2.confidence_percent == 98);
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
    CHECK(guess3.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    CHECK(guess3.recommended_config.chapter_group_count == 4);
    CHECK(guess3.recommended_config.lead_bumper_chapters == 1);
    CHECK(guess3.recommended_config.tail_bumper_chapters == 1);
    CHECK(guess3.confidence_percent >= 90);
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
    CHECK(guess4.recommended_config.mode == mkvsplit::SplitMode::Chapters);
    CHECK(guess4.recommended_config.chapter_group_count == 2);
    CHECK(guess4.recommended_config.lead_bumper_chapters == 0);
    CHECK(guess4.recommended_config.tail_bumper_chapters == 0);
    CHECK(guess4.confidence_percent >= 90);
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
    CHECK(plan_sep.size() == 6); // 1 lead + 4 eps + 1 tail
    CHECK(plan_sep[0].name_tag == "disc_intro");
    CHECK(plan_sep[0].end_time_sec == 15.0);
    CHECK(plan_sep[1].name_tag == "S01E01");
    CHECK(plan_sep[4].name_tag == "S01E04");
    CHECK(plan_sep[5].name_tag == "disc_outro");
    std::cout << "  - Separate bumpers: produced " << plan_sep.size() << " segments (disc_intro, 4 episodes, disc_outro)\n";

    // 10b: Merge bumpers
    plan_cfg.bumper_action = mkvsplit::BumperAction::Merge;
    std::vector<mkvsplit::SplitSegment> plan_merge = mkvsplit::MKVSplitter::PlanSegments(bumper_meta, plan_cfg);
    CHECK(plan_merge.size() == 4); // 4 episodes
    CHECK(plan_merge[0].name_tag == "S01E01" && plan_merge[0].start_time_sec == 0.0);
    CHECK(plan_merge[3].name_tag == "S01E04" && plan_merge[3].end_time_sec == bumper_meta.duration_sec);
    std::cout << "  - Merge bumpers: produced " << plan_merge.size() << " episodes with bumper timestamps merged\n";

    // 10c: Discard bumpers
    plan_cfg.bumper_action = mkvsplit::BumperAction::Discard;
    std::vector<mkvsplit::SplitSegment> plan_disc = mkvsplit::MKVSplitter::PlanSegments(bumper_meta, plan_cfg);
    CHECK(plan_disc.size() == 4); // 4 episodes
    CHECK(plan_disc[0].name_tag == "S01E01" && plan_disc[0].start_time_sec == 15.0);
    CHECK(plan_disc[3].name_tag == "S01E04" && plan_disc[3].end_time_sec == bumper_meta.duration_sec - 25.0);
    std::cout << "  - Discard bumpers: produced " << plan_disc.size() << " episodes with bumpers omitted\n";

    std::cout << "PASSED: Bumper actions correctly executed in PlanSegments.\n\n";

    std::cout << "[Test 11] Exact keyframe boundaries must not repeat the previous GOP...\n";
    auto long_gop = test_dir / "long_gop.mkv";
    std::string long_gop_cmd = "ffmpeg -y -v error -f lavfi -i testsrc2=duration=12:size=160x90:rate=24 "
                               "-c:v libx264 -g 120 -keyint_min 120 -sc_threshold 0 "
                               "\"" + long_gop.string() + "\"";
    CHECK(std::system(long_gop_cmd.c_str()) == 0);
    mkvsplit::SplitConfig exact_cfg;
    exact_cfg.input_file = long_gop;
    exact_cfg.output_directory = test_dir / "out_exact";
    exact_cfg.mode = mkvsplit::SplitMode::TimeRange;
    exact_cfg.range_start_sec = 5.0;
    exact_cfg.range_end_sec = 10.0;
    err.clear();
    bool exact_ok = mkvsplit::MKVSplitter::Split(exact_cfg, nullptr, &err);
    CHECK(exact_ok && err.empty());
    auto exact_meta = mkvsplit::MKVSplitter::InspectFile(exact_cfg.output_directory / "split_long_gop_part1.mkv");
    CHECK(exact_meta.duration_sec >= 4.9 && exact_meta.duration_sec < 5.2);
    std::cout << "PASSED: Exact boundary starts at the requested keyframe.\n\n";

    std::cout << "[Test 12] Filename episode lists take precedence over duration estimates...\n";
    mkvsplit::MKVMetadata named_meta;
    named_meta.duration_sec = 6 * 3000.0; // Long episodes must not be guessed as 24-minute episodes.
    for (const auto& name : {"Crying Freeman e01e02e03e04e05e06.mkv",
                             "Show.S02E07E08E09E10E11E12.mkv",
                             "Show ep01-ep02-ep03-ep04-ep05-ep06.mkv"}) {
        named_meta.file_path = name;
        auto named_guess = mkvsplit::MKVSplitter::GuessSplitConfig(named_meta);
        CHECK(named_guess.recommended_config.mode == mkvsplit::SplitMode::AutoGap);
        CHECK(named_guess.recommended_config.gap_config.expected_episodes == 6);
    }
    for (const auto& name : {"Show e01.mkv", "Show e01e03.mkv", "Show e01e01.mkv", "Movie01e02.mkv"}) {
        named_meta.file_path = name;
        CHECK(mkvsplit::MKVSplitter::GuessSplitConfig(named_meta).recommended_config.gap_config.expected_episodes == 13);
    }
    auto named_chapters = single_ch_meta;
    named_chapters.file_path = "Show S02E07E08E09E10E11E12.mkv";
    auto named_plan = mkvsplit::MKVSplitter::PlanSegments(named_chapters,
        mkvsplit::MKVSplitter::GuessSplitConfig(named_chapters).recommended_config);
    CHECK(named_plan.size() == 6);
    CHECK(named_plan.front().name_tag == "S02E07" && named_plan.back().name_tag == "S02E12");
    named_meta.file_path = "Crying Freeman e01e02e03e04e05e06.mkv";
    named_meta.duration_sec = 0.0;
    CHECK(mkvsplit::MKVSplitter::GuessSplitConfig(named_meta).recommended_config.gap_config.expected_episodes == 6);
    std::cout << "PASSED: Explicit consecutive episode lists recognized.\n\n";

    std::cout << "[Test 13] Six-episode video without chapters, duration metadata, or audio...\n";
    auto unfinalized_mkv = test_dir / "Crying Freeman e01e02e03e04e05e06.mkv";
    std::string unfinalized_cmd = "ffmpeg -y -v error -f lavfi -i color=white:s=160x90:r=10:d=36 "
        "-vf \"drawbox=c=black:t=fill:enable='gte(mod(t,6),4)*lt(t,30)'\" "
        "-c:v libx264 -g 10 -live 1 \"" + unfinalized_mkv.string() + "\"";
    int unfinalized_ret = std::system(unfinalized_cmd.c_str());
    CHECK(unfinalized_ret == 0);
    err.clear();
    auto unfinalized_meta = mkvsplit::MKVSplitter::InspectFile(unfinalized_mkv, &err);
    CHECK(err.empty());
    CHECK(unfinalized_meta.chapters.empty());
    CHECK(unfinalized_meta.tracks.size() == 1);
    CHECK(std::abs(unfinalized_meta.duration_sec - 36.0) < 0.2);
    auto six_config = mkvsplit::MKVSplitter::GuessSplitConfig(unfinalized_meta).recommended_config;
    CHECK(six_config.gap_config.expected_episodes == 6);
    six_config.gap_config.min_segment_duration_sec = 1.0;
    auto six_plan = mkvsplit::MKVSplitter::PlanSegments(unfinalized_meta, six_config);
    CHECK(six_plan.size() == 6);
    CHECK(six_plan.front().start_time_sec == 0.0);
    CHECK(six_plan.back().end_time_sec == unfinalized_meta.duration_sec);
    for (size_t i = 0; i < 5; ++i) {
        CHECK(std::abs(six_plan[i].end_time_sec - (5.0 + 6.0 * i)) < 0.2);
        CHECK(six_plan[i].end_time_sec == six_plan[i + 1].start_time_sec);
    }
    CHECK(six_plan.back().name_tag == "S01E06");
    std::cout << "PASSED: Missing duration recovered and six episodes planned using black gaps.\n\n";

    std::cout << "[Test 14] Variable chapter counts must not merge pairs of episodes...\n";
    // Chapter endpoints from the 35-chapter Crying Freeman disc. No episode
    // numbers or descriptive titles are available to guide the classifier.
    const double chapter_ends[] = {
        47.297250, 810.559750, 1537.619417, 2430.010917, 3017.973292, 3120.826042,
        3272.936333, 3791.662875, 4373.577542, 5201.863333, 6041.285250, 6158.861042,
        6794.829708, 7263.005750, 8059.634917, 9076.609208, 9228.052167,
        9590.289042, 10376.282583, 11232.429542, 12070.975583, 12267.046458,
        12355.468125, 12973.835875, 13649.761125, 14369.438417, 15041.443083, 15291.276000,
        15572.473583, 16081.315250, 16777.010250, 17526.383875, 18066.757042, 18314.296000,
        18661.664000
    };
    mkvsplit::MKVMetadata variable_meta;
    variable_meta.file_path = "Crying Freeman.mkv";
    double previous_end = 0.0;
    for (double end : chapter_ends) {
        mkvsplit::ChapterInfo chapter;
        chapter.id = static_cast<int>(variable_meta.chapters.size());
        chapter.start_time_sec = previous_end;
        chapter.end_time_sec = end;
        variable_meta.chapters.push_back(chapter);
        previous_end = end;
    }
    variable_meta.duration_sec = previous_end;
    auto variable_guess = mkvsplit::MKVSplitter::GuessSplitConfig(variable_meta);
    auto variable_config = variable_guess.recommended_config;
    CHECK(variable_config.mode == mkvsplit::SplitMode::Chapters);
    CHECK(variable_config.chapter_group_count == 0);
    CHECK((variable_config.chapter_group_sizes == std::vector<int>{6, 6, 5, 5, 6, 6}));
    CHECK(variable_config.tail_bumper_chapters == 1);
    const double expected_starts[] = {0.0, 3120.826042, 6158.861042, 9228.052167, 12267.046458, 15291.276000};
    for (auto action : {mkvsplit::BumperAction::Merge, mkvsplit::BumperAction::Separate, mkvsplit::BumperAction::Discard}) {
        variable_config.bumper_action = action;
        auto variable_plan = mkvsplit::MKVSplitter::PlanSegments(variable_meta, variable_config);
        CHECK(variable_plan.size() == (action == mkvsplit::BumperAction::Separate ? 7 : 6));
        for (size_t i = 0; i < 6; ++i) {
            CHECK(std::abs(variable_plan[i].start_time_sec - expected_starts[i]) < 0.001);
            CHECK(variable_plan[i].name_tag == "S01E0" + std::to_string(i + 1));
            if (i < 5) CHECK(variable_plan[i].end_time_sec == variable_plan[i + 1].start_time_sec);
        }
        CHECK(variable_plan[5].end_time_sec == (action == mkvsplit::BumperAction::Merge ? 18661.664000 : 18314.296000));
        if (action == mkvsplit::BumperAction::Separate) {
            CHECK(variable_plan[6].name_tag == "disc_outro");
            CHECK(variable_plan[6].start_time_sec == variable_plan[5].end_time_sec);
            CHECK(variable_plan[6].end_time_sec == variable_meta.duration_sec);
        }
    }
    // Respect an explicit filename count instead of overriding it with this heuristic.
    variable_meta.file_path = "Show E01E02E03.mkv";
    CHECK(mkvsplit::MKVSplitter::GuessSplitConfig(variable_meta).recommended_config.chapter_group_sizes.empty());
    // Uneven runtimes do not provide sufficient evidence for variable grouping.
    variable_meta.file_path = "Unrelated disc.mkv";
    for (size_t i = 0; i < 6; ++i) variable_meta.chapters[i].end_time_sec += 500.0;
    CHECK(mkvsplit::MKVSplitter::GuessSplitConfig(variable_meta).recommended_config.chapter_group_sizes.empty());
    for (const auto& bad_groups : {std::vector<int>{0}, std::vector<int>{-1}, std::vector<int>{36}}) {
        variable_config.chapter_group_sizes = bad_groups;
        CHECK(mkvsplit::MKVSplitter::PlanSegments(variable_meta, variable_config).empty());
    }
    std::cout << "PASSED: Six variable chapter groups and trailing extra planned correctly.\n\n";

    std::cout << "[Test 15] Chapter boundaries are corroborated by targeted black-frame scans...\n";
    auto corroborated_meta = unfinalized_meta;
    for (int i = 0; i < 6; ++i) {
        mkvsplit::ChapterInfo chapter;
        chapter.id = i;
        chapter.start_time_sec = i * 6.0;
        chapter.end_time_sec = (i + 1) * 6.0;
        corroborated_meta.chapters.push_back(chapter);
    }
    mkvsplit::SplitConfig corroborated_config;
    corroborated_config.mode = mkvsplit::SplitMode::Chapters;
    corroborated_config.chapter_group_count = 1;
    auto confirmed = mkvsplit::MKVSplitter::ValidateChapterBoundaries(corroborated_meta, corroborated_config);
    CHECK(confirmed.error.empty());
    CHECK((confirmed.supported_by_black == std::vector<bool>{true, true, true, true, true}));
    // A separate trailing extra must not be counted as an episode boundary.
    corroborated_config.tail_bumper_chapters = 1;
    corroborated_config.bumper_action = mkvsplit::BumperAction::Separate;
    auto with_extra = mkvsplit::MKVSplitter::ValidateChapterBoundaries(corroborated_meta, corroborated_config);
    CHECK(with_extra.error.empty() && with_extra.boundaries_sec.size() == 4);

    // Overlapping windows beginning at zero must rewind, and cannot include
    // black frames outside their requested interval due to keyframe pre-roll.
    mkvsplit::GapDetectionConfig targeted;
    targeted.min_gap_duration_sec = 0.25;
    targeted.scan_targets_sec = {3.0, 3.0};
    err.clear();
    auto repeated_gaps = mkvsplit::MKVSplitter::DetectGaps(unfinalized_mkv, targeted, &err);
    CHECK(err.empty() && repeated_gaps.size() == 2);
    targeted.scan_targets_sec = {8.0};
    targeted.scan_radius_sec = 1.0;
    CHECK(mkvsplit::MKVSplitter::DetectGaps(unfinalized_mkv, targeted, &err).empty());
    CHECK(err.empty());

    // Use long enough episodes for the auto-classifier; intentionally omit the
    // third transition. Check both 8-bit and 10-bit luminance samples.
    for (const std::string pixel_format : {"yuv420p", "yuv420p10le"}) {
        auto hybrid_mkv = test_dir / ("hybrid_" + pixel_format + ".mkv");
        std::string hybrid_cmd = "ffmpeg -y -v error -f lavfi -i color=white:s=160x90:r=4:d=720 "
            "-vf \"drawbox=c=black:t=fill:enable='gte(mod(t,120),118)*lt(t,600)*(lt(t,358)+gte(t,360))'\" "
            "-c:v libx264 -g 40 -pix_fmt " + pixel_format + " \"" + hybrid_mkv.string() + "\"";
        int hybrid_ret = std::system(hybrid_cmd.c_str());
        CHECK(hybrid_ret == 0);
        auto hybrid_meta = mkvsplit::MKVSplitter::InspectFile(hybrid_mkv);
        for (int i = 0; i < 6; ++i) {
            mkvsplit::ChapterInfo chapter;
            chapter.id = i;
            chapter.start_time_sec = i * 120.0;
            chapter.end_time_sec = (i + 1) * 120.0;
            hybrid_meta.chapters.push_back(chapter);
        }
        auto metadata_guess = mkvsplit::MKVSplitter::GuessSplitConfig(hybrid_meta);
        auto hybrid_guess = mkvsplit::MKVSplitter::GuessSplitConfig(hybrid_meta, true);
        auto hybrid_validation = mkvsplit::MKVSplitter::ValidateChapterBoundaries(hybrid_meta, hybrid_guess.recommended_config);
        CHECK(hybrid_validation.error.empty());
        CHECK((hybrid_validation.supported_by_black == std::vector<bool>{true, true, false, true, true}));
        CHECK(hybrid_guess.reasoning.find("4/5 episode boundaries") != std::string::npos);
        CHECK(hybrid_guess.reasoning.find("00:06:00.000") != std::string::npos);
        CHECK(hybrid_guess.confidence_percent < metadata_guess.confidence_percent);
        auto hybrid_plan = mkvsplit::MKVSplitter::PlanSegments(hybrid_meta, hybrid_guess.recommended_config);
        CHECK(hybrid_plan.size() == 6 && hybrid_plan[3].start_time_sec == 360.0);
        hybrid_meta.file_path = test_dir / "missing_hybrid_input.mkv";
        auto unavailable = mkvsplit::MKVSplitter::GuessSplitConfig(hybrid_meta, true);
        CHECK(unavailable.reasoning.find("validation unavailable") != std::string::npos);
        CHECK(unavailable.confidence_percent == metadata_guess.confidence_percent);
    }
    std::cout << "PASSED: Confirmed, missing, and unavailable video evidence handled without moving chapter cuts.\n\n";

    std::cout << "[Test 16] Credits, previews, optional separators, and a long finale...\n";
    // Metadata regression: variable chapter counts and a double-length finale
    // previously made this layout collapse into three fixed-size groups.
    const double gaogaigar_ends[] = {
        236.103, 764.163, 1599.331, 1726.325, 1755.887, 1761.493,
        1841.306, 1932.230, 2489.754, 3331.028, 3436.333, 3466.363, 3471.635,
        3602.399, 3694.624, 4283.780, 5062.925, 5168.030, 5203.532,
        5317.179, 5408.904, 6106.634, 6799.960, 6905.866, 6935.429, 6941.067,
        7078.038, 7170.330, 7848.107, 8560.519, 8666.625, 8696.087, 8701.826,
        8974.499, 9065.957, 9484.475, 10463.687, 10569.259, 10598.922, 10604.694,
        11055.578, 11147.436, 11798.687, 12395.917, 12511.032, 12541.429, 12547.035,
        12763.818, 12933.087, 13865.819, 14532.651, 15377.629, 15625.476
    };
    mkvsplit::MKVMetadata preview_meta;
    preview_meta.file_path = "Show - S02E01.mkv";
    previous_end = 0.0;
    for (double end : gaogaigar_ends) {
        mkvsplit::ChapterInfo chapter;
        chapter.id = static_cast<int>(preview_meta.chapters.size());
        chapter.start_time_sec = previous_end;
        chapter.end_time_sec = end;
        preview_meta.chapters.push_back(chapter);
        previous_end = end;
    }
    preview_meta.duration_sec = 15625.512;
    auto preview_guess = mkvsplit::MKVSplitter::GuessSplitConfig(preview_meta);
    CHECK((preview_guess.recommended_config.chapter_group_sizes == std::vector<int>{6, 7, 6, 7, 7, 7, 7, 6}));
    CHECK(preview_guess.recommended_config.tail_bumper_chapters == 0);
    const double preview_starts[] = {0.0, 1761.493, 3471.635, 5203.532, 6941.067, 8701.826, 10604.694, 12547.035};
    for (auto action : {mkvsplit::BumperAction::Merge, mkvsplit::BumperAction::Separate, mkvsplit::BumperAction::Discard}) {
        auto cfg = preview_guess.recommended_config;
        cfg.bumper_action = action;
        auto preview_plan = mkvsplit::MKVSplitter::PlanSegments(preview_meta, cfg);
        CHECK(preview_plan.size() == 8);
        for (size_t i = 0; i < 8; ++i) {
            CHECK(preview_plan[i].start_time_sec == preview_starts[i]);
            CHECK(preview_plan[i].name_tag == "S02E0" + std::to_string(i + 1));
            if (i < 7) CHECK(preview_plan[i].end_time_sec == preview_plan[i + 1].start_time_sec);
        }
        CHECK(preview_plan.back().end_time_sec == preview_meta.duration_sec);
        CHECK(preview_plan.back().end_time_sec - preview_plan.back().start_time_sec > 3000.0);
    }
    std::cout << "PASSED: Eight groups retain the previews and the complete long finale.\n\n";

    std::cout << "[Test 17] Repeated ending detection requires consistent evidence...\n";
    auto make_chapter_metadata = [](const std::vector<double>& durations) {
        mkvsplit::MKVMetadata metadata;
        metadata.file_path = "Unrelated disc.mkv";
        for (double duration : durations) {
            mkvsplit::ChapterInfo chapter;
            chapter.id = static_cast<int>(metadata.chapters.size());
            chapter.start_time_sec = metadata.duration_sec;
            metadata.duration_sec += duration;
            chapter.end_time_sec = metadata.duration_sec;
            metadata.chapters.push_back(chapter);
        }
        return metadata;
    };
    std::vector<double> preview_durations = {
        90, 450, 750, 100, 30, 5,
        70, 90, 500, 600, 110, 32,
        80, 90, 450, 750, 105, 29, 6,
        150, 90, 700, 750, 600, 150
    };
    auto unrelated_meta = make_chapter_metadata(preview_durations);
    auto unrelated_guess = mkvsplit::MKVSplitter::GuessSplitConfig(unrelated_meta);
    CHECK((unrelated_guess.recommended_config.chapter_group_sizes == std::vector<int>{6, 6, 7, 6}));
    auto has_preview_pattern = [](const mkvsplit::MKVMetadata& metadata) {
        return mkvsplit::MKVSplitter::GuessSplitConfig(metadata).reasoning.find("credits/preview") != std::string::npos;
    };
    unrelated_meta.file_path = "Show E01E02E03.mkv";
    CHECK(!has_preview_pattern(unrelated_meta));
    auto inconsistent_durations = preview_durations;
    inconsistent_durations[4] = 12.0;
    inconsistent_durations[11] = 55.0;
    CHECK(!has_preview_pattern(make_chapter_metadata(inconsistent_durations)));
    auto oversized_finale = preview_durations;
    oversized_finale.back() += 5000.0;
    CHECK(!has_preview_pattern(make_chapter_metadata(oversized_finale)));
    std::vector<double> two_endings(preview_durations.begin(), preview_durations.begin() + 12);
    CHECK(!has_preview_pattern(make_chapter_metadata(two_endings)));
    std::vector<double> with_bonus(preview_durations.begin(), preview_durations.begin() + 19);
    with_bonus.push_back(40.0);
    auto bonus_guess = mkvsplit::MKVSplitter::GuessSplitConfig(make_chapter_metadata(with_bonus));
    CHECK((bonus_guess.recommended_config.chapter_group_sizes == std::vector<int>{6, 6, 7}));
    CHECK(bonus_guess.recommended_config.tail_bumper_chapters == 1);
    std::cout << "PASSED: Generalized pattern, conflicting count, inconsistent endings, and trailing bonus checked.\n\n";

    std::cout << "========================================================\n";
    std::cout << " ALL AUTOMATED TESTS COMPLETED SUCCESSFULLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
