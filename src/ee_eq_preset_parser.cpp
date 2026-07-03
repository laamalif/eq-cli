#include "ee_eq_preset_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>
#include <regex>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lsp_labels.hpp"

namespace ee {

using namespace ee::labels;

namespace {

constexpr size_t kMaxBands = 32;

auto default_band_frequencies(const size_t num_bands) -> std::array<double, kMaxBands> {
  std::array<double, kMaxBands> values{};
  constexpr double min_freq = 20.0;
  constexpr double max_freq = 20000.0;

  double freq0 = min_freq;
  const double step = std::pow(max_freq / min_freq, 1.0 / static_cast<double>(num_bands));
  for (size_t i = 0; i < num_bands; ++i) {
    const double freq1 = freq0 * step;
    values[i] = freq0 + (0.5 * (freq1 - freq0));
    freq0 = freq1;
  }

  for (size_t i = num_bands; i < kMaxBands; ++i) {
    values[i] = values[num_bands > 0 ? num_bands - 1 : 0];
  }

  return values;
}

auto default_band_qs(const size_t num_bands) -> std::array<double, kMaxBands> {
  std::array<double, kMaxBands> values{};
  constexpr double min_freq = 20.0;
  constexpr double max_freq = 20000.0;

  double freq0 = min_freq;
  const double step = std::pow(max_freq / min_freq, 1.0 / static_cast<double>(num_bands));
  for (size_t i = 0; i < num_bands; ++i) {
    const double freq1 = freq0 * step;
    const double freq = freq0 + (0.5 * (freq1 - freq0));
    const double width = freq1 - freq0;
    values[i] = freq / width;
    freq0 = freq1;
  }

  for (size_t i = num_bands; i < kMaxBands; ++i) {
    values[i] = values[num_bands > 0 ? num_bands - 1 : 0];
  }

  return values;
}

template <typename Container>
auto validate_label(const std::string& value,
                    const Container& labels,
                    std::string_view field,
                    std::string& error)
    -> std::string {
  if (std::ranges::find(labels, value) == labels.end()) {
    error = std::format("unsupported value '{}' for {}", value, field);
    return {};
  }
  return value;
}

auto band_default() -> EqBand {
  return EqBand{
      .type = "Bell",
      .mode = "RLC (BT)",
      .slope = "x1",
      .solo = false,
      .mute = false,
      .gain_db = 0.0,
      .frequency = 0.0,
      .q = 4.36,
      .width = 4.0,
  };
}

void initialize_equalizer_defaults(EqPreset& preset) {
  const auto num_bands = static_cast<size_t>(std::clamp(preset.num_bands, 1, static_cast<int>(kMaxBands)));
  const auto default_freq = default_band_frequencies(num_bands);
  const auto default_q = default_band_qs(num_bands);

  for (size_t i = 0; i < kMaxBands; ++i) {
    auto defaults = band_default();
    defaults.frequency = default_freq[i];
    defaults.q = default_q[i];
    if (i >= num_bands) {
      defaults.type = "Off";
    }
    preset.left[i] = defaults;
    preset.right[i] = defaults;
  }
}

auto parse_band(const nlohmann::json& json,
                const std::string& key,
                const EqBand& defaults,
                std::string& error) -> EqBand {
  EqBand band = defaults;

  if (!json.contains(key)) {
    return band;
  }

  const auto& value = json.at(key);
  band.type = validate_label(value.value("type", band.type), kBandTypeLabels, key + ".type", error);
  if (!error.empty()) {
    return {};
  }

  band.mode = validate_label(value.value("mode", band.mode), kBandModeLabels, key + ".mode", error);
  if (!error.empty()) {
    return {};
  }

  band.slope = validate_label(value.value("slope", band.slope), kBandSlopeLabels, key + ".slope", error);
  if (!error.empty()) {
    return {};
  }

  band.solo = value.value("solo", band.solo);
  band.mute = value.value("mute", band.mute);
  band.gain_db = value.value("gain", band.gain_db);
  band.frequency = value.value("frequency", band.frequency);
  band.q = value.value("q", band.q);
  band.width = value.value("width", band.width);

  return band;
}

auto find_eq_instance_key(const nlohmann::json& output, std::string& error) -> std::string {
  if (output.contains("equalizer")) {
    return "equalizer";
  }

  for (auto it = output.begin(); it != output.end(); ++it) {
    if (it.key().rfind("equalizer#", 0) == 0) {
      return it.key();
    }
  }

  error = "missing output.equalizer payload";
  return {};
}

auto find_instance_key(const nlohmann::json& output, const std::string& base_name) -> std::optional<std::string> {
  if (output.contains(base_name)) {
    return base_name;
  }

  for (auto it = output.begin(); it != output.end(); ++it) {
    if (it.key().rfind(base_name + "#", 0) == 0) {
      return it.key();
    }
  }

  return std::nullopt;
}

auto push_plugin_kind(std::vector<std::string>& plugin_order,
                      std::string_view plugin_kind,
                      std::string& error) -> bool {
  const auto duplicate = std::ranges::find(plugin_order, plugin_kind) != plugin_order.end();
  if (duplicate) {
    error = std::format("duplicate plugin kind is not supported: {}", plugin_kind);
    return false;
  }

  plugin_order.emplace_back(plugin_kind);
  return true;
}

auto parse_limiter(const nlohmann::json& output, const std::string& key, std::string& error) -> LimiterPreset {
  LimiterPreset preset;
  const auto& json = output.at(key);

  preset.mode = validate_label(json.value("mode", preset.mode), kLimiterModeLabels, "limiter.mode", error);
  if (!error.empty()) return {};
  preset.oversampling =
      validate_label(json.value("oversampling", preset.oversampling), kLimiterOversamplingLabels, "limiter.oversampling", error);
  if (!error.empty()) return {};
  preset.dithering =
      validate_label(json.value("dithering", preset.dithering), kLimiterDitheringLabels, "limiter.dithering", error);
  if (!error.empty()) return {};
  preset.sidechain_type =
      validate_label(json.value("sidechain-type", preset.sidechain_type), kLimiterSidechainLabels, "limiter.sidechain-type", error);
  if (!error.empty()) return {};

  preset.bypass = json.value("bypass", preset.bypass);
  preset.input_gain_db = json.value("input-gain", preset.input_gain_db);
  preset.output_gain_db = json.value("output-gain", preset.output_gain_db);
  preset.lookahead = json.value("lookahead", preset.lookahead);
  preset.attack = json.value("attack", preset.attack);
  preset.release = json.value("release", preset.release);
  preset.threshold_db = json.value("threshold", preset.threshold_db);
  preset.sidechain_preamp_db = json.value("sidechain-preamp", preset.sidechain_preamp_db);
  preset.stereo_link = json.value("stereo-link", preset.stereo_link);
  preset.alr = json.value("alr", preset.alr);
  preset.alr_attack = json.value("alr-attack", preset.alr_attack);
  preset.alr_release = json.value("alr-release", preset.alr_release);
  preset.alr_knee_db = json.value("alr-knee", preset.alr_knee_db);
  preset.alr_knee_smooth_db = json.value("alr-knee-smooth", preset.alr_knee_smooth_db);
  preset.gain_boost = json.value("gain-boost", preset.gain_boost);
  preset.input_to_sidechain_db = json.value("input-to-sidechain", preset.input_to_sidechain_db);
  preset.input_to_link_db = json.value("input-to-link", preset.input_to_link_db);
  preset.sidechain_to_input_db = json.value("sidechain-to-input", preset.sidechain_to_input_db);
  preset.sidechain_to_link_db = json.value("sidechain-to-link", preset.sidechain_to_link_db);
  preset.link_to_input_db = json.value("link-to-input", preset.link_to_input_db);
  preset.link_to_sidechain_db = json.value("link-to-sidechain", preset.link_to_sidechain_db);
  return preset;
}

auto parse_convolver(const nlohmann::json& output, const std::string& key) -> ConvolverPreset {
  ConvolverPreset preset;
  const auto& json = output.at(key);
  preset.bypass = json.value("bypass", preset.bypass);
  preset.input_gain_db = json.value("input-gain", preset.input_gain_db);
  preset.output_gain_db = json.value("output-gain", preset.output_gain_db);
  preset.kernel_name = json.value("kernel-name", preset.kernel_name);
  preset.kernel_path = json.value("kernel-path", preset.kernel_path);
  preset.ir_width = json.value("ir-width", preset.ir_width);
  preset.autogain = json.value("autogain", preset.autogain);
  preset.dry_db = json.value("dry", preset.dry_db);
  preset.wet_db = json.value("wet", preset.wet_db);
  if (json.contains("sofa") && json.at("sofa").is_object()) {
    const auto& sofa = json.at("sofa");
    preset.sofa_azimuth = sofa.value("azimuth", preset.sofa_azimuth);
    preset.sofa_elevation = sofa.value("elevation", preset.sofa_elevation);
    preset.sofa_radius = sofa.value("radius", preset.sofa_radius);
  }
  return preset;
}

auto trim_copy(std::string_view value) -> std::string {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(first, last - first + 1));
}

auto uppercase_copy(std::string_view value) -> std::string {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return result;
}

auto parse_double_match(const std::string& line, const std::regex& re, double& out) -> bool {
  std::smatch matches;
  if (!std::regex_search(line, matches, re) || matches.size() != 2U) {
    return false;
  }
  try {
    out = std::stod(matches.str(1));
    return true;
  } catch (...) {
    return false;
  }
}

auto parse_autoeq_filter_line(const std::string& line,
                              EqBand& band,
                              bool& enabled,
                              std::string& error) -> bool {
  static const std::regex kFilterStateRe(
      R"(^\s*Filter\s+\d+\s*:\s*(ON|OFF)(?:\s+([A-Za-z]+(?:\s+(?:6|12)dB)?))?)",
      std::regex::icase);
  static const std::regex kFrequencyRe(R"(Fc\s+([+-]?\d+(?:\.\d+)?)\s*Hz)", std::regex::icase);
  static const std::regex kGainRe(R"(Gain\s+([+-]?\d+(?:\.\d+)?)\s*dB)", std::regex::icase);
  static const std::regex kQRe(R"(Q\s+([+-]?\d+(?:\.\d+)?))", std::regex::icase);

  std::smatch matches;
  if (!std::regex_search(line, matches, kFilterStateRe) || matches.size() < 2U) {
    return false;
  }

  enabled = uppercase_copy(matches.str(1)) == "ON";
  if (!enabled) {
    return true;
  }

  const auto filter_code = uppercase_copy(trim_copy(matches.size() >= 3U ? matches.str(2) : std::string{}));
  band = band_default();
  band.mode = "APO (DR)";

  const auto parse_freq_required = [&]() -> bool {
    if (!parse_double_match(line, kFrequencyRe, band.frequency)) {
      error = std::format("missing or invalid Fc value in line: {}", line);
      return false;
    }
    return true;
  };

  const auto parse_gain_required = [&]() -> bool {
    if (!parse_double_match(line, kGainRe, band.gain_db)) {
      error = std::format("missing or invalid Gain value in line: {}", line);
      return false;
    }
    return true;
  };

  const auto parse_q_required = [&]() -> bool {
    if (!parse_double_match(line, kQRe, band.q)) {
      error = std::format("missing or invalid Q value in line: {}", line);
      return false;
    }
    return true;
  };

  if (!parse_freq_required()) {
    return true;
  }

  if (filter_code == "PK" || filter_code == "PEQ" || filter_code == "MODAL") {
    band.type = "Bell";
    return parse_gain_required() && parse_q_required();
  }
  if (filter_code == "LSC") {
    band.type = "Lo-shelf";
    return parse_gain_required() && parse_q_required();
  }
  if (filter_code == "HSC") {
    band.type = "Hi-shelf";
    return parse_gain_required() && parse_q_required();
  }
  if (filter_code == "LS") {
    band.type = "Lo-shelf";
    band.q = 2.0 / 3.0;
    return parse_gain_required();
  }
  if (filter_code == "HS") {
    band.type = "Hi-shelf";
    band.q = 2.0 / 3.0;
    return parse_gain_required();
  }
  if (filter_code == "LP" || filter_code == "LPQ") {
    band.type = "Lo-pass";
    if (filter_code == "LPQ") {
      return parse_q_required();
    }
    return true;
  }
  if (filter_code == "HP" || filter_code == "HPQ") {
    band.type = "Hi-pass";
    if (filter_code == "HPQ") {
      return parse_q_required();
    }
    return true;
  }
  if (filter_code == "NO") {
    band.type = "Notch";
    return parse_q_required();
  }
  if (filter_code == "BP") {
    band.type = "Bandpass";
    return parse_q_required();
  }
  if (filter_code == "AP") {
    band.type = "Allpass";
    band.q = 0.0;
    return true;
  }

  error = std::format("unsupported AutoEQ filter type '{}'", filter_code);
  return true;
}

auto parse_easy_effects_preset_json(const nlohmann::json& json, std::string& error) -> ParsedPreset {
  ParsedPreset parsed;
  EqPreset& preset = parsed.equalizer;

  if (!json.contains("output") || !json.at("output").is_object()) {
    error = "missing output section";
    return {};
  }

  const auto& output = json.at("output");
  if (!output.contains("plugins_order") || !output.at("plugins_order").is_array()) {
    error = "missing output.plugins_order";
    return {};
  }

  const auto plugins = output.at("plugins_order").get<std::vector<std::string>>();
  if (plugins.empty()) {
    error = "empty output.plugins_order is not supported";
    return {};
  }

  bool has_equalizer = false;
  for (const auto& plugin : plugins) {
    if (plugin.rfind("equalizer", 0) == 0) {
      has_equalizer = true;
      if (!push_plugin_kind(parsed.plugin_order, "equalizer", error)) {
        return {};
      }
    } else if (plugin.rfind("limiter", 0) == 0) {
      if (!push_plugin_kind(parsed.plugin_order, "limiter", error)) {
        return {};
      }
    } else if (plugin.rfind("convolver", 0) == 0) {
      if (!push_plugin_kind(parsed.plugin_order, "convolver", error)) {
        return {};
      }
    } else {
      parsed.warnings.push_back(std::format("skipping unsupported plugin: {}", plugin));
    }
  }
  if (!has_equalizer) {
    error = "equalizer is required in output.plugins_order";
    return {};
  }

  const auto instance_key = find_eq_instance_key(output, error);
  if (!error.empty()) {
    return {};
  }

  const auto& eq = output.at(instance_key);

  preset.bypass = eq.value("bypass", false);
  preset.input_gain_db = eq.value("input-gain", 0.0);
  preset.output_gain_db = eq.value("output-gain", 0.0);
  preset.mode = validate_label(eq.value("mode", preset.mode), kEqModeLabels, "mode", error);
  if (!error.empty()) {
    return {};
  }

  preset.num_bands = std::clamp(eq.value("num-bands", preset.num_bands), 1, static_cast<int>(kMaxBands));
  preset.split_channels = eq.value("split-channels", false);
  preset.balance = eq.value("balance", 0.0);
  preset.pitch_left = eq.value("pitch-left", 0.0);
  preset.pitch_right = eq.value("pitch-right", 0.0);

  initialize_equalizer_defaults(preset);

  if (!eq.contains("left") || !eq.at("left").is_object()) {
    error = "missing output.equalizer.left";
    return {};
  }

  if (!eq.contains("right") || !eq.at("right").is_object()) {
    error = "missing output.equalizer.right";
    return {};
  }

  for (size_t i = 0; i < kMaxBands; ++i) {
    const auto band_key = std::format("band{}", i);
    preset.left[i] = parse_band(eq.at("left"), band_key, preset.left[i], error);
    if (!error.empty()) {
      return {};
    }

    preset.right[i] = parse_band(eq.at("right"), band_key, preset.right[i], error);
    if (!error.empty()) {
      return {};
    }
  }

  if (!preset.split_channels) {
    preset.right = preset.left;
  }

  if (std::ranges::find(parsed.plugin_order, std::string("limiter")) != parsed.plugin_order.end()) {
    const auto limiter_key = find_instance_key(output, "limiter");
    if (!limiter_key.has_value()) {
      error = "missing output.limiter payload";
      return {};
    }

    parsed.limiter = parse_limiter(output, *limiter_key, error);
    if (!error.empty()) {
      return {};
    }
  }

  if (std::ranges::find(parsed.plugin_order, std::string("convolver")) != parsed.plugin_order.end()) {
    if (const auto convolver_key = find_instance_key(output, "convolver"); convolver_key.has_value()) {
      parsed.convolver = parse_convolver(output, *convolver_key);
    } else {
      parsed.warnings.push_back("convolver requested in plugins_order but no convolver payload was found");
      std::erase(parsed.plugin_order, std::string("convolver"));
    }
  }

  if (parsed.plugin_order.empty()) {
    error = "no supported plugin remained applicable after parsing";
    return {};
  }

  return parsed;
}

}  // namespace

