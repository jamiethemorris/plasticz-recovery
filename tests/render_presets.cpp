#include "SynthEngine.h"
#include "Presets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct Config {
    fs::path output;
    fs::path reference;
    fs::path difference;
    int sampleRate = 44100, note = 24, velocity = 100, block = 512, preroll = 512;
    int first = 0, count = 128;
    double held = 2.0, tail = 4.0;
    bool full = false;
    int channels() const { return full ? 2 : 1; }
};

std::uint32_t bits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float fromBits(std::uint32_t word) {
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

void write16(std::ostream& stream, std::uint16_t value) {
    stream.put(static_cast<char>(value));
    stream.put(static_cast<char>(value >> 8));
}

void write32(std::ostream& stream, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) stream.put(static_cast<char>(value >> (byte * 8)));
}

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

std::uint32_t read32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

void writeWave(const fs::path& path, const std::vector<float>& samples, int sampleRate, int channels) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Cannot write " + path.string());
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * 4);
    stream.write("RIFF", 4); write32(stream, 48 + dataSize); stream.write("WAVEfmt ", 8);
    write32(stream, 16); write16(stream, 3); write16(stream, static_cast<std::uint16_t>(channels));
    write32(stream, static_cast<std::uint32_t>(sampleRate));
    write32(stream, static_cast<std::uint32_t>(sampleRate * channels * 4));
    write16(stream, static_cast<std::uint16_t>(channels * 4)); write16(stream, 32);
    stream.write("fact", 4); write32(stream, 4); write32(stream, static_cast<std::uint32_t>(samples.size() / channels));
    stream.write("data", 4); write32(stream, dataSize);
    for (const float value : samples) write32(stream, bits(value));
    require(static_cast<bool>(stream), "Failed writing " + path.string());
}

std::vector<float> readWave(const fs::path& path, int sampleRate, std::size_t sampleCount, int channels) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Cannot read reference " + path.string());
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    require(length >= 44 && length < 512 * 1024 * 1024, "Invalid reference WAV length");
    stream.seekg(0);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    require(static_cast<bool>(stream), "Truncated reference WAV");
    require(std::memcmp(data.data(), "RIFF", 4) == 0 && std::memcmp(data.data() + 8, "WAVE", 4) == 0,
            "Expected RIFF WAVE reference");
    require(read32(data.data() + 4) + std::uint64_t{8} == data.size(), "Reference RIFF size mismatch");
    bool formatSeen = false, payloadSeen = false;
    std::vector<float> result;
    for (std::size_t offset = 12; offset + 8 <= data.size();) {
        const auto size = static_cast<std::size_t>(read32(data.data() + offset + 4));
        require(size <= data.size() - offset - 8, "Invalid WAV chunk size");
        const auto* content = data.data() + offset + 8;
        if (std::memcmp(data.data() + offset, "fmt ", 4) == 0) {
            require(!formatSeen && size >= 16, "Invalid duplicate or truncated WAV fmt chunk");
            require(read16(content) == 3 && read16(content + 2) == channels && read16(content + 14) == 32,
                    "Reference must have the selected channel count and IEEE float32 format");
            require(read32(content + 4) == static_cast<std::uint32_t>(sampleRate), "Reference sample rate differs");
            require(read16(content + 12) == channels * 4
                    && read32(content + 8) == static_cast<std::uint32_t>(sampleRate * channels * 4),
                    "Invalid reference sample alignment/byte rate");
            formatSeen = true;
        } else if (std::memcmp(data.data() + offset, "data", 4) == 0) {
            require(!payloadSeen && size == sampleCount * 4, "Reference sample count differs");
            result.resize(sampleCount);
            for (std::size_t i = 0; i < sampleCount; ++i) result[i] = fromBits(read32(content + i * 4));
            payloadSeen = true;
        }
        offset += 8 + size + (size & 1u);
    }
    require(formatSeen && payloadSeen, "Reference WAV missing format or audio data");
    return result;
}

