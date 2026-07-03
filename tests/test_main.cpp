#include <array>
#include <cstdlib>
#include <cstdio>
#include <format>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sndfile.hh>
#include <string>
#include <vector>
#include <string_view>

#include "app_metadata.hpp"
#include "cli_args.hpp"
#include "logging.hpp"
#include "convolver_host.hpp"
#include "ee_eq_preset_parser.hpp"
#include "kernel_resolver.hpp"
#include "lsp_labels.hpp"
#include "math_utils.hpp"
#include "preset_source.hpp"

namespace {

int g_failures = 0;

struct TempDir {
  std::filesystem::path path;

  ~TempDir() {
    if (!path.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  }

  auto is_valid() const -> bool {
    return !path.empty();
  }
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ee::log::error(std::string("FAIL: ") + std::string(message));
    ++g_failures;
  }
}

void expect_near(double actual, double expected, double tolerance, std::string_view message) {
  if (std::abs(actual - expected) > tolerance) {
    ee::log::error(std::format("FAIL: {} (expected {}, got {})", message, expected, actual));
    ++g_failures;
  }
}

auto stereo_energy(const std::vector<float>& left, const std::vector<float>& right) -> float {
  float energy = 0.0F;
  for (size_t i = 0; i < left.size(); ++i) {
    energy += left[i] * left[i] + right[i] * right[i];
  }
  return energy;
}

auto fixture_path(std::string_view file_name) -> std::string {
  return std::string(EQ_CLI_TEST_FIXTURE_DIR) + "/" + std::string(file_name);
}

auto make_temp_dir() -> TempDir {
  std::array<char, 64> pattern{};
  std::snprintf(pattern.data(), pattern.size(), "/tmp/eq-cli-tests-XXXXXX");
  if (char* created = mkdtemp(pattern.data()); created != nullptr) {
    return TempDir{.path = created};
  }
  return {};
}

auto resolver_ir_dir(const TempDir& temp_dir) -> std::filesystem::path {
  setenv("XDG_DATA_HOME", temp_dir.path.c_str(), 1);
  return temp_dir.path / "eq-cli" / "irs";
}

void test_cli_args_accept_local_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", fixture_path("Boosted.json")};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "local preset path should parse");
  expect(args.preset_source == fixture_path("Boosted.json"), "parsed preset path should be preserved");
  expect(!args.preset_from_env, "direct preset path should not be marked as env-derived");
  expect(!args.list_sinks, "--list-sinks should default to false");
}

void test_cli_args_convert_autoeq() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--convert-autoeq", fixture_path("Boosted.json")};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--convert-autoeq should parse");
  expect(args.convert_autoeq_source == fixture_path("Boosted.json"), "--convert-autoeq should preserve its input path");
  expect(args.preset_source.empty(), "--convert-autoeq should not populate preset_source");
}

void test_cli_args_convert_autoeq_output() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--convert-autoeq", "input.txt", "--output", "preset.json"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--output should be accepted with --convert-autoeq");
  expect(args.output_path == "preset.json", "--output should preserve its output path");
}

void test_cli_args_output_requires_convert_autoeq() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--output", "preset.json"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(args.output_path.empty(), "--output alone should not be accepted");
  expect(error == "--output requires --convert-autoeq", "--output alone should produce the expected error");
}

void test_cli_args_convert_autoeq_conflicts_with_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", "a.json", "--convert-autoeq", "b.txt"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(args.preset_source.empty(), "conflicting preset sources should fail parsing");
  expect(error == "--preset and --convert-autoeq cannot be used together",
         "--preset and --convert-autoeq should be mutually exclusive");
}

void test_cli_args_reject_url_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", "https://example.invalid/preset.json"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(args.preset_source.empty(), "URL preset should not be accepted");
  expect(error == "--preset must be a local file path; URL presets are no longer supported",
         "URL preset should produce the local-file-only diagnostic");
}

void test_cli_args_allow_list_sinks_without_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--list-sinks"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--list-sinks should not require --preset");
  expect(args.list_sinks, "--list-sinks should be parsed");
}

