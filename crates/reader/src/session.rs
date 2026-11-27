use crate::{
    document::{Document, OpenError, SOURCE_LIMIT},
    engine::Translator,
    ocr,
};
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, VecDeque},
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
        mpsc::{self, Receiver, Sender},
    },
    thread::{self, JoinHandle},
};

#[repr(u32)]
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub enum Phase {
    #[default]
    Idle,
    Loading,
    ReadingImage,
    Translating,
    Ready,
    Empty,
    Error,
    Password,
}

#[derive(Default, Clone)]
pub struct View {
    pub revision: u64,
    pub image_id: u64,
    pub png: Arc<[u8]>,
    pub page: u32, // one-based; zero before a document has opened
    pub count: u32,
    pub phase: Phase,
    pub source: Arc<str>,
    pub translation: Arc<str>,
    pub message: Arc<str>,
}

struct Shared {
    generation: AtomicU64,
    view: Mutex<View>,
}
impl Shared {
    fn active(&self, id: u64) -> bool {
        self.generation.load(Ordering::Acquire) == id
    }
    fn update(&self, id: u64, change: impl FnOnce(&mut View)) {
        let mut v = self.view.lock().unwrap();
        if self.active(id) {
            change(&mut v);
            v.revision += 1;
        }
    }
    fn fail(&self, id: u64, reason: String) {
        self.update(id, |v| {
            v.phase = Phase::Error;
            v.message = reason.into();
        });
    }
}

enum Action {
    Open(PathBuf, String),
    Page(u32),
    Find(String, u32),
}
struct Request {
    id: u64,
    action: Action,
}
struct ToTranslate {
    id: u64,
    source: Arc<str>,
}

pub struct Reader {
    shared: Arc<Shared>,
    tx: Option<Sender<Request>>,
    workers: Vec<JoinHandle<()>>,
}

impl Reader {
    pub fn new(model: PathBuf) -> std::io::Result<Self> {
        let shared = Arc::new(Shared {
            generation: AtomicU64::new(0),
            view: Mutex::new(View::default()),
        });
        let (tx, rx) = mpsc::channel();
        let (infer, inputs) = mpsc::channel();
        let s = shared.clone();
        let translation = thread::Builder::new()
            .name("translation".into())
            .spawn(move || translate_loop(s, inputs, model))?;
        let s = shared.clone();
        let pdf = thread::Builder::new()
            .name("pdf".into())
            .spawn(move || pdf_loop(s, rx, infer))?;
        Ok(Self {
            shared,
            tx: Some(tx),
            workers: vec![pdf, translation],
        })
    }
    fn submit(&self, action: Action) {
        let mut view = self.shared.view.lock().unwrap();
        if let Action::Page(page) = action
            && (page == 0 || page > view.count || page == view.page)
        {
            return;
        }
        let id = self.shared.generation.fetch_add(1, Ordering::AcqRel) + 1;
        let revision = view.revision + 1;
        match &action {
            Action::Open(..) => {
                *view = View {
                    revision,
                    phase: Phase::Loading,
                    ..View::default()
                }
            }
            Action::Page(page) => {
                *view = View {
                    revision,
                    phase: Phase::Loading,
                    page: *page,
                    count: view.count,
                    ..View::default()
                }
            }
            Action::Find(..) => {
                view.phase = Phase::Loading;
                view.message = "検索中…".into();
                view.revision = revision;
            }
        }
        drop(view);
        if self
            .tx
            .as_ref()
            .unwrap()
            .send(Request { id, action })
            .is_err()
        {
            self.shared.fail(id, "読み込み処理が停止しました。".into());
        }
    }
    pub fn open(&self, path: PathBuf, password: String) {
        self.submit(Action::Open(path, password));
    }
    pub fn page(&self, page: u32) {
        self.submit(Action::Page(page));
    }
    pub fn find(&self, query: String) {
        let page = self.shared.view.lock().unwrap().page;
        if page > 0 && !query.trim().is_empty() {
            self.submit(Action::Find(query.to_lowercase(), page));
        }
    }
    pub fn view(&self, after: u64) -> Option<View> {
        let v = self.shared.view.lock().unwrap();
        (v.revision != after).then(|| v.clone())
    }
}
impl Drop for Reader {
    fn drop(&mut self) {
        self.shared.generation.fetch_add(1, Ordering::AcqRel);
        self.tx.take();
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
    }
}

