// Compile the reader's actual request, cache and publication code against
// deterministic PDF, OCR and model inputs. The C ABI replay uses the native
// providers; here we can force failures and a raster above the cache budget.
mod session {
    include!("../src/session.rs");

    // Replay the publication boundary of reader.qnt's lateTranslationTest without
    // relying on the relative speeds of PDF rendering, inference, and the UI.
    #[test]
    fn delayed_reply_after_revisiting_same_page() {
        let (tx, requests) = mpsc::channel();
        let reader = Reader {
            shared: Arc::new(Shared {
                generation: AtomicU64::new(0),
                state: Mutex::new(State {
                    view: View {
                        page: 1,
                        count: 2,
                        ..View::default()
                    },
                    ..State::default()
                }),
            }),
            tx: Some(tx),
            workers: Vec::new(),
        };
        reader.page(2);
        let old = requests.try_recv().unwrap().id;
        reader.page(1);
        reader.page(2);
        let revision = reader.view(u64::MAX).unwrap().revision;
        reader.shared.update(old, |v| {
            v.translation = "stale".into();
            v.phase = Phase::Ready;
        });
        reader.shared.fail(old, "stale error".into());
        assert!(reader.view(revision).is_none());
    }

    #[test]
    fn search_miss_during_translation_accepts_the_original_reply() {
        // Keep the PDF worker running and hold its translation request until
        // after Find. A miss must neither cancel nor resubmit that request.
        let (tx, requests) = mpsc::channel();
        let (tasks, translations) = mpsc::channel();
        let shared = Arc::new(Shared::default());
        let pdf = shared.clone();
        let reader = Reader {
            shared,
            tx: Some(tx),
            workers: vec![thread::spawn(move || pdf_loop(pdf, requests, tasks))],
        };
        reader.open("search".into(), String::new());
        let task = translations
            .recv_timeout(super::Duration::from_secs(10))
            .unwrap();
        let before = reader.view(u64::MAX).unwrap();
        assert_eq!(before.phase, Phase::Translating);
        reader.find("absent query".into());
        reader.shared.update(task.id, |v| {
            v.phase = Phase::Ready;
            v.translation = "completed".into();
        });
        let after = super::settled(&reader);
        assert_eq!(after.phase, Phase::Ready);
        assert_eq!(after.image_id, before.image_id);
        assert_eq!(after.source, task.source);
        assert_eq!(&*after.translation, "completed");
        assert!(after.message.contains("一致する本文がありません"));
        assert!(translations.try_recv().is_err());
    }

    #[test]
    fn translation_cache_evicts_least_recently_used_at_real_entry_limit() {
        let mut cache = Cache::new();
        let key = |i: u32| -> [u8; 32] { Sha256::digest(i.to_le_bytes()).into() };
        for i in 0..4096 {
            cache.insert(key(i), "cached".into());
        }
        assert_eq!(cache.get(&key(0)).as_deref(), Some("cached"));
        cache.insert(key(4096), "new".into());
        assert!(cache.get(&key(1)).is_none());
        assert_eq!(cache.get(&key(0)).as_deref(), Some("cached"));
        assert_eq!(cache.get(&key(4096)).as_deref(), Some("new"));
        assert_eq!(cache.values.len(), 4096);
    }
}

mod document {
    use std::{
        path::Path,
        sync::Arc,
        sync::atomic::{AtomicUsize, Ordering},
    };

    pub const SOURCE_LIMIT: usize = 256 * 1024;
    pub static SMALL_PAGE_RENDERS: AtomicUsize = AtomicUsize::new(0);
    pub struct Document(String);
    pub struct Image {
        pub png: Arc<[u8]>,
    }
    pub enum OpenError {
        Password,
        Message(String),
    }