void test_cli_args_require_preset_or_env() {
  unsetenv(ee::kDefaultPresetEnv);

  std::string error;
  const std::vector<std::string> arguments = {"eq-cli"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(args.preset_source.empty(), "missing preset should not produce a preset path");
  expect(error == std::format("no preset specified; use --preset <path> or set {}", ee::kDefaultPresetEnv),
         "missing preset should produce the actionable error");
}

void test_cli_args_use_default_preset_env() {
  setenv(ee::kDefaultPresetEnv, fixture_path("Boosted.json").c_str(), 1);

  std::string error;
  const std::vector<std::string> arguments = {"eq-cli"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "default preset env should satisfy the preset requirement");
  expect(args.preset_source == fixture_path("Boosted.json"), "default preset env should populate the preset path");
  expect(args.preset_from_env, "default preset env should be marked as env-derived");

  unsetenv(ee::kDefaultPresetEnv);
}

void test_cli_args_cli_wins_over_default_preset_env() {
  setenv(ee::kDefaultPresetEnv, fixture_path("Boosted.json").c_str(), 1);

  std::string error;
  const std::vector<std::string> arguments = {
      "eq-cli",
      "--preset",
      fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"),
  };
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "cli preset should still parse with default preset env set");
  expect(args.preset_source == fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"),
         "cli preset should override the default preset env");
  expect(!args.preset_from_env, "cli preset should clear the env-derived marker");

  unsetenv(ee::kDefaultPresetEnv);
}

void test_load_preset_local_file() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("Boosted.json"), error);
  expect(error.empty(), "fixture preset should load");
  expect(!loaded.bytes.empty(), "fixture preset bytes should be present");
  expect(loaded.origin.ends_with("Boosted.json"), "loaded origin should point at the fixture");
}

void test_load_preset_missing_file() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("missing-preset.json"), error);
  expect(loaded.bytes.empty(), "missing preset should not load bytes");
  expect(error == "failed to open preset file", "missing preset should report a file-open error");
}

void test_parse_fixture_preset() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("Boosted.json"), error);
  expect(error.empty(), "fixture preset load for parsing should succeed");

  std::string parse_error;
  const auto parsed = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  expect(parse_error.empty(), "fixture preset should parse");
  expect(parsed.plugin_order.size() == 1, "fixture preset should contain a single supported plugin");
  expect(!parsed.plugin_order.empty() && parsed.plugin_order.front() == "equalizer",
         "fixture preset should preserve equalizer ordering");
  expect(parsed.equalizer.mode == "IIR", "fixture equalizer mode should parse");
}

void test_parse_fixture_with_convolver_and_limiter() {
  std::string error;
  const auto loaded =
      ee::load_preset_source(fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"), error);
  expect(error.empty(), "convolver/limiter preset fixture should load");

  std::string parse_error;
  const auto parsed = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  expect(parse_error.empty(), "convolver/limiter preset fixture should parse");
  expect(parsed.plugin_order.size() == 3, "convolver/limiter fixture should keep three supported plugins");
  expect(parsed.plugin_order == std::vector<std::string>({"equalizer", "convolver", "limiter"}),
         "convolver/limiter fixture should preserve normalized plugin ordering");
  expect(parsed.convolver.has_value(), "convolver fixture should produce a convolver payload");
  expect(parsed.limiter.has_value(), "convolver fixture should produce a limiter payload");
}

void test_parse_invalid_preset() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("invalid-preset.json"), error);
  expect(error.empty(), "invalid preset fixture should still load as a file");

  std::string parse_error;
  const auto parsed = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  expect(parse_error.starts_with("invalid preset JSON:"), "invalid preset should produce a JSON parse error");
  expect(parsed.plugin_order.empty(), "invalid preset should not produce plugin ordering");
}

