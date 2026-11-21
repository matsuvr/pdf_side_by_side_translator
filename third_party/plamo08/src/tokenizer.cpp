#include "plamo08/tokenizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace plamo08 {
namespace {

using json = nlohmann::json;
constexpr char32_t kBoundary = 0xEE00;
constexpr int kBoundaryId = 10000000;
constexpr std::int64_t kUnknownCost = 10000000;
constexpr std::int64_t kInvalidScore = -20000000;

// Strict UTF-8 decoding makes character boundaries match Python Unicode.
std::u32string codepoints(const std::string& text) {
  std::u32string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    unsigned length = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2
                               : c >= 0xE0 && c <= 0xEF ? 3
                               : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (length == 0 || i + length > text.size())
      throw std::runtime_error("Tokenizer input is not valid UTF-8");
    char32_t cp = c & (length == 1 ? 0x7F : (1u << (7 - length)) - 1);
    for (unsigned j = 1; j < length; ++j) {
      const auto b = static_cast<unsigned char>(text[i + j]);
      if ((b & 0xC0) != 0x80)
        throw std::runtime_error("Tokenizer input is not valid UTF-8");
      cp = (cp << 6) | (b & 0x3F);
    }
    if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) ||
        (length == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
      throw std::runtime_error("Tokenizer input is not valid UTF-8");
    out.push_back(cp);
    i += length;
  }
  return out;
}

std::string utf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) out.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

// Python str.isspace()/re's Unicode \\s set, including ASCII separators.
bool whitespace(char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) ||
         c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
         c == 0x202F || c == 0x205F || c == 0x3000;
}

std::u32string boundary_preprocess(const std::u32string& text,
                                  int space_threshold, int repeat_threshold) {
  static const std::u32string prefix = U"<|plamo:";
  std::u32string special;
  special.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    std::size_t end = i;
    if (text.compare(i, prefix.size(), prefix) == 0) {
      std::size_t p = i + prefix.size();
      std::size_t count = 0;
      while (p < text.size() && count < 64 && text[p] != U'|' && !whitespace(text[p])) {
        ++p;
        ++count;
      }
      if (p + 1 < text.size() && text[p] == U'|' && text[p + 1] == U'>') end = p + 2;
    }
    if (end > i) {
      special.push_back(kBoundary);
      special.append(text, i, end - i);
      special.push_back(kBoundary);
      i = end;
    } else special.push_back(text[i++]);
  }
  std::u32string result;
  result.reserve(special.size());
  for (std::size_t i = 0; i < special.size();) {
    std::size_t end = i + 1;
    while (end < special.size() && special[end] == special[i]) ++end;
    const auto count = end - i;
    const bool repeated = repeat_threshold > 0 && special[i] != U'\n' &&
                          count >= static_cast<std::size_t>(repeat_threshold);
    const bool spaces = space_threshold > 0 && special[i] == U' ' &&
                        count >= static_cast<std::size_t>(space_threshold);
    if (repeated || spaces) result.push_back(kBoundary);
    result.append(special, i, count);
    if (repeated || spaces) result.push_back(kBoundary);
    i = end;
  }
  return result;
}

std::int64_t rounded_score(double score) {
  // Python round() uses nearest-even; std::round() would differ at ties.
  const double scaled = score * 10000.0;
  const double floor_value = std::floor(scaled);
  const double fraction = scaled - floor_value;
  auto result = static_cast<std::int64_t>(floor_value);
  if (fraction > 0.5 || (fraction == 0.5 && (result & 1))) ++result;
  return result;
}

json read_json_if_present(const std::filesystem::path& path) {
  std::ifstream in(path);
  return in ? json::parse(in) : json::object();
}

// Equivalent to bytes.decode('utf-8', errors='replace'). Invalid continuation
// bytes are reconsidered, while an incomplete valid prefix is replaced once.
std::string replace_invalid_utf8(const std::string& bytes) {
  std::string result;
  result.reserve(bytes.size());
  constexpr const char* replacement = "\xEF\xBF\xBD";
  for (std::size_t i = 0; i < bytes.size();) {
    const auto lead = static_cast<unsigned char>(bytes[i]);
    if (lead < 0x80) { result.push_back(bytes[i++]); continue; }
    const unsigned n = lead >= 0xC2 && lead <= 0xDF ? 2
                     : lead >= 0xE0 && lead <= 0xEF ? 3
                     : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
    if (n == 0) { result += replacement; ++i; continue; }
    std::size_t consumed = 1;
    while (consumed < n && i + consumed < bytes.size()) {
      const auto next = static_cast<unsigned char>(bytes[i + consumed]);
      if ((next & 0xC0) != 0x80) break;
      if (consumed == 1 && ((lead == 0xE0 && next < 0xA0) ||
                            (lead == 0xED && next >= 0xA0) ||
                            (lead == 0xF0 && next < 0x90) ||
                            (lead == 0xF4 && next >= 0x90))) break;
      ++consumed;
    }
    if (consumed == n) result.append(bytes, i, n);
    else result += replacement;
    i += consumed;
  }
  return result;
}

}  // namespace