std::string filename(int index) {
    std::ostringstream result;
    result << "preset-" << std::setw(3) << std::setfill('0') << index << ".wav";
    return result.str();
}

std::string jsonString(const std::string& value) {
    std::ostringstream stream;
    stream << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') stream << '\\' << c;
        else if (c < 32 || c >= 127) stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
        else stream << c;
    }
    stream << '"';
    return stream.str();
}

std::string csvString(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}

std::string programName(const plasticz::Program& program) {
    return {program.name.begin(), std::find(program.name.begin(), program.name.end(), '\0')};
}

std::vector<float> render(const Config& config, plasticz::Parameters parameters) {
    const auto heldSamples = static_cast<int>(std::llround(config.held * config.sampleRate));
    const auto totalSamples = heldSamples + static_cast<int>(std::llround(config.tail * config.sampleRate));
    if (!config.full) parameters[37] = 0.0f; // Explicit dry suite contract.
    plasticz::SynthEngine engine;
    engine.prepare(config.sampleRate, config.block);
    engine.setFullProcessingEnabled(config.full);
    engine.setParameters(parameters);
    std::vector<float> output(static_cast<std::size_t>(totalSamples) * config.channels());
    std::vector<float> left(static_cast<std::size_t>(config.block));
    std::vector<float> right(static_cast<std::size_t>(std::max(config.block, config.preroll)));
    std::vector<float> discarded(static_cast<std::size_t>(config.preroll));
    // Settle original mono-mode initialization and retain its fractional clock.
    engine.render(discarded.data(), right.data(), config.preroll);
    for (int frame = 0; frame < config.preroll; ++frame)
        require(std::isfinite(discarded[static_cast<std::size_t>(frame)])
                && std::isfinite(right[static_cast<std::size_t>(frame)]), "Non-finite preroll audio");
    engine.noteOn(config.note, static_cast<float>(config.velocity) / 127.0f);
    bool released = false;
    for (int hostStart = 0; hostStart < totalSamples; hostStart += config.block) {
        const int hostEnd = std::min(totalSamples, hostStart + config.block);
        int position = hostStart;
        while (position < hostEnd) {
            if (!released && position == heldSamples) { engine.noteOff(config.note); released = true; }
            const int end = !released ? std::min(hostEnd, heldSamples) : hostEnd;
            const int count = end - position;
            require(count > 0, "Invalid MIDI split interval");
            engine.render(left.data(), right.data(), count);
            for (int i = 0; i < count; ++i) {
                const auto value = left[static_cast<std::size_t>(i)];
                const auto rightValue = right[static_cast<std::size_t>(i)];
                require(std::isfinite(value) && std::isfinite(rightValue), "Native engine produced non-finite audio");
                if (config.full) {
                    output[static_cast<std::size_t>(position + i) * 2] = value;
                    output[static_cast<std::size_t>(position + i) * 2 + 1] = rightValue;
                } else {
                    require(bits(value) == bits(rightValue), "Dry stereo channels diverged");
                    output[static_cast<std::size_t>(position + i)] = value;
                }
            }
            position = end;
        }
    }
    return output;
}

struct Comparison {
    std::uint64_t bitMismatches = 0, sampleCount = 0;
    std::int64_t firstMismatch = -1;
    double maxError = 0, squareError = 0, peakReference = 0, peakNative = 0;
    double rms() const { return sampleCount ? std::sqrt(squareError / static_cast<double>(sampleCount)) : 0; }
};

Comparison compare(const std::vector<float>& actual, const std::vector<float>& reference,
                   std::vector<float>& difference) {
    require(actual.size() == reference.size(), "Sample count differs");
    Comparison result;
    result.sampleCount = actual.size();
    difference.resize(actual.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(std::isfinite(reference[i]), "Reference contains non-finite audio");
        if (bits(actual[i]) != bits(reference[i])) {
            ++result.bitMismatches;
            if (result.firstMismatch < 0) result.firstMismatch = static_cast<std::int64_t>(i);
        }
        const double error = static_cast<double>(actual[i]) - static_cast<double>(reference[i]);
        difference[i] = static_cast<float>(error);
        result.maxError = std::max(result.maxError, std::abs(error));
        result.squareError += error * error;
        result.peakReference = std::max(result.peakReference, std::abs(static_cast<double>(reference[i])));
        result.peakNative = std::max(result.peakNative, std::abs(static_cast<double>(actual[i])));
    }
    return result;
}