void test_parse_duplicate_plugin_kind_rejected() {
  constexpr std::string_view duplicate_preset = R"({
    "output": {
      "plugins_order": ["equalizer", "equalizer#0"],
      "equalizer": {
        "bypass": false,
        "input-gain": 0.0,
        "output-gain": 0.0,
        "mode": "IIR",
        "num-bands": 2,
        "split-channels": false,
        "left": {},
        "right": {}
      },
      "equalizer#0": {
        "bypass": false,
        "input-gain": 0.0,
        "output-gain": 0.0,
        "mode": "IIR",
        "num-bands": 2,
        "split-channels": false,
        "left": {},
        "right": {}
      }
    }
  })";

  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(duplicate_preset, error);
  expect(error == "duplicate plugin kind is not supported: equalizer",
         "duplicate normalized plugin kinds should be rejected explicitly");
  expect(parsed.plugin_order.empty(), "duplicate plugin kinds should not produce a parsed plugin order");
}

void test_parse_autoeq_fixture(std::string_view fixture_name,
                               double expected_preamp,
                               double expected_first_frequency,
                               double expected_last_gain) {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path(fixture_name), error);
  expect(error.empty(), "AutoEQ fixture should load");

  std::string parse_error;
  const auto parsed = ee::parse_autoeq_preset(loaded.bytes, parse_error);
  expect(parse_error.empty(), "AutoEQ fixture should parse");
  expect(parsed.plugin_order == std::vector<std::string>({"equalizer"}),
         "AutoEQ import should produce an equalizer-only plugin order");
  expect_near(parsed.equalizer.input_gain_db, expected_preamp, 1e-6, "AutoEQ preamp should map to equalizer input gain");
  expect(parsed.equalizer.num_bands == 10, "AutoEQ fixture should import all ten bands");
  expect(parsed.equalizer.left[0].type == "Lo-shelf", "LSC should map to Lo-shelf");
  expect(parsed.equalizer.left[1].type == "Bell", "PK should map to Bell");
  expect(parsed.equalizer.left[9].type == "Hi-shelf", "HSC should map to Hi-shelf");
  expect(parsed.equalizer.left[1].mode == "APO (DR)", "imported AutoEQ bands should use APO (DR) mode");
  expect_near(parsed.equalizer.left[0].frequency, expected_first_frequency, 1e-6, "first band frequency should be preserved");
  expect_near(parsed.equalizer.left[9].gain_db, expected_last_gain, 1e-6, "last band gain should be preserved");
}

void test_parse_autoeq_requires_supported_filter_lines() {
  constexpr std::string_view autoeq = R"(Preamp: -2.25 dB
# comment only
Filter 1: OFF PK Fc 105.0 Hz Gain -2.4 dB Q 0.70
)";

  std::string error;
  const auto parsed = ee::parse_autoeq_preset(autoeq, error);
  expect(error == "no supported AutoEQ filter lines were found",
         "AutoEQ import should fail if no enabled supported filter lines remain");
  expect(parsed.plugin_order.empty(), "failed AutoEQ import should not produce a parsed preset");
}

void test_parse_autoeq_unsupported_filter_type() {
  constexpr std::string_view autoeq = R"(Filter 1: ON LS 6dB Fc 105.0 Hz Gain -2.4 dB Q 0.70
)";

  std::string error;
  const auto parsed = ee::parse_autoeq_preset(autoeq, error);
  expect(error == "unsupported AutoEQ filter type 'LS 6DB'",
         "unsupported AutoEQ filter types should produce an actionable error");
  expect(parsed.plugin_order.empty(), "unsupported AutoEQ filter types should not produce a parsed preset");
}

void test_render_easy_effects_preset_json_round_trip() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("Samsung Galaxy Buds Pro 2.txt"), error);
  expect(error.empty(), "AutoEQ fixture for round-trip should load");

  std::string parse_error;
  const auto imported = ee::parse_autoeq_preset(loaded.bytes, parse_error);
  expect(parse_error.empty(), "AutoEQ import for round-trip should parse");

  const auto rendered = ee::render_easy_effects_preset_json(imported);
  std::string round_trip_error;
  const auto reparsed = ee::parse_easy_effects_preset(rendered, round_trip_error);
  expect(round_trip_error.empty(), "rendered AutoEQ JSON should parse as an EasyEffects preset");
  expect(reparsed.plugin_order == std::vector<std::string>({"equalizer"}),
         "rendered AutoEQ JSON should remain equalizer-only");
  expect(reparsed.equalizer.num_bands == 10, "rendered AutoEQ JSON should preserve band count");
  expect(reparsed.equalizer.left[0].mode == "APO (DR)", "rendered AutoEQ JSON should preserve APO band mode");
  expect_near(reparsed.equalizer.left[0].frequency, 105.0, 1e-6, "rendered AutoEQ JSON should preserve frequency");
}