fn pdf_loop(shared: Arc<Shared>, rx: Receiver<Request>, tasks: Sender<ToTranslate>) {
    let mut document = None;
    // Nearby pages keep their raster and effective source, including OCR.
    // Only complete reads enter this document-local cache.
    let mut recent: VecDeque<(u32, Arc<[u8]>, Arc<str>)> = VecDeque::new();
    for Request { id, action } in rx {
        if !shared.active(id) {
            continue;
        }
        let result =
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<(), String> {
                let mut note = String::new();
                let page = match action {
                    Action::Open(path, password) => {
                        document = None;
                        recent.clear();
                        match Document::open(&path, &password) {
                            Ok(doc) => {
                                document = Some(doc);
                                1
                            }
                            Err(OpenError::Password) => {
                                shared.update(id, |v| v.phase = Phase::Password);
                                return Ok(());
                            }
                            Err(OpenError::Message(e)) => return Err(e),
                        }
                    }
                    Action::Page(page) => page,
                    Action::Find(query, start) => {
                        let doc = document.as_ref().ok_or("PDFが開かれていません。")?;
                        let mut found = None;
                        for offset in 0..doc.count() {
                            if !shared.active(id) {
                                return Ok(());
                            }
                            let index = (start as usize + offset) % doc.count();
                            if doc
                                .text(index)
                                .is_ok_and(|text| text.to_lowercase().contains(&query))
                            {
                                found = Some(index as u32 + 1);
                                break;
                            }
                        }
                        if found.is_none() {
                            note = "一致する本文がありません。".into();
                        }
                        found.unwrap_or(start)
                    }
                };
                let doc = document.as_ref().ok_or("PDFが開かれていません。")?;
                if !shared.active(id) {
                    return Ok(());
                }
                let count =
                    u32::try_from(doc.count()).map_err(|_| "ページ数が上限を超えています。")?;
                shared.update(id, |v| {
                    *v = View {
                        revision: v.revision,
                        page,
                        count,
                        phase: Phase::Loading,
                        message: note.into(),
                        ..View::default()
                    };
                });
                let cached = recent
                    .iter()
                    .position(|(p, _, _)| *p == page)
                    .and_then(|i| recent.remove(i));
                let (png, source) = if let Some((_, png, text)) = cached {
                    shared.update(id, |v| {
                        v.png = png.clone();
                        v.image_id = id;
                    });
                    (png, text)
                } else {
                    let index = (page - 1) as usize;
                    let image = doc.image(index)?;
                    if !shared.active(id) {
                        return Ok(());
                    }
                    shared.update(id, |v| {
                        v.png = image.png.clone();
                        v.image_id = id;
                    });
                    let mut text = doc.text(index)?;
                    if !shared.active(id) {
                        return Ok(());
                    }
                    if text.trim().is_empty() || text.contains('�') {
                        shared.update(id, |v| v.phase = Phase::ReadingImage);
                        let recognized = ocr::recognize(&image)?;
                        if !recognized.trim().is_empty() {
                            text = recognized;
                        }
                    }
                    if text.len() > SOURCE_LIMIT {
                        return Err("本文が長すぎるため、このページの翻訳を省略しました。".into());
                    }
                    (image.png, Arc::<str>::from(text))
                };
                recent.push_front((page, png, source.clone()));
                while recent.len() > 8
                    || recent
                        .iter()
                        .map(|(_, p, s)| p.len() + s.len())
                        .sum::<usize>()
                        > 24 * 1024 * 1024
                {
                    recent.pop_back();
                }
                if source.trim().is_empty() {
                    shared.update(id, |v| v.phase = Phase::Empty);
                    return Ok(());
                }
                shared.update(id, |v| {
                    v.source = source.clone();
                    v.phase = Phase::Translating;
                });
                if shared.active(id) {
                    tasks
                        .send(ToTranslate { id, source })
                        .map_err(|_| "翻訳処理が停止しました。")?;
                }
                Ok(())
            }));
        match result {
            Ok(Err(e)) => shared.fail(id, e),
            Err(_) => shared.fail(id, "このページを処理できません。".into()),
            _ => {}
        }
    }
}

struct Cache {
    values: HashMap<[u8; 32], Arc<str>>,
    order: VecDeque<[u8; 32]>,
    bytes: usize,
}
impl Cache {
    fn new() -> Self {
        Self {
            values: HashMap::new(),
            order: VecDeque::new(),
            bytes: 0,
        }
    }
    fn get(&mut self, key: &[u8; 32]) -> Option<Arc<str>> {
        let value = self.values.get(key)?.clone();
        self.order.retain(|k| k != key);
        self.order.push_back(*key);
        Some(value)
    }
    fn insert(&mut self, key: [u8; 32], value: Arc<str>) {
        const LIMIT: usize = 256 * 1024 * 1024;
        let size = value.len() + 128;
        if size > LIMIT {
            return;
        }
        while self.bytes + size > LIMIT || self.values.len() >= 4096 {
            let oldest = self.order.pop_front().unwrap();
            self.bytes -= self.values.remove(&oldest).unwrap().len() + 128;
        }
        self.bytes += size;
        self.order.push_back(key);
        self.values.insert(key, value);
    }
}
fn translate_loop(shared: Arc<Shared>, rx: Receiver<ToTranslate>, path: PathBuf) {
    let mut model = None;
    let mut cache = Cache::new();
    for ToTranslate { id, source } in rx {
        if !shared.active(id) {
            continue;
        }
        // A reader has one immutable model and translation style. Identical
        // extracted input from reopened documents can reuse the same result.
        let key = Sha256::digest(source.as_bytes()).into();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(
            || -> Result<Arc<str>, String> {
                if let Some(hit) = cache.get(&key) {
                    return Ok(hit);
                }
                if model.is_none() {
                    model = Some(Translator::open(&path)?);
                }
                if !shared.active(id) {
                    return Err("cancelled".into());
                }
                let text = model
                    .as_mut()
                    .unwrap()
                    .translate(&source, || !shared.active(id))?;
                if text.trim().is_empty() {
                    return Err("訳文を生成できませんでした。".into());
                }
                let value: Arc<str> = text.into();
                cache.insert(key, value.clone());
                Ok(value)
            },
        ));
        match result {
            Ok(Ok(text)) => shared.update(id, |v| {
                v.translation = text;
                v.phase = Phase::Ready;
            }),
            Ok(Err(e)) => shared.fail(id, e),
            Err(_) => {
                model = None;
                shared.fail(id, "翻訳処理を続行できません。".into());
            }
        }
    }
}
