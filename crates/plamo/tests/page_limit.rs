use plamo3_translate_rs::{Engine, TranslationRequest};

#[test]
#[ignore = "requires the S4 model in OPT_P4X86_TEST_MODEL"]
fn page_translation_accepts_eos_at_the_context_boundary() {
    let path = std::env::var_os("OPT_P4X86_TEST_MODEL").expect("set OPT_P4X86_TEST_MODEL");
    let request = TranslationRequest {
        text: "The device has three sensors.",
        target: "ja",
        form: Some("plain"),
        ..Default::default()
    };
    let reference = {
        let engine = Engine::open(&path, 4, 256).unwrap();
        let output = engine.translate_with_options(&request, 128, false).unwrap();
        assert!(output.stopped && !output.tokens.is_empty());
        assert_eq!(
            engine.translate_page(request.text, || false).unwrap(),
            output.text
        );
        output
    };
    let exact = reference.prompt_tokens + reference.tokens.len();
    let translate = |capacity| {
        Engine::open(&path, 4, capacity)
            .unwrap()
            .translate_page(request.text, || false)
    };
    assert!(
        translate(exact - 1).is_err(),
        "accepted a truncated translation"
    );
    assert_eq!(
        translate(exact).unwrap_or_else(|err| panic!(
            "EOS after {} content tokens: {err}",
            reference.tokens.len()
        )),
        reference.text
    );
}
