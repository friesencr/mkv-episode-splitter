#include "mkvsplit/core.hpp"
#include "mkvsplit/utils.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <iomanip>

static std::string CleanPath(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();

    // Strip file:// scheme (common when dragging from Nautilus/Dolphin/Explorer)
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
    }

    // Strip leading/trailing quotes
    while (!s.empty() && (s.front() == '\'' || s.front() == '"')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '\'' || s.back() == '"')) s.pop_back();

    // Strip backslash escapes added by Linux/macOS terminal drag-and-drop
    std::string clean;
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] == '\\' && i + 1 < s.length() && (s[i+1] == ' ' || s[i+1] == '\'' || s[i+1] == '"' || s[i+1] == '(' || s[i+1] == ')')) {
            continue;
        }
        clean += s[i];
    }
    return clean;
}

void PrintUsage(const char* prog) {
    std::cout << "mkvsplit-cli - High Performance Cross-Platform MKV Splitter\n\n"
              << "Usage: " << prog << " [options] [input.mkv]\n\n"
              << "Options:\n"
              << "  -i, --input <path>      Path to input .mkv file\n"
              << "  -I, --interactive       Launch interactive drag-and-drop mode\n"
              << "  -o, --output-dir <path> Target output directory (default: ./output)\n"
              << "  -m, --mode <mode>       Splitting mode: range | duration | size | chapters | auto (default: range)\n"
              << "  -s, --start <time>      Start time for range mode (HH:MM:SS.mmm or seconds, default: 00:00:00)\n"
              << "  -e, --end <time>        End time for range mode (HH:MM:SS.mmm or seconds, default: end of file)\n"
              << "  -d, --duration <sec>    Chunk duration in seconds for duration mode (default: 600)\n"
              << "  -z, --size <MB>         Target chunk size in MB for size mode (default: 500)\n"
              << "  -c, --chapters <ids>    Comma-separated list of chapter numbers (e.g. 1,2,4)\n"
              << "  -g, --group-chapters <K> Group K chapters per episode segment (e.g. 4 or 6)\n"
              << "  -n, --episodes <N>      Expected number of episodes in multi-episode file (optional)\n"
              << "  --gap-type <type>       Auto gap detection type: black | silence | combined (default: black)\n"
              << "  --min-gap <sec>         Min gap/black screen duration in sec for auto mode (default: 2.0)\n"
              << "  --min-segment <sec>     Min episode segment duration in sec for auto mode (default: 10.0)\n"
              << "  --noise-db <dB>         Audio noise floor threshold in dB for audio gap mode (default: -30)\n"
              << "  --bumpers <action>      Handling for intro/outro bumpers: merge | separate | discard (default: merge)\n"
              << "  --lead-bumpers <L>      Leading bumper chapters to separate/merge/discard (default: auto or 0)\n"
              << "  --tail-bumpers <T>      Trailing bumper chapters to separate/merge/discard (default: auto or 0)\n"
              << "  --season <N>            Season number for Plex/Kodi/Jellyfin naming (default: auto-detect or 1)\n"
              << "  --start-episode <N>     Starting episode number for Plex/Kodi/Jellyfin naming (default: auto-detect or 1)\n"
              << "  --info                  Display MKV file details, tracks, and chapters without splitting\n"
              << "  -h, --help              Show this help message\n\n"
              << "Examples:\n"
              << "  " << prog << "                            # Interactive mode with file drag-and-drop\n"
              << "  " << prog << " -i multi_episode.mkv -m auto --bumpers separate\n"
              << "  " << prog << " -i multi_episode.mkv -m chapters -g 4 --lead-bumpers 1 --tail-bumpers 1\n"
              << "  " << prog << " -i \"GaoGaiGar - S01E39.mkv\" -m chapters -g 6\n"
              << "  " << prog << " -i video.mkv --info\n";
}

