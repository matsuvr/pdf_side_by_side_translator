use super::{
    document::{Document, OpenError},
    session::{Phase, Reader, View},
};
use std::{
    path::PathBuf,
    thread,
    time::{Duration, Instant},
};

fn fixtures() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}
fn open(name: &str) -> Document {
    Document::open(&fixtures().join(name), "").ok().unwrap()
}
#[test]
fn pdf_reading_order() {
    for name in ["pdf-ground-truth.json", "unicode-ground-truth.json"] {
        let manifest: serde_json::Value =
            serde_json::from_slice(&std::fs::read(fixtures().join(name)).unwrap()).unwrap();
        for (file, expected) in manifest.as_object().unwrap() {
            if !file.ends_with(".pdf") {
                continue;
            }
            let doc = open(file);
            if let Some(n) = expected["page_count"].as_u64() {
                assert_eq!(doc.count(), n as usize, "{file}");
            }
            for page in 0..doc.count() {
                let text = doc.text(page).unwrap();
                let markers = expected["ordered_markers"]
                    .as_array()
                    .or_else(|| expected["reading_order"][page].as_array());
                let mut rest = text.as_str();
                for marker in markers.into_iter().flatten() {
                    let m = marker.as_str().unwrap();
                    rest = rest
                        .split_once(m)
                        .unwrap_or_else(|| panic!("{file} page {page}, missing {m} in {text:?}"))
                        .1;
                }
                for m in expected["contains"].as_array().into_iter().flatten() {
                    assert!(text.contains(m.as_str().unwrap()), "{file} {text:?}");
                }
                for m in expected["excludes"].as_array().into_iter().flatten() {
                    assert!(!text.contains(m.as_str().unwrap()), "{file} {text:?}");
                }
                if expected["no_text_pages"]
                    .as_array()
                    .is_some_and(|pages| pages.iter().any(|x| x.as_u64() == Some(page as u64)))
                {
                    assert!(text.is_empty());
                }
                if expected["same_text_as_first_page"] == true {
                    assert_eq!(text, doc.text(0).unwrap(), "{file} page {page}");
                }
            }
        }
    }
}
#[test]
fn protected_pdf_and_rendering() {
    let path = fixtures().join("P11-password.pdf");
    assert!(matches!(
        Document::open(&path, "wrong"),
        Err(OpenError::Password)
    ));
    let doc = Document::open(&path, "reader-fixture").ok().unwrap();
    assert!(doc.text(0).unwrap().contains("11 samples"));
    let image = doc.image(0).unwrap();
    assert!(image.png.starts_with(b"\x89PNG") && image.width > 0 && image.height > 0);
    assert!(image.rgba.as_chunks::<4>().0.iter().any(|p| p[0] < 80));
    let japanese = open("P06-japanese-fallback.pdf").image(0).unwrap();
    assert!(
        japanese
            .rgba
            .as_chunks::<4>()
            .0
            .iter()
            .filter(|p| p[0] < 80)
            .count()
            > 500,
        "Japanese substitute glyphs were not painted"
    );
}

#[test]
#[ignore = "requires the native offline text recognizer"]
fn native_image_reading() {
    let page = open("P01-simple.pdf").image(0).unwrap();
    let text = crate::ocr::recognize(&page).unwrap();
    assert!(text.contains("3 sensors"), "{text}");
}

fn wait(reader: &Reader, done: impl Fn(&View) -> bool) -> View {
    let start = Instant::now();
    loop {
        if let Some(v) = reader.view(u64::MAX)
            && done(&v)
        {
            return v;
        }
        assert!(
            start.elapsed() < Duration::from_secs(120),
            "reader timed out"
        );
        thread::sleep(Duration::from_millis(20));
    }
}
#[test]
fn current_page_survives_failed_model_and_document_replacement() {
    let reader = Reader::new(fixtures().join("missing-model")).unwrap();
    reader.open(fixtures().join("P01-simple.pdf"), String::new());
    wait(&reader, |v| v.count == 3);
    reader.page(2);
    reader.page(1);
    reader.page(2);
    let state = wait(&reader, |v| v.phase == Phase::Error);
    assert!(
        state.page == 2
            && !state.png.is_empty()
            && state.source.contains("12 samples")
            && state.translation.is_empty()
    );
    reader.open(fixtures().join("P11-password.pdf"), String::new());
    let locked = wait(&reader, |v| v.phase == Phase::Password);
    assert!(locked.source.is_empty() && locked.png.is_empty() && locked.count == 0);
    reader.open(fixtures().join("P04-crop.pdf"), String::new());
    let fresh = wait(&reader, |v| v.phase == Phase::Error);
    assert!(fresh.source.contains("crop box") && fresh.translation.is_empty());
}
#[test]
#[ignore = "requires the bundled platform model in PDF_READER_TEST_MODEL"]
fn real_translation_and_navigation() {
    let model = std::env::var_os("PDF_READER_TEST_MODEL").expect("set PDF_READER_TEST_MODEL");
    let reader = Reader::new(model.into()).unwrap();
    reader.open(fixtures().join("P01-simple.pdf"), String::new());
    let finish = |v: &View| matches!(v.phase, Phase::Ready | Phase::Error);
    let first = wait(&reader, finish);
    assert_eq!(first.phase, Phase::Ready, "{}", first.message);
    assert!(
        first.translation.contains('3')
            && first
                .translation
                .chars()
                .any(|c| ('ぁ'..='ヿ').contains(&c)),
        "{}",
        first.translation
    );
    reader.page(2);
    let second = wait(&reader, finish);
    assert_eq!(second.phase, Phase::Ready, "{}", second.message);
    assert!(second.translation.contains("12"));
    reader.page(1);
    let again = wait(&reader, finish);
    assert!(
        std::sync::Arc::ptr_eq(&first.translation, &again.translation),
        "cache miss"
    );
}
