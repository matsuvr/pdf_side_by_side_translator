#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plamo08 {
enum class TokenType { normal, control, unknown, unused, byte };

// Native reader for PLaMo's tokenizer.jsonl Unicode Unigram vocabulary.
// encode() corresponds to add_special_tokens=False: the caller supplies PFML.
class Tokenizer {
 public:
  explicit Tokenizer(const std::filesystem::path& model_dir,
                     const std::function<void()>& checkpoint = {});

  std::vector<int> encode(const std::string& text,
                          const std::function<void()>& checkpoint = {}) const;
  std::string decode(const std::vector<int>& ids) const;
  // Individual byte-fallback pieces may be incomplete UTF-8. This accessor
  // preserves those bytes so a streaming client can buffer them correctly.
  std::string token_bytes(int id) const;
  TokenType token_type(int id) const;
  int token_id(const std::string& token) const;
  bool is_eos(int id) const;
  std::size_t vocab_size() const;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

// Language arguments accept model language codes, e.g. en, ja, zh-hans.
std::string build_translation_prompt(const std::string& text,
                                     const std::string& source_language,
                                     const std::string& target_language);

}  // namespace plamo08