static void PrintEpisodeCountMismatch(const mkvsplit::SplitConfig& config,
                                      const std::vector<mkvsplit::SplitSegment>& plan) {
    if (config.mode != mkvsplit::SplitMode::AutoGap || config.gap_config.expected_episodes <= 1) return;
    size_t episodes = 0;
    for (const auto& segment : plan) {
        if (segment.name_tag != "disc_intro" && segment.name_tag != "disc_outro") ++episodes;
    }
    if (episodes != static_cast<size_t>(config.gap_config.expected_episodes)) {
        std::cout << " Could identify " << episodes << " episode segment(s), but expected "
                  << config.gap_config.expected_episodes
                  << ". Check the source file and gap settings before splitting.\n";
    }
}

int RunInteractiveMode(std::string input_file = "") {
    std::cout << "========================================================\n";
    std::cout << "  MKV SPLITTER — Interactive Mode\n";
    std::cout << "========================================================\n\n";

    input_file = CleanPath(input_file);
    while (input_file.empty()) {
        std::cout << "Drag and drop an MKV file here (or type path): ";
        std::string raw;
        if (!std::getline(std::cin, raw)) return 0;
        input_file = CleanPath(raw);
        if (input_file.empty()) continue;
        if (!std::filesystem::exists(input_file)) {
            std::cout << "Error: File '" << input_file << "' does not exist. Try again.\n\n";
            input_file.clear();
        }
    }

    std::string err_msg;
    mkvsplit::MKVMetadata meta = mkvsplit::MKVSplitter::InspectFile(input_file, &err_msg);
    if (meta.tracks.empty()) {
        std::cerr << "Error inspecting file: " << (err_msg.empty() ? "Unable to read MKV metadata" : err_msg) << "\n";
        return 1;
    }

    std::cout << "Analyzing chapters and checking nearby black transitions...\n" << std::flush;
    mkvsplit::AutoGuessResult guess = mkvsplit::MKVSplitter::GuessSplitConfig(meta, true);

    std::cout << "\n========================================================\n";
    std::cout << " FILE METADATA SUMMARY\n";
    std::cout << "========================================================\n";
    std::cout << " File:     " << meta.file_path.filename().string() << "\n";
    std::cout << " Size:     " << mkvsplit::FormatBytes(meta.file_size_bytes) << "\n";
    std::cout << " Duration: " << mkvsplit::FormatDuration(meta.duration_sec) << " (" << meta.duration_sec << " s)\n";
    std::cout << " Tracks:   " << meta.tracks.size() << " total\n";
    for (const auto& tr : meta.tracks) {
        std::cout << "   - Stream #" << tr.index << " [" << tr.media_type << "] Codec: " << tr.codec_name;
        if (tr.media_type == "video") std::cout << " (" << tr.width << "x" << tr.height << ")";
        if (tr.media_type == "audio") std::cout << " (" << tr.sample_rate << " Hz, " << tr.channels << " ch)";
        std::cout << " lang: " << tr.language;
        if (!tr.title.empty()) std::cout << " title: \"" << tr.title << "\"";
        std::cout << "\n";
    }
    std::cout << " Chapters: " << meta.chapters.size() << " total\n";
    std::cout << " Auto-Classifier Recommendation (" << guess.confidence_percent << "% Confidence):\n";
    std::cout << "   " << guess.reasoning << "\n";
    std::cout << "========================================================\n\n";

    mkvsplit::SplitConfig config;
    config.input_file = input_file;
    config.output_directory = "./output";

    config = guess.recommended_config;
    config.input_file = input_file;
    config.output_directory = "./output";

    bool configured = false;
    while (!configured) {
        std::vector<mkvsplit::SplitSegment> plan = mkvsplit::MKVSplitter::PlanSegments(meta, config);

        std::cout << "--------------------------------------------------------\n";
        std::cout << " CONFIGURATION & PREVIEW PLAN (" << plan.size() << " episode segment" << (plan.size() == 1 ? "" : "s") << ")\n";
        PrintEpisodeCountMismatch(config, plan);
        std::cout << "--------------------------------------------------------\n";
        std::string mode_desc;
        if (config.mode == mkvsplit::SplitMode::Chapters) {
            mode_desc = "Chapters (" + std::to_string(config.chapter_group_count) + " ch/ep";
            if (config.chapter_group_count == 0 && !config.chapter_group_sizes.empty()) {
                mode_desc = "Chapters (variable grouping: ";
                for (size_t i = 0; i < config.chapter_group_sizes.size(); ++i) {
                    if (i > 0) mode_desc += ", ";
                    mode_desc += std::to_string(config.chapter_group_sizes[i]);
                }
                mode_desc += " ch/ep";
            }
            if (config.lead_bumper_chapters > 0 || config.tail_bumper_chapters > 0) {
                mode_desc += ", Bumpers: lead=" + std::to_string(config.lead_bumper_chapters) + " ch, tail=" + std::to_string(config.tail_bumper_chapters) + " ch";
            }
            mode_desc += ")";
        } else if (config.mode == mkvsplit::SplitMode::AutoGap) {
            mode_desc = "Auto Black-Frame Gap (Expected " + std::to_string(config.gap_config.expected_episodes) + " eps)";
        } else if (config.mode == mkvsplit::SplitMode::FixedDuration) {
            mode_desc = "Fixed Duration (" + std::to_string(config.chunk_duration_sec) + "s)";
        } else {
            mode_desc = "Time Range";
        }

        std::cout << " [1] Strategy Mode:     " << mode_desc << "\n";
        std::cout << " [2] Bumper Handling:   " 
                  << (config.bumper_action == mkvsplit::BumperAction::Merge ? "Merge intro/outro into adjacent episode (Recommended)" :
                     config.bumper_action == mkvsplit::BumperAction::Separate ? "Export disc intro/outro as separate files (disc_intro.mkv / disc_outro.mkv)" : "Discard intro/outro bumpers") << "\n";
        std::cout << " [3] Season & Start Ep: Season " << (config.season_number > 0 ? config.season_number : 1) 
                  << ", Start Ep " << (config.start_episode_number > 0 ? config.start_episode_number : 1) << " (Plex/Kodi Format)\n";
        std::cout << " [4] Output Directory:  " << config.output_directory.string() << "\n";
        std::cout << "--------------------------------------------------------\n";
        std::cout << " PREVIEW SPLIT FILES:\n";
        int preview_count = std::min(5, static_cast<int>(plan.size()));
        for (int i = 0; i < preview_count; i++) {
            std::cout << "   " << (i + 1) << ". " << plan[i].name_tag << " (" << mkvsplit::SecondsToTimeString(plan[i].start_time_sec)
                      << " -> " << mkvsplit::SecondsToTimeString(plan[i].end_time_sec) << ")\n";
        }
        if (plan.size() > 5) {
            std::cout << "   ... and " << (plan.size() - 5) << " more segment(s)\n";
        }
        std::cout << "--------------------------------------------------------\n";
        std::cout << " Press [ENTER] to start split, or type option # (1-4) to adjust settings: ";

        std::string choice;
        if (!std::getline(std::cin, choice)) return 0;
        choice = CleanPath(choice);

        if (choice.empty()) {
            configured = true;
        } else if (choice == "1") {
            std::cout << "\nSelect Strategy Mode:\n";
            std::cout << " [1] Auto-Guess Classifier (Recommended)\n";
            std::cout << " [2] Chapter Grouping (Group K chapters into 1 episode)\n";
            std::cout << " [3] Black Frame Gap Detection\n";
            std::cout << " [4] Audio Silence Gap Detection\n";
            std::cout << " [5] Fixed Duration Chunks\n";
            std::cout << "Choice (1-5): ";
            std::string smode;
            std::getline(std::cin, smode);
            smode = CleanPath(smode);
            if (smode == "1") {
                config = guess.recommended_config;
                config.input_file = input_file;
                config.output_directory = "./output";
            } else if (smode == "2") {
                config.mode = mkvsplit::SplitMode::Chapters;
                std::cout << "Enter chapters per episode (e.g. 1, 4, 6): ";
                std::string cg;
                std::getline(std::cin, cg);
                try { config.chapter_group_count = std::stoi(CleanPath(cg)); } catch (...) {}
                config.chapter_group_sizes.clear();
            } else if (smode == "3") {
                config.mode = mkvsplit::SplitMode::AutoGap;
                config.gap_config.type = mkvsplit::GapType::BlackFrame;
                std::cout << "Enter expected episode count (0 = auto): ";
                std::string epc;
                std::getline(std::cin, epc);
                try { config.gap_config.expected_episodes = std::stoi(CleanPath(epc)); } catch (...) {}
            } else if (smode == "4") {
                config.mode = mkvsplit::SplitMode::AutoGap;
                config.gap_config.type = mkvsplit::GapType::AudioSilence;
            } else if (smode == "5") {
                config.mode = mkvsplit::SplitMode::FixedDuration;
                std::cout << "Enter chunk duration in seconds (e.g. 600): ";
                std::string cd;
                std::getline(std::cin, cd);
                try { config.chunk_duration_sec = std::stod(CleanPath(cd)); } catch (...) {}
            }
        } else if (choice == "2") {
            std::cout << "\nSelect Bumper Handling:\n";
            std::cout << " [1] Merge intro/outro bumpers into adjacent episode (Recommended)\n";
            std::cout << " [2] Export disc intro/outro as separate files (disc_intro.mkv / disc_outro.mkv)\n";
            std::cout << " [3] Discard intro/outro bumpers entirely\n";
            std::cout << " [4] Set lead/tail bumper chapter counts (Current: lead=" << config.lead_bumper_chapters << ", tail=" << config.tail_bumper_chapters << ")\n";
            std::cout << "Choice (1-4): ";
            std::string bchoice;
            std::getline(std::cin, bchoice);
            bchoice = CleanPath(bchoice);
            if (bchoice == "1") {
                config.bumper_action = mkvsplit::BumperAction::Merge;
            } else if (bchoice == "2") {
                config.bumper_action = mkvsplit::BumperAction::Separate;
            } else if (bchoice == "3") {
                config.bumper_action = mkvsplit::BumperAction::Discard;
            } else if (bchoice == "4") {
                std::cout << "Enter lead bumper chapters (0-3): ";
                std::string lb; std::getline(std::cin, lb);
                try { config.lead_bumper_chapters = std::stoi(CleanPath(lb)); } catch (...) {}
                std::cout << "Enter tail bumper chapters (0-3): ";
                std::string tb; std::getline(std::cin, tb);
                try { config.tail_bumper_chapters = std::stoi(CleanPath(tb)); } catch (...) {}
            }
        } else if (choice == "3") {
            std::cout << "\nEnter Season Number (0 = auto-detect): ";
            std::string sn;
            std::getline(std::cin, sn);
            try { config.season_number = std::stoi(CleanPath(sn)); } catch (...) {}

            std::cout << "Enter Start Episode Number (0 = auto-detect): ";
            std::string en;
            std::getline(std::cin, en);
            try { config.start_episode_number = std::stoi(CleanPath(en)); } catch (...) {}
        } else if (choice == "4") {
            std::cout << "\nEnter Output Directory path: ";
            std::string od;
            std::getline(std::cin, od);
            od = CleanPath(od);
            if (!od.empty()) config.output_directory = od;
        }
    }

    std::cout << "\n========================================================\n";
    std::cout << " EXECUTING SPLIT OPERATION...\n";
    std::cout << "========================================================\n";

    auto progress_cb = [](const mkvsplit::ProgressInfo& info) {
        int bar_width = 18;
        int filled = static_cast<int>((info.overall_progress / 100.0) * bar_width);
        std::string bar = std::string(filled, '=') + (filled < bar_width ? ">" : "") + std::string(std::max(0, bar_width - filled - 1), ' ');
        std::string truncated_fn = mkvsplit::TruncateString(info.current_output_filename, 22);

        std::ostringstream ss;
        ss << "[" << bar << "] " << std::setw(3) << static_cast<int>(info.overall_progress) << "% | "
           << "Seg " << info.current_segment_index << "/" << info.total_segments << " (" << truncated_fn << ") - "
           << mkvsplit::FormatBytes(info.bytes_written) << " written";

        std::string line = ss.str();
        if (line.length() > 75) line = line.substr(0, 75);

        std::cout << "\r" << line << "\033[K" << std::flush;
    };

    bool success = mkvsplit::MKVSplitter::Split(config, progress_cb, &err_msg);
    std::cout << "\n========================================================\n";

    if (success) {
        std::cout << " SUCCESS: Split completed! Files saved to: " << config.output_directory.string() << "\n";
        std::cout << "========================================================\n";
    } else {
        std::cerr << " ERROR during split: " << err_msg << "\n";
        std::cout << "========================================================\n";
    }

    std::cout << "\nPress [ENTER] to exit...";
    std::string dummy;
    std::getline(std::cin, dummy);
    return success ? 0 : 1;
}