void test_resolve_kernel_exact_match() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for kernel resolution should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should be created");

  std::ofstream file(ir_dir / "room.irs", std::ios::binary);
  expect(file.is_open(), "exact-match IR file should be writable");
  file << "stub";
  file.close();

  ee::ConvolverPreset preset;
  preset.kernel_name = "room";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(resolved.has_value(), "exact kernel match should resolve");
  expect(warning.empty(), "exact kernel match should not warn");
  expect(resolved.has_value() && resolved->name == "room", "exact kernel match should preserve the kernel name");
}

void test_resolve_kernel_fuzzy_match() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for fuzzy kernel resolution should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should exist for fuzzy match");

  std::ofstream file(ir_dir / "Room Correction.irs", std::ios::binary);
  expect(file.is_open(), "fuzzy-match IR file should be writable");
  file << "stub";
  file.close();

  ee::ConvolverPreset preset;
  preset.kernel_name = "room_correction";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(resolved.has_value(), "fuzzy kernel match should resolve");
  expect(warning.find("fuzzy local match") != std::string::npos, "fuzzy kernel match should emit a warning");
  expect(resolved.has_value() && resolved->name == "Room Correction",
         "fuzzy kernel match should return the matched local file name");
}

void test_resolve_kernel_missing() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for missing kernel resolution should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  setenv("XDG_DATA_HOME", temp_dir.path.c_str(), 1);

  ee::ConvolverPreset preset;
  preset.kernel_name = "does-not-exist";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(!resolved.has_value(), "missing kernel should not resolve");
  expect(warning.find("convolver kernel not found") != std::string::npos,
         "missing kernel should emit a not-found warning");
}

void test_convolver_validate_rate() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for convolver rate validation should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should be created for convolver rate validation");

  const auto ir_path = ir_dir / "rate-check.irs";
  {
    SndfileHandle sndfile(ir_path.string(), SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 2, 44100);
    expect(sndfile.error() == 0, "rate-check fixture should be writable as audio");
    const std::array<float, 4> frames = {1.0F, 1.0F, 0.0F, 0.0F};
    expect(sndfile.writef(frames.data(), 2) == 2, "rate-check audio fixture should be written");
  }

  ee::ResolvedKernel kernel{
      .name = "rate-check",
      .path = ir_path.string(),
      .is_sofa = false,
  };

  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;
  const auto loaded = host.load(preset, kernel, error);
  expect(loaded, "valid IR fixture should load into the convolver host");
  expect(error.empty(), "valid IR fixture should load without error");

  error.clear();
  expect(!host.validate_rate(48000, error), "mismatched convolver rate should be rejected");
  expect(error == "convolver kernel sample rate 44100 Hz does not match active stream rate 48000 Hz",
         "mismatched convolver rate should produce a clear error");

  error.clear();
  expect(host.validate_rate(44100, error), "matching convolver rate should be accepted");
  expect(error.empty(), "matching convolver rate should not warn");
}

void test_db_to_linear_identity() {
  expect_near(ee::math::db_to_linear(0.0), 1.0, 1e-9, "0 dB should equal unity gain");
}

void test_db_to_linear_positive() {
  expect_near(ee::math::db_to_linear(6.0), 1.99526, 1e-4, "+6 dB should roughly double amplitude");
  expect_near(ee::math::db_to_linear(20.0), 10.0, 1e-4, "+20 dB should equal 10x amplitude");
}

void test_db_to_linear_negative() {
  expect_near(ee::math::db_to_linear(-6.0), 0.50119, 1e-4, "-6 dB should roughly halve amplitude");
  expect_near(ee::math::db_to_linear(-20.0), 0.1, 1e-4, "-20 dB should equal 0.1x amplitude");
}

