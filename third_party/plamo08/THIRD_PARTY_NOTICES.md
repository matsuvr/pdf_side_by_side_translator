# Source and dependency notices

## Dedicated Metal decoder

The Metal kernels and decoder organization in `src/kernels.cpp` are adapted from
Preferred Networks' `pfnet/plamo-translate-cli`, specifically
`src/plamo_translate/servers/mlx/plamo3_metal.py` and `plamo3.py` at commit
`82788925ce64b5ec15abb25a650ccf4616eff57a`.

Upstream: <https://github.com/pfnet/plamo-translate-cli>

License: Apache License, Version 2.0; the full text is included as `LICENSE`.
Modifications in this project include the C++ host port, 0.8B dimensions,
16:1 multi-query attention, published-checkpoint BF16 rounding, stable online
softmax, pre-quantized weight loading, native tokenization, and validation tools.

## Model architecture and tokenizer behavior

The checkpoint configuration, tensor layout, numerical boundaries and tokenizer
behavior were checked against these files published by Preferred Networks:

- <https://huggingface.co/pfnet/plamo-3-translate-0.8b-MLX-4bit/blob/main/config.json>
- <https://huggingface.co/pfnet/plamo-3-translate-0.8b-MLX-4bit/blob/main/modeling_plamo_mlx.py>
- <https://huggingface.co/pfnet/plamo-3-translate-0.8b-MLX-4bit/blob/main/tokenization_plamo.py>
- <https://huggingface.co/pfnet/plamo-3-translate-0.8b-MLX-4bit/blob/main/generation_config.json>

The native tokenizer is a new C++ trie/Unigram implementation of the published
behavior. The official tokenizer vocabulary and model weights are obtained
separately from the checkpoint repository. Their accompanying model license
continues to govern those assets. This source archive contains neither weights
nor the tokenizer vocabulary.

## MLX

Copyright © 2023-2026 Apple Inc. MLX is licensed under the MIT License.
The upstream license text is included in `LICENSES/MLX-MIT.txt`.

The build selects MLX v0.32.4, commit
`36969c094354086e15d2a327a9c50feb20b9cf11`.

Source: <https://github.com/ml-explore/mlx>

## nlohmann/json

Copyright (c) 2013-2023 Niels Lohmann. Licensed under the MIT License.
The build fetches version 3.11.3, including its upstream license.

Source: <https://github.com/nlohmann/json/tree/v3.11.3>

## App integration changes

The viewer adapter adds cancellation checkpoints, bounded streaming output,
nonprinting byte-token suppression, and native folder selection. The decoder
and tokenizer are compiled into the application; no Python runtime is used.
Dependencies are prepared by `scripts/prepare_mlx.py` at the repository root.
The root `THIRD_PARTY_NOTICES.txt` includes the fmt and Apple metal-cpp licenses.
