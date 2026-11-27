module decode {
  // Both backends reserve context for the prompt and cap generation by the
  // remaining capacity AND the output limit. Units below are model tokens.
  pure val capacity = 5
  pure val outputLimit = 3
  type State = { stage: str, prompt: int, used: int, budget: int, generated: int,
                 tail: bool, hasText: bool, verified: bool, eos: bool }
  pure val empty = { stage: "model", prompt: 0, used: 0, budget: 0, generated: 0,
                     tail: false, hasText: false, verified: false, eos: false }
  var s: State
  action init = s' = empty
  action reject = s' = { ...s, stage: "error" }
  action acceptModel = s' = { ...s, stage: "prompt", verified: true }
  action loadModel = all {
    s.stage == "model", any { acceptModel, reject }
  }
  action tokenize(n: int): bool = all {
    s.stage == "prompt",
    val room = capacity - n
    if (room <= 0) reject else s' = { ...s, stage: "prefill", prompt: n,
      budget: if (room < outputLimit) room else outputLimit },
  }
  action prefill = all {
    s.stage == "prefill",
    s' = { ...s, used: s.used + 1,
      stage: if (s.used + 1 == s.prompt) "generate" else "prefill" },
  }
  // Tokens represent valid text prefixes. A token may complete a previous
  // partial character, emit text and/or end in another partial character.
  action piece(endsPartial: bool, emitsText: bool): bool = all {
    s.stage == "generate",
    // EOS is observed in the last logits; only another content token costs a slot.
    if (s.generated == s.budget) reject
    else s' = { ...s, used: s.used + 1, generated: s.generated + 1,
                tail: endsPartial, hasText: s.hasText or emitsText },
  }
  action stopToken = all {
    s.stage == "generate", not(s.tail),
    s' = { ...s, eos: true, stage: if (s.hasText) "ready" else "error" },
  }
  action step = any {
    loadModel,
    nondet n = 1.to(capacity + 1).oneOf()
    tokenize(n),
    prefill,
    nondet partial = Bool.oneOf()
    nondet text = Bool.oneOf()
    piece(partial, text),
    stopToken,
    // Cancellation, model error, a rejected prompt or no valid candidate.
    all { Set("prompt", "prefill", "generate").contains(s.stage), reject },
    all { Set("ready", "error").contains(s.stage), s' = empty },
  }
  invariant verifiedBeforeUse =
    Set("model", "error").contains(s.stage) or s.verified
  invariant contextBound = s.used <= capacity and s.generated <= s.budget and
    s.prompt + s.budget <= capacity and s.budget <= outputLimit
  invariant prefilled = s.generated == 0 or s.used == s.prompt + s.generated
  invariant completeResult = s.stage == "ready" implies
    (s.eos and not(s.tail) and s.hasText and s.verified and s.generated <= s.budget)

  run splitCharacterTest = init.then(acceptModel)
    .then(tokenize(1)).then(prefill).then(piece(true, false)).expect(not(s.eos))
    .then(piece(false, true)).then(stopToken).expect(s.stage == "ready")
  run fullContext = init.then(acceptModel)
    .then(tokenize(3)).then(prefill).then(prefill).then(prefill)
    .then(piece(true, false)).then(piece(false, true))
    .expect(s.used == capacity and s.generated == s.budget and not(s.eos))
  run exactFitTest = fullContext.then(stopToken).expect(s.stage == "ready")
  run contextExhaustionTest = fullContext.then(piece(false, true))
    .expect(s.used == capacity and s.stage == "error" and not(s.eos))
  run outputLimitFitTest = init.then(acceptModel).then(tokenize(1)).then(prefill)
    .then(piece(false, true)).then(piece(false, true)).then(piece(false, true))
    .then(stopToken).expect(s.stage == "ready" and s.used < capacity and s.generated == outputLimit)
}