void test_db_to_linear_extreme() {
  expect_near(ee::math::db_to_linear(-100.0), 1e-5, 1e-7, "-100 dB should be near silence");
}

void test_label_index_first_element() {
  using namespace ee::labels;
  expect(label_index(kBandTypeLabels, "Off") == 0.0F, "first band type 'Off' should be index 0");
  expect(label_index(kEqModeLabels, "IIR") == 0.0F, "first EQ mode 'IIR' should be index 0");
}

void test_label_index_middle_elements() {
  using namespace ee::labels;
  expect(label_index(kBandTypeLabels, "Bell") == 1.0F, "'Bell' should be index 1");
  expect(label_index(kEqModeLabels, "FFT") == 2.0F, "'FFT' should be index 2");
}

void test_label_index_last_elements() {
  using namespace ee::labels;
  expect(label_index(kBandTypeLabels, "Ladder-rej") == 11.0F, "last band type should be index 11");
  expect(label_index(kLimiterModeLabels, "Line Duck") == 11.0F, "last limiter mode should be index 11");
}

void test_label_index_unknown_returns_zero() {
  using namespace ee::labels;
  expect(label_index(kBandTypeLabels, "Unknown") == 0.0F, "unknown label should fall back to index 0");
  expect(label_index(kEqModeLabels, "Nonexistent") == 0.0F, "unknown EQ mode should fall back to index 0");
}

void test_cli_args_help_flag() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--help"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--help should not produce an error");
  expect(args.show_help, "--help should set show_help");
}

void test_cli_args_help_short() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "-h"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "-h should not produce an error");
  expect(args.show_help, "-h should set show_help");
}

void test_cli_args_version_flag() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--version"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--version should not produce an error");
  expect(args.show_version, "--version should set show_version");
}

void test_cli_args_version_short() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "-v"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "-v should not produce an error");
  expect(args.show_version, "-v should set show_version");
}

void test_cli_args_dry_run() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", fixture_path("Boosted.json"), "--dry-run"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--dry-run should parse without error");
  expect(args.dry_run, "--dry-run should set dry_run");
}

void test_cli_args_dry_run_short() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", fixture_path("Boosted.json"), "-d"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "-d should parse without error");
  expect(args.dry_run, "-d should set dry_run");
}

void test_cli_args_sink_selector() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", fixture_path("Boosted.json"), "--sink", "my_sink"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--sink should parse without error");
  expect(args.sink_selector == "my_sink", "--sink should set sink_selector");
}

void test_cli_args_sink_short() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset", fixture_path("Boosted.json"), "-s", "my_sink"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "-s should parse without error");
  expect(args.sink_selector == "my_sink", "-s should set sink_selector");
}

void test_cli_args_preset_short() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "-p", fixture_path("Boosted.json")};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "-p should parse without error");
  expect(args.preset_source == fixture_path("Boosted.json"), "-p should set preset_source");
}

void test_cli_args_unknown_option() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--bogus"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error == "unknown option: --bogus", "unknown option should produce the expected error");
}

void test_cli_args_missing_preset_value() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--preset"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error == "missing value for --preset", "missing --preset value should produce the expected error");
}

void test_cli_args_missing_sink_value() {
  std::string error;
  const std::vector<std::string> arguments = {"eq-cli", "--sink"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error == "missing value for --sink", "missing --sink value should produce the expected error");
}

// --- Parser edge cases ---

constexpr auto kIrsFixtureName = "Razor Surround ((48k Z-Edition)) 2.Stereo +20 bass Low Latency";

auto minimal_eq_json(std::string_view plugins_order_fragment,
                     std::string_view extra_output_fields = "") -> std::string {
  return std::format(R"({{
    "output": {{
      "plugins_order": [{}],
      "equalizer": {{
        "mode": "IIR", "num-bands": 2, "split-channels": false,
        "left": {{}}, "right": {{}}
      }}{}
    }}
  }})", plugins_order_fragment, extra_output_fields);
}

void test_parse_missing_output_section() {
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset("{}", error);
  expect(error == "missing output section", "missing output should produce the expected error");
}

void test_parse_missing_plugins_order() {
  constexpr std::string_view json = R"({"output": {"equalizer": {}}})";
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error == "missing output.plugins_order", "missing plugins_order should produce the expected error");
}