auto parse_easy_effects_preset(std::string_view bytes, std::string& error) -> ParsedPreset {
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(bytes.begin(), bytes.end());
  } catch (const std::exception& e) {
    error = std::format("invalid preset JSON: {}", e.what());
    return {};
  }

  try {
    return parse_easy_effects_preset_json(json, error);
  } catch (const nlohmann::json::exception& e) {
    // value()/get() throw on fields present with the wrong JSON type.
    error = std::format("invalid preset structure: {}", e.what());
    return {};
  }
}

auto parse_autoeq_preset(std::string_view text, std::string& error) -> ParsedPreset {
  ParsedPreset parsed;
  parsed.plugin_order.emplace_back("equalizer");
  parsed.equalizer.mode = "IIR";
  parsed.equalizer.split_channels = false;

  std::vector<EqBand> imported_bands;
  std::istringstream stream{std::string(text)};
  std::string line;
  bool saw_preamp = false;

  static const std::regex kPreampRe(R"(^\s*Preamp\s*:\s*([+-]?\d+(?:\.\d+)?)\s*dB\s*$)", std::regex::icase);

  while (std::getline(stream, line)) {
    const auto trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed.starts_with('#')) {
      continue;
    }

    double preamp = 0.0;
    if (parse_double_match(trimmed, kPreampRe, preamp)) {
      if (saw_preamp || !imported_bands.empty()) {
        error = "multiple AutoEQ preset blocks detected; split them into separate files";
        return {};
      }
      parsed.equalizer.input_gain_db = preamp;
      saw_preamp = true;
      continue;
    }

    EqBand band;
    bool enabled = false;
    if (parse_autoeq_filter_line(trimmed, band, enabled, error)) {
      if (!error.empty()) {
        return {};
      }
      if (enabled) {
        imported_bands.push_back(std::move(band));
      }
      continue;
    }

    error = std::format("unexpected AutoEQ text line: {}", trimmed);
    return {};
  }

  if (imported_bands.empty()) {
    error = "no supported AutoEQ filter lines were found";
    return {};
  }

  if (!saw_preamp) {
    parsed.warnings.push_back("AutoEQ text omitted Preamp; defaulting input gain to 0 dB");
  }

  parsed.equalizer.num_bands = std::clamp(static_cast<int>(imported_bands.size()), 1, static_cast<int>(kMaxBands));
  initialize_equalizer_defaults(parsed.equalizer);

  for (size_t i = 0; i < static_cast<size_t>(parsed.equalizer.num_bands); ++i) {
    parsed.equalizer.left[i] = imported_bands[i];
    parsed.equalizer.right[i] = imported_bands[i];
  }

  return parsed;
}