    impl Document {
        pub fn open(path: &Path, _: &str) -> Result<Self, OpenError> {
            match path.to_str() {
                Some(
                    name @ ("damaged" | "blank" | "replacement-marks" | "oversize" | "search"),
                ) => Ok(Self(name.to_owned())),
                Some("locked") => Err(OpenError::Password),
                _ => Err(OpenError::Message("unknown test document".into())),
            }
        }
        pub fn count(&self) -> usize {
            if matches!(self.0.as_str(), "damaged" | "oversize") {
                2
            } else {
                1
            }
        }
        pub fn image(&self, page: usize) -> Result<Image, String> {
            let size = match (self.0.as_str(), page) {
                ("oversize", 0) => {
                    SMALL_PAGE_RENDERS.fetch_add(1, Ordering::SeqCst);
                    1
                }
                ("oversize", 1) => 24 * 1024 * 1024 + 1,
                _ => 1,
            };
            Ok(Image {
                png: vec![0; size].into(),
            })
        }
        pub fn text(&self, page: usize) -> Result<String, String> {
            Ok(match self.0.as_str() {
                "damaged" if page == 0 => "Readable embedded text �".to_owned(),
                "blank" => String::new(),
                "replacement-marks" => " �\n� ".to_owned(),
                _ => format!("PDF page {page}"),
            })
        }
    }
}

mod ocr {
    pub fn recognize(_: &super::document::Image) -> Result<String, String> {
        Err("test OCR failure".into())
    }
}

mod engine {
    use std::path::Path;
    pub struct Translator;
    impl Translator {
        pub fn open(_: &Path) -> Result<Self, String> {
            Ok(Self)
        }
        pub fn translate(&mut self, source: &str, _: impl Fn() -> bool) -> Result<String, String> {
            Ok(format!("訳({source})"))
        }
    }
}

use session::{Phase, Reader, View};
use std::time::{Duration, Instant};

fn settled(reader: &Reader) -> View {
    let start = Instant::now();
    loop {
        if let Some(v) = reader.view(u64::MAX)
            && matches!(v.phase, Phase::Ready | Phase::Error | Phase::Empty)
        {
            return v;
        }
        assert!(
            start.elapsed() < Duration::from_secs(10),
            "reader did not settle"
        );
        std::thread::sleep(Duration::from_millis(2));
    }
}

#[test]
fn ocr_failure_preserves_readable_extraction() {
    let reader = Reader::new("model".into()).unwrap();
    reader.open("damaged".into(), String::new());
    let v = settled(&reader);
    assert_eq!(&*v.source, "Readable embedded text �");
    assert_eq!(v.phase, Phase::Ready);
    assert_eq!(&*v.translation, "訳(Readable embedded text �)");
    assert!(
        !v.message.is_empty(),
        "OCR failure should remain distinguishable"
    );
    reader.find("absent query".into());
    let miss = settled(&reader);
    assert!(miss.message.contains(v.message.as_ref()));
    assert!(miss.message.contains("一致する本文がありません"));
    reader.page(2);
    assert!(settled(&reader).message.is_empty());
    reader.page(1);
    let revisited = settled(&reader);
    assert_eq!(revisited.source, v.source);
    assert_eq!(revisited.message, v.message);
}

#[test]
fn ocr_failure_on_blank_extraction_reports_an_error() {
    let reader = Reader::new("model".into()).unwrap();
    for name in ["blank", "replacement-marks"] {
        reader.open(name.into(), String::new());
        let v = settled(&reader);
        assert_eq!(v.phase, Phase::Error);
        assert!(v.source.is_empty() && v.translation.is_empty() && !v.message.is_empty());
    }
}

#[test]
fn oversize_page_preserves_resident_page() {
    use document::SMALL_PAGE_RENDERS;
    use std::sync::atomic::Ordering;
    let reader = Reader::new("model".into()).unwrap();
    reader.open("oversize".into(), String::new());
    assert_eq!(&*settled(&reader).translation, "訳(PDF page 0)");
    let renders = SMALL_PAGE_RENDERS.load(Ordering::SeqCst);
    reader.page(2);
    assert_eq!(&*settled(&reader).translation, "訳(PDF page 1)");
    reader.page(1);
    assert_eq!(&*settled(&reader).translation, "訳(PDF page 0)");
    assert_eq!(
        SMALL_PAGE_RENDERS.load(Ordering::SeqCst),
        renders,
        "resident page rendered again"
    );
}

#[test]
fn unsuccessful_search_preserves_a_completed_translation_and_image() {
    let reader = Reader::new("model".into()).unwrap();
    reader.open("search".into(), String::new());
    let before = settled(&reader);
    assert_eq!(before.phase, Phase::Ready);
    reader.find("absent query".into());
    let after = settled(&reader);
    assert_eq!(after.page, before.page);
    assert_eq!(after.source, before.source);
    assert_eq!(after.translation, before.translation);
    assert_eq!(
        after.image_id, before.image_id,
        "search miss would reset the viewport"
    );
}