void test_parse_empty_plugins_order() {
  constexpr std::string_view json = R"({"output": {"plugins_order": []}})";
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error == "empty output.plugins_order is not supported", "empty plugins_order should produce the expected error");
}

void test_parse_no_equalizer_in_plugins() {
  constexpr std::string_view json = R"({
    "output": {
      "plugins_order": ["limiter"],
      "limiter": {}
    }
  })";
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error == "equalizer is required in output.plugins_order",
         "plugins_order without equalizer should produce the expected error");
}

void test_parse_unsupported_plugin_warning() {
  const auto json = minimal_eq_json(R"("equalizer", "compressor")");
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error.empty(), "unsupported plugin should not cause a hard error");
  expect(parsed.warnings.size() == 1, "unsupported plugin should produce exactly one warning");
  expect(!parsed.warnings.empty() && parsed.warnings[0] == "skipping unsupported plugin: compressor",
         "unsupported plugin warning should name the plugin");
}

void test_parse_num_bands_clamped_high() {
  const auto json = std::format(R"({{
    "output": {{
      "plugins_order": ["equalizer"],
      "equalizer": {{
        "mode": "IIR", "num-bands": 99, "split-channels": false,
        "left": {{}}, "right": {{}}
      }}
    }}
  }})");
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error.empty(), "num-bands 99 should not error");
  expect(parsed.equalizer.num_bands == 32, "num-bands 99 should be clamped to 32");
}

void test_parse_num_bands_clamped_low() {
  const auto json = std::format(R"({{
    "output": {{
      "plugins_order": ["equalizer"],
      "equalizer": {{
        "mode": "IIR", "num-bands": 0, "split-channels": false,
        "left": {{}}, "right": {{}}
      }}
    }}
  }})");
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error.empty(), "num-bands 0 should not error");
  expect(parsed.equalizer.num_bands == 1, "num-bands 0 should be clamped to 1");
}

void test_parse_split_channels() {
  constexpr std::string_view json = R"({
    "output": {
      "plugins_order": ["equalizer"],
      "equalizer": {
        "mode": "IIR", "num-bands": 2, "split-channels": true,
        "left": { "band0": { "gain": 5.0 } },
        "right": { "band0": { "gain": -3.0 } }
      }
    }
  })";
  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(json, error);
  expect(error.empty(), "split-channels preset should parse");
  expect(parsed.equalizer.split_channels, "split-channels should be true");
  expect(parsed.equalizer.left[0].gain_db == 5.0, "left band0 gain should be 5.0");
  expect(parsed.equalizer.right[0].gain_db == -3.0, "right band0 gain should be -3.0");
}

void test_parse_wrong_typed_fields_return_error() {
  // Wrong-typed fields must produce a parse error, not an uncaught
  // nlohmann::json exception (which would terminate the daemon).
  constexpr std::array<std::string_view, 4> presets = {
      // non-string entry in plugins_order
      R"({"output": {"plugins_order": ["equalizer", 3], "equalizer": {"left": {}, "right": {}}}})",
      // equalizer payload is not an object
      R"({"output": {"plugins_order": ["equalizer"], "equalizer": 42}})",
      // scalar field present with the wrong type
      R"({"output": {"plugins_order": ["equalizer"], "equalizer": {"input-gain": "0", "left": {}, "right": {}}}})",
      // wrong-typed field nested in a band
      R"({"output": {"plugins_order": ["equalizer"], "equalizer": {"left": {"band0": {"gain": "5"}}, "right": {}}}})",
  };
  for (const auto preset_json : presets) {
    std::string error;
    const auto parsed = ee::parse_easy_effects_preset(preset_json, error);
    expect(!error.empty(), std::format("wrong-typed preset should produce an error: {}", preset_json));
    expect(parsed.plugin_order.empty(), "wrong-typed preset should return an empty result");
  }
}

// --- Convolver tests with real IRS ---