void usage() {
    std::cout << "render_presets [--full] [--output DIR] [--reference DIR] [--difference DIR] [--first N] [--count N]\n"
                 "  [--sample-rate N] [--note N] [--velocity N] [--block N] [--preroll N] [--held SECONDS] [--tail SECONDS]\n"
                 "Defaults: all128 factory programs, MIDI24, velocity100/127,44100Hz,512-frame silent preroll,\n"
                 "512-frame host blocks,2s held+4s release. Default is dry mono with original4x off.\n"
                 "--full enables native effects/output and stereo, preserving all factory fields. No normalization.\n"
                 "Comparison exits1 for ANY sample-bit mismatch; no tolerance is accepted.\n";
}

Config parse(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") { usage(); std::exit(0); }
        if (key == "--full") { config.full = true; continue; }
        require(i + 1 < argc, "Missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--output") config.output = value;
        else if (key == "--reference") config.reference = value;
        else if (key == "--difference") config.difference = value;
        else if (key == "--sample-rate") config.sampleRate = std::stoi(value);
        else if (key == "--note") config.note = std::stoi(value);
        else if (key == "--velocity") config.velocity = std::stoi(value);
        else if (key == "--block") config.block = std::stoi(value);
        else if (key == "--preroll") config.preroll = std::stoi(value);
        else if (key == "--first") config.first = std::stoi(value);
        else if (key == "--count") config.count = std::stoi(value);
        else if (key == "--held") config.held = std::stod(value);
        else if (key == "--tail") config.tail = std::stod(value);
        else throw std::runtime_error("Unknown argument " + key);
    }
    require(config.sampleRate >= 8000 && config.sampleRate <= 384000, "Sample rate outside8000..384000");
    require(config.note >= 0 && config.note <= 127 && config.velocity >= 1 && config.velocity <= 127, "Invalid MIDI note or velocity");
    require(config.block > 0 && config.block <= 65536 && config.preroll >= 0 && config.preroll <= 65536, "Invalid block/preroll size");
    require(config.first >= 0 && config.count > 0 && config.first + config.count <= 128, "Preset range outside0..127");
    require(std::isfinite(config.held) && std::isfinite(config.tail) && config.held > 0 && config.tail >= 0
            && config.held + config.tail <= 120, "Duration must be finite,positive and at most120seconds");
    if (config.output.empty()) config.output = config.full ? "analysis/recovered/preset-renders-full/native" : "build/preset-renders-dry/native";
    if (config.difference.empty()) config.difference = config.output.parent_path() / "difference";
    if (!config.reference.empty()) {
        require(fs::weakly_canonical(config.reference) != fs::weakly_canonical(config.output), "Native output cannot overwrite references");
        require(fs::weakly_canonical(config.reference) != fs::weakly_canonical(config.difference), "Difference output cannot overwrite references");
        require(fs::weakly_canonical(config.output) != fs::weakly_canonical(config.difference), "Difference and native directories must differ");
    }
    return config;
}
}