int main(int argc, char* argv[]) {
    bool interactive = false;
    std::string input_file;
    std::string output_dir = "./output";
    std::string mode_str = "range";
    std::string start_str = "0";
    std::string end_str = "0";
    double chunk_duration = 600.0;
    uint64_t chunk_size_mb = 500;
    std::string chapters_str;
    int group_chapters = 0;
    int expected_eps = 0;
    int season_num = 0;
    int start_ep_num = 0;
    std::string gap_type_str = "black";
    std::string bumpers_str = "merge";
    int lead_bumpers = -1;
    int tail_bumpers = -1;
    double min_gap = 2.0;
    double min_segment = 10.0;
    double noise_db = -30.0;
    bool info_only = false;

    if (argc < 2) {
        return RunInteractiveMode();
    }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            input_file = argv[++i];
        } else if (arg == "-I" || arg == "--interactive") {
            interactive = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                input_file = argv[++i];
            }
        } else if ((arg == "-o" || arg == "--output-dir") && i + 1 < argc) {
            output_dir = argv[++i];
        } else if ((arg == "-m" || arg == "--mode") && i + 1 < argc) {
            mode_str = argv[++i];
        } else if ((arg == "-s" || arg == "--start") && i + 1 < argc) {
            start_str = argv[++i];
        } else if ((arg == "-e" || arg == "--end") && i + 1 < argc) {
            end_str = argv[++i];
        } else if ((arg == "-d" || arg == "--duration") && i + 1 < argc) {
            chunk_duration = std::stod(argv[++i]);
        } else if ((arg == "-z" || arg == "--size") && i + 1 < argc) {
            chunk_size_mb = std::stoull(argv[++i]);
        } else if ((arg == "-c" || arg == "--chapters") && i + 1 < argc) {
            chapters_str = argv[++i];
        } else if ((arg == "-g" || arg == "--group-chapters") && i + 1 < argc) {
            group_chapters = std::stoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--episodes") && i + 1 < argc) {
            expected_eps = std::stoi(argv[++i]);
        } else if (arg == "--season" && i + 1 < argc) {
            season_num = std::stoi(argv[++i]);
        } else if ((arg == "--start-episode" || arg == "--start-ep") && i + 1 < argc) {
            start_ep_num = std::stoi(argv[++i]);
        } else if (arg == "--gap-type" && i + 1 < argc) {
            gap_type_str = argv[++i];
        } else if (arg == "--bumpers" && i + 1 < argc) {
            bumpers_str = argv[++i];
        } else if (arg == "--lead-bumpers" && i + 1 < argc) {
            lead_bumpers = std::stoi(argv[++i]);
        } else if (arg == "--tail-bumpers" && i + 1 < argc) {
            tail_bumpers = std::stoi(argv[++i]);
        } else if ((arg == "--min-gap" || arg == "--min-silence") && i + 1 < argc) {
            min_gap = std::stod(argv[++i]);
        } else if (arg == "--min-segment" && i + 1 < argc) {
            min_segment = std::stod(argv[++i]);
        } else if (arg == "--noise-db" && i + 1 < argc) {
            noise_db = std::stod(argv[++i]);
        } else if (arg == "--info") {
            info_only = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (i == 1 && arg[0] != '-') {
            // Positional input file argument e.g. `./mkvsplit-cli file.mkv`
            input_file = arg;
        }
    }

    // If interactive mode requested OR if single file path passed (e.g. drag & drop onto executable icon)
    if (interactive || (argc <= 2 && !input_file.empty())) {
        return RunInteractiveMode(input_file);
    }

    if (input_file.empty()) {
        std::cerr << "Error: Input file (-i/--input) is required.\n";
        return 1;
    }

    std::string err_msg;
    mkvsplit::MKVMetadata meta = mkvsplit::MKVSplitter::InspectFile(input_file, &err_msg);
    if (meta.tracks.empty()) {
        std::cerr << "Error inspecting file: " << (err_msg.empty() ? "Unable to read MKV metadata" : err_msg) << "\n";
        return 1;
    }

    std::cout << "========================================================\n";
    std::cout << " File:     " << meta.file_path.filename().string() << "\n";
    std::cout << " Size:     " << mkvsplit::FormatBytes(meta.file_size_bytes) << "\n";
    std::cout << " Duration: " << mkvsplit::FormatDuration(meta.duration_sec) << " (" << meta.duration_sec << " s)\n";
    std::cout << " Tracks:   " << meta.tracks.size() << " total\n";
    for (const auto& tr : meta.tracks) {
        std::cout << "   - Stream #" << tr.index << " [" << tr.media_type << "] Codec: " << tr.codec_name;
        if (tr.media_type == "video") std::cout << " (" << tr.width << "x" << tr.height << ")";
        if (tr.media_type == "audio") std::cout << " (" << tr.sample_rate << " Hz, " << tr.channels << " ch)";
        std::cout << " lang: " << tr.language;
        if (!tr.title.empty()) std::cout << " title: \"" << tr.title << "\"";
        std::cout << "\n";
    }
    std::cout << " Chapters: " << meta.chapters.size() << "\n";
    for (const auto& ch : meta.chapters) {
        std::cout << "   - Chapter " << (ch.id + 1) << ": " << mkvsplit::SecondsToTimeString(ch.start_time_sec)
                  << " -> " << mkvsplit::SecondsToTimeString(ch.end_time_sec) << " (\"" << ch.title << "\")\n";
    }
    std::cout << " Keyframes: " << meta.keyframes_sec.size() << " detected\n";
    
    std::cout << "Analyzing chapters and checking nearby black transitions...\n" << std::flush;
    mkvsplit::AutoGuessResult guess = mkvsplit::MKVSplitter::GuessSplitConfig(meta, true);
    std::cout << " Auto-Guess Strategy (" << guess.confidence_percent << "% Confidence):\n";
    std::cout << "   " << guess.reasoning << "\n";
    std::cout << "========================================================\n";

    if (info_only) {
        return 0;
    }

    mkvsplit::SplitConfig config;
    config.input_file = input_file;
    config.output_directory = output_dir;
    config.season_number = season_num;
    config.start_episode_number = start_ep_num;

    if (mode_str == "duration") {
        config.mode = mkvsplit::SplitMode::FixedDuration;
        config.chunk_duration_sec = chunk_duration;
    } else if (mode_str == "size") {
        config.mode = mkvsplit::SplitMode::FileSize;
        config.chunk_max_bytes = chunk_size_mb * 1024 * 1024;
    } else if (mode_str == "auto") {
        if (guess.recommended_config.mode == mkvsplit::SplitMode::Chapters) {
            config = guess.recommended_config;
            config.input_file = input_file;
            config.output_directory = output_dir;
            if (season_num > 0) config.season_number = season_num;
            if (start_ep_num > 0) config.start_episode_number = start_ep_num;
        } else {
            config.mode = mkvsplit::SplitMode::AutoGap;
            config.gap_config.type = mkvsplit::GapType::BlackFrame;
            config.gap_config.expected_episodes = expected_eps > 0 ? expected_eps :
                guess.recommended_config.gap_config.expected_episodes;
            config.gap_config.min_gap_duration_sec = min_gap;
            config.gap_config.min_segment_duration_sec = min_segment;
        }
    } else if (mode_str == "gap") {
        config.mode = mkvsplit::SplitMode::AutoGap;
        if (gap_type_str == "silence" || gap_type_str == "audio") {
            config.gap_config.type = mkvsplit::GapType::AudioSilence;
        } else if (gap_type_str == "combined") {
            config.gap_config.type = mkvsplit::GapType::Combined;
        } else {
            config.gap_config.type = mkvsplit::GapType::BlackFrame;
        }
        config.gap_config.expected_episodes = expected_eps;
        config.gap_config.min_gap_duration_sec = min_gap;
        config.gap_config.min_segment_duration_sec = min_segment;
        config.gap_config.noise_threshold_db = noise_db;
    } else if (mode_str == "chapters") {
        config.mode = mkvsplit::SplitMode::Chapters;
        config.chapter_group_count = (group_chapters > 0) ? group_chapters : guess.recommended_config.chapter_group_count;
        if (group_chapters <= 0 && chapters_str.empty()) config.chapter_group_sizes = guess.recommended_config.chapter_group_sizes;
        config.lead_bumper_chapters = (lead_bumpers >= 0) ? lead_bumpers : guess.recommended_config.lead_bumper_chapters;
        config.tail_bumper_chapters = (tail_bumpers >= 0) ? tail_bumpers : guess.recommended_config.tail_bumper_chapters;
        if (!chapters_str.empty()) {
            std::stringstream ss(chapters_str);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try {
                    config.selected_chapter_ids.push_back(std::stoi(token) - 1);
                } catch (...) {}
            }
        }
    } else {
        config.mode = mkvsplit::SplitMode::TimeRange;
        config.range_start_sec = mkvsplit::TimeStringToSeconds(start_str);
        config.range_end_sec = mkvsplit::TimeStringToSeconds(end_str);
    }

    if (bumpers_str == "separate" || bumpers_str == "dump") {
        config.bumper_action = mkvsplit::BumperAction::Separate;
    } else if (bumpers_str == "discard" || bumpers_str == "omit" || bumpers_str == "skip") {
        config.bumper_action = mkvsplit::BumperAction::Discard;
    } else {
        config.bumper_action = mkvsplit::BumperAction::Merge;
    }

    if (lead_bumpers >= 0) config.lead_bumper_chapters = lead_bumpers;
    if (tail_bumpers >= 0) config.tail_bumper_chapters = tail_bumpers;

    std::vector<mkvsplit::SplitSegment> plan = mkvsplit::MKVSplitter::PlanSegments(meta, config);
    std::cout << "\nSplit Execution Plan (" << plan.size() << " segment" << (plan.size() == 1 ? "" : "s") << "):\n";
    PrintEpisodeCountMismatch(config, plan);
    for (size_t i = 0; i < plan.size(); i++) {
        std::cout << " [" << (i + 1) << "/" << plan.size() << "] Tag: " << plan[i].name_tag
                  << " | Range: " << mkvsplit::SecondsToTimeString(plan[i].start_time_sec)
                  << " -> " << mkvsplit::SecondsToTimeString(plan[i].end_time_sec) << "\n";
    }
    std::cout << "\nSplitting lossless streams to " << output_dir << "...\n";

    auto progress_cb = [](const mkvsplit::ProgressInfo& info) {
        std::string truncated_fn = mkvsplit::TruncateString(info.current_output_filename, 22);

        std::ostringstream ss;
        ss << "[" << std::setw(3) << static_cast<int>(info.overall_progress) << "%] "
           << "Segment " << info.current_segment_index << "/" << info.total_segments
           << " (" << truncated_fn << ") - "
           << mkvsplit::FormatBytes(info.bytes_written) << " written";

        std::string line = ss.str();
        if (line.length() > 75) line = line.substr(0, 75);

        std::cout << "\r" << line << "\033[K" << std::flush;
    };

    bool success = mkvsplit::MKVSplitter::Split(config, progress_cb, &err_msg);
    std::cout << "\n";

    if (success) {
        std::cout << "SUCCESS: All MKV segments created cleanly.\n";
        return 0;
    } else {
        std::cerr << "ERROR during split: " << err_msg << "\n";
        return 1;
    }
}