struct Tokenizer::Impl {
  struct Node {
    std::uint32_t start = 0;
    std::uint32_t count = 0;
    int token = -1;
    std::int64_t score = kInvalidScore;
  };
  struct Edge { char32_t cp; std::uint32_t child; };
  std::vector<Node> nodes{1};
  std::vector<Edge> edges;
  std::vector<std::string> pieces;
  std::vector<std::string> raw_bytes;
  std::vector<TokenType> types;
  std::unordered_map<std::string, int> ids;
  std::array<int, 256> byte_ids{};
  std::unordered_set<int> eos;
  int space_threshold = 0;
  int repeat_threshold = 0;

  explicit Impl(const std::filesystem::path& model_dir, const std::function<void()>& checkpoint) {
    const auto config = read_json_if_present(model_dir / "tokenizer_config.json");
    if (config.contains("break_around_consecutive_spaces_threshold") &&
        !config["break_around_consecutive_spaces_threshold"].is_null())
      space_threshold = config["break_around_consecutive_spaces_threshold"].get<int>();
    if (config.contains("break_around_repeated_chars_threshold") &&
        !config["break_around_repeated_chars_threshold"].is_null())
      repeat_threshold = config["break_around_repeated_chars_threshold"].get<int>();
    byte_ids.fill(-1);
    std::unordered_map<std::uint64_t, std::uint32_t> links;
    auto insert = [&](const std::string& token, int id, std::int64_t score) {
      std::uint32_t node = 0;
      for (char32_t cp : codepoints(token)) {
        const std::uint64_t key = (std::uint64_t(node) << 32) | std::uint32_t(cp);
        auto it = links.find(key);
        if (it == links.end()) {
          const auto child = static_cast<std::uint32_t>(nodes.size());
          links.emplace(key, child);
          nodes.emplace_back();
          node = child;
        } else node = it->second;
      }
      nodes[node].token = id;
      nodes[node].score = score;
    };
    std::ifstream in(model_dir / "tokenizer.jsonl");
    if (!in) throw std::runtime_error("Cannot open tokenizer.jsonl in " + model_dir.string());
    std::string line;
    while (std::getline(in, line)) {
      if (checkpoint && pieces.size() % 1024 == 0) checkpoint();
      const auto row = json::parse(line);
      if (!row.is_array() || row.size() < 2 || !row[0].is_string() || !row[1].is_number())
        throw std::runtime_error("Invalid tokenizer.jsonl vocabulary entry");
      const std::string piece = row[0].get<std::string>();
      const auto id = static_cast<int>(pieces.size());
      pieces.push_back(piece);
      ids[piece] = id;
      const bool byte = row.size() > 2 && row[2] == "BYTE";
      const auto kind = row.size() > 2 ? row[2].get<std::string>() : "NORMAL";
      types.push_back(byte ? TokenType::byte : kind == "CONTROL" ? TokenType::control :
                      kind == "UNKNOWN" ? TokenType::unknown : kind == "UNUSED" ? TokenType::unused : TokenType::normal);
      if (byte) {
        if (piece.size() != 6 || piece.compare(0, 3, "<0x") != 0 || piece.back() != '>')
          throw std::runtime_error("Invalid byte-fallback vocabulary entry: " + piece);
        const auto value = std::stoul(piece.substr(3, 2), nullptr, 16);
        byte_ids.at(value) = id;
        raw_bytes.emplace_back(1, static_cast<char>(value));
      } else {
        const auto score = row[1].get<double>();
        if (!std::isfinite(score)) throw std::runtime_error("Non-finite tokenizer score");
        raw_bytes.push_back(piece);
        insert(piece, id, rounded_score(score));
      }
    }
    for (int id : byte_ids)
      if (id < 0) throw std::runtime_error("tokenizer.jsonl must contain all 256 byte-fallback entries");
    insert(utf8(kBoundary), kBoundaryId, 0);

    std::vector<std::tuple<std::uint32_t, char32_t, std::uint32_t>> sorted;
    sorted.reserve(links.size());
    for (const auto& [key, child] : links)
      sorted.emplace_back(static_cast<std::uint32_t>(key >> 32),
                          static_cast<char32_t>(key & 0xFFFFFFFF), child);
    std::sort(sorted.begin(), sorted.end());
    edges.reserve(sorted.size());
    for (const auto& [parent, cp, child] : sorted) {
      if (nodes[parent].count == 0) nodes[parent].start = static_cast<std::uint32_t>(edges.size());
      ++nodes[parent].count;
      edges.push_back({cp, child});
    }

    const auto generation = read_json_if_present(model_dir / "generation_config.json");
    if (generation.contains("eos_token_id")) {
      const auto& value = generation["eos_token_id"];
      if (value.is_array()) for (const auto& id : value) eos.insert(id.get<int>());
      else if (value.is_number_integer()) eos.insert(value.get<int>());
    }
    if (eos.empty() && config.contains("eos_token")) {
      const auto& value = config["eos_token"];
      const auto token = value.is_string() ? value.get<std::string>() : value.value("content", "");
      const auto it = ids.find(token);
      if (it != ids.end()) eos.insert(it->second);
    }
    // A new PFML section ends a translation, as in the reference CLI.
    if (const auto it = ids.find("<|plamo:tag|>"); it != ids.end()) eos.insert(it->second);
  }

