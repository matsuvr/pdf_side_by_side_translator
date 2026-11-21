#include "plamo08/tokenizer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;

void check(bool condition, const std::string& what) {
  if (!condition) throw std::runtime_error(what);
}

struct TemporaryDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("plamo08-tokenizer-" + std::to_string(
          std::chrono::high_resolution_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

void synthetic_tests() {
  TemporaryDirectory dir;
  std::ofstream vocab(dir.path / "tokenizer.jsonl");
  auto entry = [&](const std::string& piece, double score, const char* kind = "NORMAL") {
    vocab << json::array({piece, score, kind}).dump() << '\n';
  };
  entry("<|plamo:unk|>", 0, "UNKNOWN");
  entry("<|plamo:bos|>", 0, "CONTROL");
  entry("<|plamo:eos|>", 0, "CONTROL");
  entry("<|plamo:tag|>", 0, "CONTROL");
  for (int i = 0; i < 256; ++i) {
    std::ostringstream token;
    token << "<0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << i << ">";
    entry(token.str(), 0, "BYTE");
  }
  entry("a", -1); entry("b", -1); entry("ab", -2); entry("c", -1); entry("bc", -1.5);
  entry("x", -1); entry("y", -1); entry("  ", -0.5); entry("x  y", 0);
  entry("aaaa", -1); entry("xaaaay", 0);
  entry("<|plamo:bogus|>", 0, "CONTROL"); entry("x<|plamo:bogus|>y", 0);
  entry("d", -0.00005); entry("e", -0.00005); entry("de", -0.0001);
  entry("a\n\n\n\nb", 0);
  vocab.close();
  std::ofstream(dir.path / "tokenizer_config.json")
      << R"({"break_around_consecutive_spaces_threshold":2,"break_around_repeated_chars_threshold":4,"eos_token":"<|plamo:bos|>"})";
  std::ofstream(dir.path / "generation_config.json") << R"({"eos_token_id":[1,3]})";
  plamo08::Tokenizer tokenizer(dir.path);
  auto ids = [&](std::initializer_list<std::string> pieces) {
    std::vector<int> result;
    for (const auto& piece : pieces) result.push_back(tokenizer.token_id(piece));
    return result;
  };
  check(tokenizer.encode("").empty(), "empty input");
  check(tokenizer.encode("ab") == ids({"ab"}), "longest prefix wins equal Unigram score");
  check(tokenizer.encode("abc") == ids({"a", "bc"}), "globally best suffix beats greedy longest token");
  check(tokenizer.encode("de") == ids({"d", "e"}), "Python nearest-even score rounding");
  check(tokenizer.encode("x  y") == ids({"x", "  ", "y"}), "consecutive-space hard boundary");
  check(tokenizer.encode("xaaaay") == ids({"x", "aaaa", "y"}), "repeated-character hard boundary");
  check(tokenizer.encode("a\n\n\n\nb") == ids({"a\n\n\n\nb"}), "newline excluded from repeat matcher");
  check(tokenizer.encode("x<|plamo:bogus|>y") == ids({"x", "<|plamo:bogus|>", "y"}),
        "PFML special-token hard boundary");
  check(tokenizer.encode("<|plamo:bos|>ab") == ids({"<|plamo:bos|>", "ab"}), "BOS is explicit, inserted once");
  check(tokenizer.decode(tokenizer.encode("🧑‍🚀こんにちは")) == "🧑‍🚀こんにちは", "Unicode byte fallback round trip");
  check(tokenizer.decode(tokenizer.encode("a\xEE\xB8\x80" "b")) == "ab", "internal boundary token is removed");
  check(tokenizer.decode(ids({"<0xE3>", "<0x81>"})) == "\xEF\xBF\xBD", "incomplete UTF-8 prefix replacement");
  check(tokenizer.decode(ids({"<0xE3>", "<0x28>", "<0xA1>"})) == "\xEF\xBF\xBD(\xEF\xBF\xBD",
        "invalid UTF-8 continuation replacement");
  check(tokenizer.token_bytes(tokenizer.token_id("<0xE3>")) == std::string("\xE3"), "streaming preserves byte fragment");
  check(tokenizer.is_eos(1) && tokenizer.is_eos(3) && !tokenizer.is_eos(2), "model-specific EOS IDs");
  check(tokenizer.token_type(0) == plamo08::TokenType::unknown &&
        tokenizer.token_type(1) == plamo08::TokenType::control &&
        tokenizer.token_type(tokenizer.token_id("<0x00>")) == plamo08::TokenType::byte,
        "token attributes used by the viewer");
  int checkpoints = 0;
  bool cancelled = false;
  try { tokenizer.encode(std::string(4096, 'a'), [&] { if (++checkpoints == 2) throw std::runtime_error("cancel"); }); }
  catch (const std::runtime_error&) { cancelled = true; }
  check(cancelled && checkpoints == 2, "cancellation during tokenization");
  check(plamo08::build_translation_prompt("Hello", "", "ja(easy)").find("ja-easy<|plamo:msg|>") != std::string::npos,
        "official easy-Japanese PFML target");
  bool bad_utf8 = false;
  try { tokenizer.encode(std::string("\xC0\xAF")); } catch (const std::runtime_error&) { bad_utf8 = true; }
  check(bad_utf8, "invalid UTF-8 input rejected");
  const auto prompt = plamo08::build_translation_prompt("Hello", "en", "ja");
  check(prompt == "<|plamo:bos|><|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input"
                  "<|plamo:key|>lang<|plamo:val|>en<|plamo:msg|>Hello<|plamo:tag|>output"
                  "<|plamo:key|>lang<|plamo:val|>ja<|plamo:msg|>", "translation PFML layout");
  std::cout << "Tokenizer synthetic tests passed\n";
}

void official_vocabulary_tests(const std::filesystem::path& model,
                               const std::filesystem::path& fixtures) {
  plamo08::Tokenizer tokenizer(model);
  check(tokenizer.vocab_size() == 107520, "official vocabulary size");
  check(tokenizer.is_eos(1) && tokenizer.is_eos(16) && !tokenizer.is_eos(2), "official EOS IDs");
  std::ifstream in(fixtures);
  if (!in) throw std::runtime_error("Cannot open parity fixtures: " + fixtures.string());
  const auto examples = json::parse(in).at("cases");
  for (std::size_t i = 0; i < examples.size(); ++i) {
    const auto& example = examples[i];
    const auto encoded = tokenizer.encode(example.at("text").get<std::string>());
    const auto expected = example.at("ids").get<std::vector<int>>();
    check(encoded == expected, "official Python token-ID parity case " + std::to_string(i));
    check(tokenizer.decode(encoded) == example.at("decoded").get<std::string>(),
          "official Python decode parity case " + std::to_string(i));
  }
  std::cout << "Official tokenizer parity: " << examples.size() << " cases passed\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    synthetic_tests();
    if (argc > 1) {
      const auto fixture = argc > 2 ? std::filesystem::path(argv[2])
          : std::filesystem::path(__FILE__).parent_path() / "tokenizer_parity.json";
      official_vocabulary_tests(argv[1], fixture);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Tokenizer test failure: " << error.what() << '\n';
    return 1;
  }
}