int main(int argc, char** argv) {
    try {
        const auto config = parse(argc, argv);
        fs::create_directories(config.output);
        const auto bank = plasticz::factoryBank();
        std::ofstream csv;
        if (!config.reference.empty()) {
            csv.open(config.output / "comparison.csv");
            require(static_cast<bool>(csv), "Cannot open comparison report");
            csv << "preset,name,samples,bit_mismatches,first_mismatch,max_abs_error,rms_error,peak_reference,peak_native,channels,first_mismatch_frame,first_mismatch_channel\n";
            csv << std::setprecision(17);
        }
        Comparison total;
        int failedPresets = 0;
        for (int preset = config.first; preset < config.first + config.count; ++preset) {
            const auto outputName = filename(preset);
            const auto samples = render(config, bank[static_cast<std::size_t>(preset)].values);
            writeWave(config.output / outputName, samples, config.sampleRate, config.channels());
            if (!config.reference.empty()) {
                const auto reference = readWave(config.reference / outputName, config.sampleRate, samples.size(), config.channels());
                std::vector<float> difference;
                const auto result = compare(samples, reference, difference);
                writeWave(config.difference / outputName, difference, config.sampleRate, config.channels());
                csv << preset << ',' << csvString(programName(bank[static_cast<std::size_t>(preset)])) << ','
                    << result.sampleCount << ',' << result.bitMismatches << ',' << result.firstMismatch << ','
                    << result.maxError << ',' << result.rms() << ',' << result.peakReference << ',' << result.peakNative << ','
                    << config.channels() << ',' << (result.firstMismatch < 0 ? -1 : result.firstMismatch / config.channels()) << ','
                    << (result.firstMismatch < 0 ? -1 : result.firstMismatch % config.channels()) << '\n';
                total.bitMismatches += result.bitMismatches;
                total.sampleCount += result.sampleCount;
                total.maxError = std::max(total.maxError, result.maxError);
                total.squareError += result.squareError;
                if (result.bitMismatches) {
                    ++failedPresets;
                    std::cout << "MISMATCH preset=" << preset << " bits=" << result.bitMismatches
                              << " first=" << result.firstMismatch << " max=" << result.maxError << " rms=" << result.rms() << '\n';
                }
            }
            if ((preset - config.first + 1) % 16 == 0 || preset + 1 == config.first + config.count)
                std::cout << "Rendered " << preset - config.first + 1 << '/' << config.count << " presets\n" << std::flush;
        }
        std::ofstream manifest(config.output / "native-manifest.json");
        require(static_cast<bool>(manifest), "Cannot open native manifest");
        manifest << std::setprecision(17)
            << "{\n  \"renderer\": " << jsonString(config.full ? "SynthEngine::render native arm64 full processing" : "SynthEngine::render native arm64 dry path") << ",\n"
            << "  \"channels\": " << config.channels() << ",\n"
            << "  \"sample_rate\": " << config.sampleRate << ",\n  \"note\": " << config.note
            << ",\n  \"velocity_midi\": " << config.velocity << ",\n  \"host_block\": " << config.block
            << ",\n  \"silent_preroll_frames\": " << config.preroll << ",\n  \"held_seconds\": " << config.held
            << ",\n  \"tail_seconds\": " << config.tail << ",\n  \"first_preset\": " << config.first
            << ",\n  \"preset_count\": " << config.count << ",\n  \"effects\": " << (config.full ? "true" : "false") << ",\n  \"oversampling\": false,\n"
            << "  \"factory_parameter_37\": " << jsonString(config.full ? "preserved (all factory programs have 4x off)" : "forced to zero") << ",\n"
            << "  \"reference_directory\": " << jsonString(config.reference.string())
            << ",\n  \"difference_directory\": " << jsonString(config.reference.empty() ? "" : config.difference.string())
            << ",\n  \"compared_samples\": " << total.sampleCount << ",\n  \"bit_mismatches\": " << total.bitMismatches
            << ",\n  \"failed_presets\": " << failedPresets << ",\n  \"max_abs_error\": " << total.maxError
            << ",\n  \"rms_error\": " << total.rms() << "\n}\n";
        if (!config.reference.empty())
            std::cout << "Compared " << total.sampleCount << " samples across " << config.count << " presets; bit_mismatches="
                      << total.bitMismatches << " failed_presets=" << failedPresets << " max_abs_error=" << total.maxError
                      << " rms_error=" << total.rms() << '\n';
        return total.bitMismatches ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Render/comparison failed: " << error.what() << '\n';
        return 2;
    }
}
