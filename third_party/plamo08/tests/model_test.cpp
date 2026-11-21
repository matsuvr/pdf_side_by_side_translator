// Run on Apple Silicon with the official 0.8B MLX 4bit model directory.
#include "plamo08/model.hpp"
#include "plamo08/tokenizer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string(argv[1]) != "--model")
            throw std::runtime_error("Usage: plamo08-model-test --model DIR");
        plamo08::Tokenizer tokenizer(argv[2]);
        plamo08::Model model(argv[2], 4096);
        const auto en = tokenizer.encode(plamo08::build_translation_prompt(
            "Apple Silicon shares memory between the CPU and GPU. Translate this sentence accurately.", "en", "ja"));
        const auto ja = tokenizer.encode(plamo08::build_translation_prompt(
            "桜の季節には、川沿いの道をゆっくり歩くのが楽しみです。", "ja", "en"));
        auto check = [&](const std::string& name, const std::vector<int>& prompt, int steps, int chunk) {
            const auto result = model.verify(prompt, steps, chunk);
            std::cout << name << ": steps=" << result.compared_steps
                      << " top1=" << result.same_argmax_steps
                      << " max_abs=" << result.max_absolute_error
                      << " relative_rms=" << result.max_relative_rms_error << '\n';
            // Reduction order may change near-tied logits. This catches indexing,
            // normalization, RoPE and stale-cache errors without assuming bit identity.
            if (!result.all_finite || result.compared_steps != steps ||
                !std::isfinite(result.max_relative_rms_error) || result.max_relative_rms_error > 0.03)
                throw std::runtime_error("Decoder parity failed: " + name);
        };
        check("English prompt", en, 8, 17);
        check("Japanese prompt", ja, 8, 256);
        check("Empty prefill", {tokenizer.token_id("<|plamo:bos|>")}, 4, 256);

        // Exercise the transition from unwrapped to wrapped 2048-slot local KV.
        std::vector<int> boundary;
        boundary.reserve(2051);
        while (boundary.size() < 2046) {
            const auto count = std::min(en.size(), std::size_t(2046) - boundary.size());
            boundary.insert(boundary.end(), en.begin(), en.begin() + count);
        }
        check("Sliding KV wrap during decode", boundary, 8, 256);
        boundary.resize(2051, en.back());
        check("Sliding KV already wrapped after prefill", boundary, 4, 257);

        // A reset reuses allocated caches while excluding every previous token.
        const plamo08::GenerationOptions settings{8, 256, plamo08::Backend::fused};
        auto eos = [&](int id) { return tokenizer.is_eos(id); };
        const auto first = model.generate(en, eos, settings).token_ids;
        model.generate(ja, eos, settings);
        const auto again = model.generate(en, eos, settings).token_ids;
        if (first != again) throw std::runtime_error("Reset leaked previous translation state");
        int delivered = 0;
        const auto limited = model.generate(en, [](int) { return false; }, settings,
            [&](int) { ++delivered; return false; });
        if (delivered != 1 || limited.token_ids.size() != 1)
            throw std::runtime_error("Streaming callback stop was ignored");
        auto cancel_options = settings;
        bool cancelled = true;
        cancel_options.cancelled = [&] { return cancelled; };
        const auto before = model.generate(en, eos, cancel_options);
        if (!before.cancelled || !before.token_ids.empty())
            throw std::runtime_error("Cancellation before prefill was ignored");
        cancelled = false;
        delivered = 0;
        const auto during = model.generate(en, [](int) { return false; }, cancel_options,
            [&](int) { ++delivered; cancelled = true; return true; });
        if (!during.cancelled || delivered != 1)
            throw std::runtime_error("Cancellation during streaming was ignored");
        if (model.generate(en, eos, settings).token_ids != first)
            throw std::runtime_error("Cancellation leaked KV state into next request");
        std::cout << "Model regression checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