auto render_easy_effects_preset_json(const ParsedPreset& preset) -> std::string {
  nlohmann::json output;
  output["plugins_order"] = preset.plugin_order;

  nlohmann::json equalizer;
  equalizer["bypass"] = preset.equalizer.bypass;
  equalizer["input-gain"] = preset.equalizer.input_gain_db;
  equalizer["output-gain"] = preset.equalizer.output_gain_db;
  equalizer["mode"] = preset.equalizer.mode;
  equalizer["split-channels"] = preset.equalizer.split_channels;
  equalizer["balance"] = preset.equalizer.balance;
  equalizer["pitch-left"] = preset.equalizer.pitch_left;
  equalizer["pitch-right"] = preset.equalizer.pitch_right;
  equalizer["num-bands"] = preset.equalizer.num_bands;

  nlohmann::json left = nlohmann::json::object();
  nlohmann::json right = nlohmann::json::object();
  for (int i = 0; i < preset.equalizer.num_bands; ++i) {
    const auto band_key = std::format("band{}", i);
    const auto& left_band = preset.equalizer.left[static_cast<size_t>(i)];
    const auto& right_band = preset.equalizer.right[static_cast<size_t>(i)];

    left[band_key] = {
        {"type", left_band.type},
        {"mode", left_band.mode},
        {"slope", left_band.slope},
        {"solo", left_band.solo},
        {"mute", left_band.mute},
        {"gain", left_band.gain_db},
        {"frequency", left_band.frequency},
        {"q", left_band.q},
        {"width", left_band.width},
    };
    right[band_key] = {
        {"type", right_band.type},
        {"mode", right_band.mode},
        {"slope", right_band.slope},
        {"solo", right_band.solo},
        {"mute", right_band.mute},
        {"gain", right_band.gain_db},
        {"frequency", right_band.frequency},
        {"q", right_band.q},
        {"width", right_band.width},
    };
  }

  equalizer["left"] = std::move(left);
  equalizer["right"] = std::move(right);
  output["equalizer"] = std::move(equalizer);

  nlohmann::json root;
  root["output"] = std::move(output);
  return root.dump(2);
}

}  // namespace ee