void test_convolver_load_real_irs() {
  const auto irs_path = fixture_path(std::string(kIrsFixtureName) + ".irs");
  ee::ResolvedKernel kernel{.name = kIrsFixtureName, .path = irs_path, .is_sofa = false};
  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;

  expect(host.load(preset, kernel, error), "real IRS fixture should load");
  expect(error.empty(), "real IRS fixture should load without error");

  error.clear();
  expect(host.validate_rate(48000, error), "real IRS at 48kHz should accept 48kHz stream rate");
  expect(error.empty(), "matching rate should not produce an error");
}

void test_convolver_process_round_trip() {
  const auto irs_path = fixture_path(std::string(kIrsFixtureName) + ".irs");
  ee::ResolvedKernel kernel{.name = kIrsFixtureName, .path = irs_path, .is_sofa = false};
  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;

  expect(host.load(preset, kernel, error), "IRS should load for process test");
  expect(host.ensure_ready(256), "convolver should become ready with block size 256");

  std::vector<float> left(256, 0.0F);
  std::vector<float> right(256, 0.0F);
  left[0] = 1.0F;
  right[0] = 1.0F;

  expect(host.process(left, right), "convolver process should succeed");

  expect(stereo_energy(left, right) > 0.0F, "convolver output should contain non-zero energy after impulse input");
}

void test_convolver_process_non_power_of_two_block_size() {
  const auto irs_path = fixture_path(std::string(kIrsFixtureName) + ".irs");
  ee::ResolvedKernel kernel{.name = kIrsFixtureName, .path = irs_path, .is_sofa = false};
  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;

  expect(host.load(preset, kernel, error), "IRS should load for non-power-of-two block size test");
  expect(host.ensure_ready(96), "convolver should become ready with stream block size 96");

  std::vector<float> left(96, 0.0F);
  std::vector<float> right(96, 0.0F);
  left[0] = 1.0F;
  right[0] = 1.0F;

  float energy = 0.0F;
  for (int i = 0; i < 3; ++i) {
    expect(host.process(left, right), "non-power-of-two stream block should process through the adapter");
    energy += stereo_energy(left, right);
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
  }

  expect(energy > 0.0F, "non-power-of-two stream block should produce convolver output");
}

void test_convolver_process_sub_minimum_block_size() {
  const auto irs_path = fixture_path(std::string(kIrsFixtureName) + ".irs");
  ee::ResolvedKernel kernel{.name = kIrsFixtureName, .path = irs_path, .is_sofa = false};
  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;

  expect(host.load(preset, kernel, error), "IRS should load for sub-minimum block size test");
  expect(host.ensure_ready(32), "convolver should become ready with stream block size 32");

  std::vector<float> left(32, 0.0F);
  std::vector<float> right(32, 0.0F);
  left[0] = 1.0F;
  right[0] = 1.0F;

  float energy = 0.0F;
  for (int i = 0; i < 4; ++i) {
    expect(host.process(left, right), "sub-64 stream block should process through the adapter");
    energy += stereo_energy(left, right);
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
  }

  expect(energy > 0.0F, "sub-64 stream block should produce convolver output");
}

void test_convolver_process_unconfigured_stream_block_size() {
  const auto irs_path = fixture_path(std::string(kIrsFixtureName) + ".irs");
  ee::ResolvedKernel kernel{.name = kIrsFixtureName, .path = irs_path, .is_sofa = false};
  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;

  expect(host.load(preset, kernel, error), "IRS should load for stream block size mismatch test");
  expect(host.ensure_ready(256), "convolver should become ready with stream block size 256");

  std::vector<float> left(512, 0.0F);
  std::vector<float> right(512, 0.0F);
  expect(!host.process(left, right), "process with unconfigured stream block size should fail");
}

// --- Kernel resolver with real IRS ---

void test_resolve_kernel_real_irs() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temp dir for real IRS resolver test should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should be created");

  const auto src = fixture_path(std::string(kIrsFixtureName) + ".irs");
  const auto dst = ir_dir / (std::string(kIrsFixtureName) + ".irs");
  std::filesystem::copy_file(src, dst);

  ee::ConvolverPreset preset;
  preset.kernel_name = kIrsFixtureName;
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(resolved.has_value(), "real IRS should resolve by exact name");
  expect(warning.empty(), "exact real IRS match should not warn");
  expect(resolved.has_value() && resolved->name == kIrsFixtureName, "resolved name should match");
  expect(resolved.has_value() && resolved->path == dst.string(), "resolved path should point to the copied IRS");
}