  std::uint32_t child(std::uint32_t node, char32_t cp) const {
    const auto begin = edges.begin() + nodes[node].start;
    const auto end = begin + nodes[node].count;
    const auto it = std::lower_bound(begin, end, cp,
                                    [](const Edge& edge, char32_t value) { return edge.cp < value; });
    return it != end && it->cp == cp ? it->child : std::numeric_limits<std::uint32_t>::max();
  }
};

Tokenizer::Tokenizer(const std::filesystem::path& model_dir, const std::function<void()>& checkpoint)
    : impl_(std::make_shared<Impl>(model_dir, checkpoint)) {}

std::vector<int> Tokenizer::encode(const std::string& text, const std::function<void()>& checkpoint) const {
  if (checkpoint) checkpoint();
  const auto data = boundary_preprocess(codepoints(text), impl_->space_threshold, impl_->repeat_threshold);
  struct Choice { std::size_t length = 1; int token = -1; };
  std::vector<std::int64_t> cost(data.size() + 1, 0);
  std::vector<Choice> choice(data.size());
  for (std::size_t i = data.size(); i-- > 0;) {
    if (checkpoint && i % 1024 == 0) checkpoint();
    cost[i] = cost[i + 1] + kUnknownCost;
    std::uint32_t node = 0;
    for (std::size_t j = i; j < data.size(); ++j) {
      node = impl_->child(node, data[j]);
      if (node == std::numeric_limits<std::uint32_t>::max()) break;
      const auto& candidate = impl_->nodes[node];
      if (candidate.token < 0 || candidate.score <= kInvalidScore) continue;
      const auto next_cost = cost[j + 1] - candidate.score;
      const auto length = j + 1 - i;
      // The original backwards Aho-Corasick traversal considers longest
      // prefixes first. Explicitly preserve that tie break with a forward trie.
      if (next_cost < cost[i] || (next_cost == cost[i] && length >= choice[i].length)) {
        cost[i] = next_cost;
        choice[i] = {length, candidate.token};
      }
    }
  }
  std::vector<int> result;
  result.reserve(data.size());
  for (std::size_t pos = 0; pos < data.size();) {
    const auto selected = choice[pos];
    if (selected.token >= 0) {
      if (selected.token != kBoundaryId) result.push_back(selected.token);
    } else {
      for (const unsigned char byte : utf8(data[pos])) result.push_back(impl_->byte_ids[byte]);
    }
    pos += selected.length;
  }
  return result;
}

std::string Tokenizer::decode(const std::vector<int>& ids) const {
  std::string bytes;
  for (int id : ids) bytes += token_bytes(id);
  return replace_invalid_utf8(bytes);
}

std::string Tokenizer::token_bytes(int id) const {
  if (id < 0 || static_cast<std::size_t>(id) >= impl_->raw_bytes.size())
    throw std::out_of_range("Token ID outside vocabulary: " + std::to_string(id));
  return impl_->raw_bytes[id];
}

TokenType Tokenizer::token_type(int id) const { return impl_->types.at(id); }

int Tokenizer::token_id(const std::string& token) const {
  const auto it = impl_->ids.find(token);
  if (it == impl_->ids.end()) throw std::invalid_argument("Unknown token: " + token);
  return it->second;
}

bool Tokenizer::is_eos(int id) const { return impl_->eos.count(id) != 0; }
std::size_t Tokenizer::vocab_size() const { return impl_->pieces.size(); }

std::string build_translation_prompt(const std::string& text,
                                     const std::string& source_language,
                                     const std::string& target_language) {
  if (target_language.empty()) throw std::invalid_argument("A target language code is required");
  auto code = [](const std::string& value) {
    if (value == "English") return std::string("en");
    if (value == "Japanese") return std::string("ja");
    if (value == "Japanese(easy)" || value == "ja(easy)") return std::string("ja-easy");
    return value;
  };
  std::string prompt = "<|plamo:bos|><|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input";
  if (!source_language.empty()) prompt += "<|plamo:key|>lang<|plamo:val|>" + code(source_language);
  prompt += "<|plamo:msg|>" + text + "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>" +
            code(target_language) + "<|plamo:msg|>";
  return prompt;
}

}  // namespace plamo08