void test_resolve_kernel_name_from_path() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temp dir for kernel-path resolver test should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should be created");

  const auto src = fixture_path(std::string(kIrsFixtureName) + ".irs");
  const auto dst = ir_dir / (std::string(kIrsFixtureName) + ".irs");
  std::filesystem::copy_file(src, dst);

  ee::ConvolverPreset preset;
  preset.kernel_path = "/some/old/path/" + std::string(kIrsFixtureName) + ".irs";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(resolved.has_value(), "kernel_path stem should resolve when IRS exists locally");
  expect(warning.empty(), "kernel_path exact match should not warn");
  expect(resolved.has_value() && resolved->name == kIrsFixtureName,
         "resolved name should be derived from kernel_path stem");
}

}  // namespace

int main() {
  test_cli_args_accept_local_preset();
  test_cli_args_convert_autoeq();
  test_cli_args_convert_autoeq_output();
  test_cli_args_output_requires_convert_autoeq();
  test_cli_args_convert_autoeq_conflicts_with_preset();
  test_cli_args_reject_url_preset();
  test_cli_args_allow_list_sinks_without_preset();
  test_cli_args_require_preset_or_env();
  test_cli_args_use_default_preset_env();
  test_cli_args_cli_wins_over_default_preset_env();
  test_load_preset_local_file();
  test_load_preset_missing_file();
  test_parse_fixture_preset();
  test_parse_fixture_with_convolver_and_limiter();
  test_parse_invalid_preset();
  test_parse_duplicate_plugin_kind_rejected();
  test_parse_autoeq_fixture("Samsung Galaxy Buds Pro 2.txt", -3.76, 105.0, -4.4);
  test_parse_autoeq_fixture("Sony MH755.txt", -2.43, 105.0, -2.4);
  test_parse_autoeq_fixture("Anker Soundcore Life Q20.txt", -3.63, 105.0, 0.7);
  test_parse_autoeq_requires_supported_filter_lines();
  test_parse_autoeq_unsupported_filter_type();
  test_render_easy_effects_preset_json_round_trip();
  test_resolve_kernel_exact_match();
  test_resolve_kernel_fuzzy_match();
  test_resolve_kernel_missing();
  test_convolver_validate_rate();

  test_db_to_linear_identity();
  test_db_to_linear_positive();
  test_db_to_linear_negative();
  test_db_to_linear_extreme();

  test_label_index_first_element();
  test_label_index_middle_elements();
  test_label_index_last_elements();
  test_label_index_unknown_returns_zero();

  test_cli_args_help_flag();
  test_cli_args_help_short();
  test_cli_args_version_flag();
  test_cli_args_version_short();
  test_cli_args_dry_run();
  test_cli_args_dry_run_short();
  test_cli_args_sink_selector();
  test_cli_args_sink_short();
  test_cli_args_preset_short();
  test_cli_args_unknown_option();
  test_cli_args_missing_preset_value();
  test_cli_args_missing_sink_value();

  test_parse_missing_output_section();
  test_parse_missing_plugins_order();
  test_parse_empty_plugins_order();
  test_parse_no_equalizer_in_plugins();
  test_parse_unsupported_plugin_warning();
  test_parse_num_bands_clamped_high();
  test_parse_num_bands_clamped_low();
  test_parse_split_channels();
  test_parse_wrong_typed_fields_return_error();

  test_convolver_load_real_irs();
  test_convolver_process_round_trip();
  test_convolver_process_non_power_of_two_block_size();
  test_convolver_process_sub_minimum_block_size();
  test_convolver_process_unconfigured_stream_block_size();

  test_resolve_kernel_real_irs();
  test_resolve_kernel_name_from_path();

  if (g_failures != 0) {
    ee::log::error(std::format("{} test assertion(s) failed", g_failures));
    return 1;
  }

  ee::log::info("eq-cli tests passed");
  return 0;
}
