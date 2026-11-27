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
    time::{Duration, Instant},
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
    Paused,
}

#[derive(Default, Clone)]
pub struct View {
    pub revision: u64,
    pub image_id: u64,
    pub png: Arc<[u8]>,
    pub page_size: (f64, f64),
    pub page: u32, // one-based; zero before a document has opened
    pub count: u32,
    pub phase: Phase,
    pub source: Arc<str>,
    pub translation: Arc<str>,
    pub message: Arc<str>,
}

#[derive(Default)]
enum Search {
    #[default]
    Idle,
    Pending(u64),
    Notice(Arc<str>),
}

struct State {
    view: View,
    search: Search,
    automatic: bool,
    easy: bool,
    model: Option<PathBuf>,
}
impl Default for State {
    fn default() -> Self {
        Self {
            view: View::default(),
            search: Search::Idle,
            automatic: true,
            easy: false,
            model: None,
        }
    }
}
impl State {
    fn translation_task(&mut self, id: u64) -> Option<ToTranslate> {
        self.view.translation = Arc::default();
        self.view.phase = if self.view.source.is_empty() {
            Phase::Empty
        } else if self.automatic {
            Phase::Translating
        } else {
            Phase::Paused
        };
        (self.view.phase == Phase::Translating).then(|| ToTranslate {
            id,
            source: self.view.source.clone(),
            easy: self.easy,
            model: self.model.clone(),
        })
    }
}

fn with_notice(notice: &str, message: &str) -> Arc<str> {
    if message.is_empty() {
        notice.into()
    } else {
        format!("{notice} {message}").into()
    }
}

#[derive(Default)]
struct Shared {
    generation: AtomicU64,
    state: Mutex<State>,
}
impl Shared {
    fn active(&self, id: u64) -> bool {
        self.generation.load(Ordering::Acquire) == id
    }
    fn update(&self, id: u64, change: impl FnOnce(&mut View)) {
        let mut state = self.state.lock().unwrap();
        if self.active(id) {
            change(&mut state.view);
            state.view.revision += 1;
        }
    }
    // Page work stays current during a search until a different page is selected.
    fn load_page(&self, state: &mut State, id: u64, page: u32, count: u32) {
        self.generation.store(id, Ordering::Release);
        state.search = Search::Idle;
        state.view = View {
            revision: state.view.revision + 1,
            page,
            count,
            phase: Phase::Loading,
            ..View::default()
        };
    }
    fn search_page(&self, id: u64) -> Option<u32> {
        let state = self.state.lock().unwrap();
        matches!(state.search, Search::Pending(current) if current == id).then_some(state.view.page)
    }
    fn finish_search(&self, id: u64, found: Option<u32>) -> Option<u32> {
        let mut state = self.state.lock().unwrap();
        if !matches!(state.search, Search::Pending(current) if current == id) {
            return None;
        }
        if let Some(page) = found
            && page != state.view.page
        {
            let count = state.view.count;
            self.load_page(&mut state, id, page, count);
            return Some(page);
        }
        state.search = if found.is_none() {
            Search::Notice("一致する本文がありません。".into())
        } else {
            Search::Idle
        };
        state.view.revision += 1;
        None
    }
    fn fail(&self, id: u64, reason: String) {
        let mut state = self.state.lock().unwrap();
        if self.active(id) {
            if state.view.phase == Phase::Translating {
                state.view.translation = Arc::default();
            }
            state.view.phase = Phase::Error;
            state.view.message = with_notice(&reason, &state.view.message);
        } else if matches!(state.search, Search::Pending(current) if current == id) {
            state.search = Search::Notice(reason.into());
        } else {
            return;
        }
        state.view.revision += 1;
    }
}

enum Action {
    Open(PathBuf, String),
    Close,
    Page(u32),
    Find {
        query: String,
        backwards: bool,
        match_case: bool,
    },
    Translate(ToTranslate),
}
struct Request {
    id: u64,
    action: Action,
}
struct ToTranslate {
    id: u64,
    source: Arc<str>,
    easy: bool,
    model: Option<PathBuf>,
}

pub struct Reader {
    shared: Arc<Shared>,
    tx: Option<Sender<Request>>,
    workers: Vec<JoinHandle<()>>,
}

impl Reader {
    pub fn new(model: PathBuf) -> std::io::Result<Self> {
        let shared = Arc::new(Shared::default());
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
        let mut state = self.shared.state.lock().unwrap();
        let id = state.view.revision + 1;
        let count = state.view.count;
        match &action {
            Action::Open(..) => self.shared.load_page(&mut state, id, 0, 0),
            Action::Close => {
                self.shared.load_page(&mut state, id, 0, 0);
                state.view.phase = Phase::Idle;
            }
            Action::Page(page) if *page > 0 && *page <= count && *page != state.view.page => {
                self.shared.load_page(&mut state, id, *page, count);
            }
            Action::Find { .. } if state.view.page > 0 => {
                state.search = Search::Pending(id);
                state.view.revision = id;
            }
            _ => return,
        }
        drop(state);
        self.send(id, action);
    }
    fn send(&self, id: u64, action: Action) {
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
    pub fn close(&self) {
        self.submit(Action::Close);
    }
    pub fn page(&self, page: u32) {
        self.submit(Action::Page(page));
    }
    pub fn find(&self, query: String) {
        self.find_matching(query, false, false);
    }
    pub fn find_matching(&self, query: String, backwards: bool, match_case: bool) {
        if !query.trim().is_empty() {
            let query = if match_case {
                query
            } else {
                query.to_lowercase()
            };
            self.submit(Action::Find {
                query,
                backwards,
                match_case,
            });
        }
    }
    pub fn cancel_find(&self) {
        let mut state = self.shared.state.lock().unwrap();
        if !matches!(state.search, Search::Idle) {
            state.search = Search::Idle;
            state.view.revision += 1;
        }
    }
    pub fn translation_options(&self, automatic: bool, easy: bool) {
        let mut state = self.shared.state.lock().unwrap();
        if state.automatic == automatic && state.easy == easy {
            return;
        }
        let same_style = state.easy == easy;
        state.automatic = automatic;
        state.easy = easy;
        if same_style && state.view.phase == Phase::Ready {
            return;
        }
        self.refresh_translation(state);
    }
    pub fn select_model(&self, path: PathBuf) {
        let mut state = self.shared.state.lock().unwrap();
        if state.model.as_ref() == Some(&path) && state.view.phase != Phase::Error {
            return;
        }
        state.model = Some(path);
        if state.view.phase != Phase::Ready {
            self.refresh_translation(state);
        }
    }
    fn refresh_translation(&self, mut state: std::sync::MutexGuard<'_, State>) {
        if state.view.source.is_empty()
            || !matches!(
                state.view.phase,
                Phase::Translating | Phase::Ready | Phase::Paused | Phase::Error
            )
        {
            return;
        }
        let id = state.view.revision + 1;
        self.shared.generation.store(id, Ordering::Release);
        state.view.revision = id;
        if state.view.phase == Phase::Error {
            state.view.message = Arc::default();
        }
        let task = state.translation_task(id);
        drop(state);
        if let Some(task) = task {
            self.send(id, Action::Translate(task));
        }
    }
    pub fn view(&self, after: u64) -> Option<View> {
        let state = self.shared.state.lock().unwrap();
        (state.view.revision != after).then(|| {
            let mut view = state.view.clone();
            match &state.search {
                Search::Pending(_) => {
                    view.phase = Phase::Loading;
                    view.message = "検索中…".into();
                }
                Search::Notice(note) => view.message = with_notice(note, &view.message),
                Search::Idle => {}
            }
            view
        })
    }
}
impl Drop for Reader {
    fn drop(&mut self) {
        {
            let mut state = self.shared.state.lock().unwrap();
            state.search = Search::Idle;
            self.shared.generation.store(0, Ordering::Release);
        }
        self.tx.take();
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
    }
}

struct CachedPage {
    page: u32,
    png: Arc<[u8]>,
    size: (f64, f64),
    source: Arc<str>,
    warning: Arc<str>,
}
impl CachedPage {
    fn bytes(&self) -> usize {
        self.png.len() + self.source.len() + self.warning.len()
    }
}

fn readable(text: &str) -> bool {
    text.chars().any(|c| !c.is_whitespace() && c != '�')
}

fn read_page(shared: &Shared, id: u64, doc: &Document, page: u32) -> Result<CachedPage, String> {
    let index = (page - 1) as usize;
    let image = doc.image(index)?;
    if !shared.active(id) {
        return Err("cancelled".into());
    }
    shared.update(id, |v| {
        v.png = image.png.clone();
        v.image_id = id;
        v.page_size = image.size;
    });
    let mut source: Arc<str> = doc.text(index)?.into();
    let mut warning = Arc::default();
    if source.trim().is_empty() || source.contains('�') {
        if !shared.active(id) {
            return Err("cancelled".into());
        }
        if !readable(&source) {
            source = Arc::default();
        }
        shared.update(id, |v| {
            v.phase = Phase::ReadingImage;
            if source.len() <= SOURCE_LIMIT {
                v.source = source.clone();
            }
        });
        match ocr::recognize(&image) {
            Ok(text) if readable(&text) => source = text.into(),
            Ok(_) => {}
            Err(e) if source.is_empty() => return Err(e),
            Err(e) => warning = format!("{e} PDFの埋め込み本文を使用します。").into(),
        }
    }
    if source.len() > SOURCE_LIMIT {
        return Err("本文が長すぎるため、このページの翻訳を省略しました。".into());
    }
    Ok(CachedPage {
        page,
        png: image.png,
        size: image.size,
        source,
        warning,
    })
}

fn pdf_loop(shared: Arc<Shared>, rx: Receiver<Request>, tasks: Sender<ToTranslate>) {
    let mut document = None;
    // Nearby pages keep their raster and effective source, including OCR.
    // Only complete reads enter this document-local cache.
    let mut recent: VecDeque<CachedPage> = VecDeque::new();
    for Request { id, action } in rx {
        if !matches!(action, Action::Find { .. }) && !shared.active(id) {
            continue;
        }
        let result =
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<(), String> {
                let page = match action {
                    Action::Open(path, password) => {
                        document = None;
                        recent.clear();
                        match Document::open(&path, &password) {
                            Ok(doc) => {
                                let count = u32::try_from(doc.count())
                                    .map_err(|_| "ページ数が上限を超えています。")?;
                                document = Some(doc);
                                shared.update(id, |v| {
                                    v.page = 1;
                                    v.count = count;
                                });
                                1
                            }
                            Err(OpenError::Password) => {
                                shared.update(id, |v| v.phase = Phase::Password);
                                return Ok(());
                            }
                            Err(OpenError::Message(e)) => return Err(e),
                        }
                    }
                    Action::Close => {
                        document = None;
                        recent.clear();
                        return Ok(());
                    }
                    Action::Translate(task) => {
                        tasks.send(task).map_err(|_| "翻訳処理が停止しました。")?;
                        return Ok(());
                    }
                    Action::Page(page) => page,
                    Action::Find {
                        query,
                        backwards,
                        match_case,
                    } => {
                        let Some(start) = shared.search_page(id) else {
                            return Ok(());
                        };
                        let doc = document.as_ref().ok_or("PDFが開かれていません。")?;
                        let mut found = None;
                        for offset in 0..doc.count() {
                            if shared.search_page(id).is_none() {
                                return Ok(());
                            }
                            let next = if backwards {
                                doc.count() + start as usize - 2 - offset
                            } else {
                                start as usize + offset
                            };
                            let index = next % doc.count();
                            if doc.text(index).is_ok_and(|text| {
                                if match_case {
                                    text.contains(&query)
                                } else {
                                    text.to_lowercase().contains(&query)
                                }
                            }) {
                                found = Some(index as u32 + 1);
                                break;
                            }
                        }
                        let Some(page) = shared.finish_search(id, found) else {
                            return Ok(());
                        };
                        page
                    }
                };
                let doc = document.as_ref().ok_or("PDFが開かれていません。")?;
                if !shared.active(id) {
                    return Ok(());
                }
                let cached = match recent
                    .iter()
                    .position(|entry| entry.page == page)
                    .and_then(|i| recent.remove(i))
                {
                    Some(hit) => hit,
                    None => read_page(&shared, id, doc, page)?,
                };
                let task = {
                    let mut state = shared.state.lock().unwrap();
                    if !shared.active(id) {
                        return Ok(());
                    }
                    state.view.png = cached.png.clone();
                    state.view.image_id = id;
                    state.view.page_size = cached.size;
                    state.view.source = cached.source.clone();
                    state.view.message = cached.warning.clone();
                    state.view.revision += 1;
                    state.translation_task(id)
                };
                const PAGE_CACHE_LIMIT: usize = 24 * 1024 * 1024;
                if cached.bytes() <= PAGE_CACHE_LIMIT {
                    recent.push_front(cached);
                    while recent.len() > 8
                        || recent.iter().map(CachedPage::bytes).sum::<usize>() > PAGE_CACHE_LIMIT
                    {
                        recent.pop_back();
                    }
                }
                if let Some(task) = task {
                    tasks.send(task).map_err(|_| "翻訳処理が停止しました。")?;
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
    let mut loaded_path = None;
    let mut cache = Cache::new();
    for ToTranslate {
        id,
        source,
        easy,
        model: selected,
    } in rx
    {
        if !shared.active(id) {
            continue;
        }
        // All accepted companion files have the same checksum. Translation
        // style and extracted source determine whether a result can be reused.
        let mut hash = Sha256::new();
        hash.update([u8::from(easy)]);
        hash.update(source.as_bytes());
        let key = hash.finalize().into();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(
            || -> Result<Arc<str>, String> {
                if let Some(hit) = cache.get(&key) {
                    return Ok(hit);
                }
                let selected = selected.as_ref().unwrap_or(&path);
                if model.is_none() || loaded_path.as_ref() != Some(selected) {
                    model = Some(Translator::open(selected)?);
                    loaded_path = Some(selected.clone());
                }
                if !shared.active(id) {
                    return Err("cancelled".into());
                }
                let mut partial = String::new();
                let mut has_text = false;
                let mut last_publish: Option<Instant> = None;
                let text = model.as_mut().unwrap().translate(
                    &source,
                    easy,
                    || !shared.active(id),
                    |chunk| {
                        partial.push_str(chunk);
                        has_text |= !chunk.trim().is_empty();
                        let now = Instant::now();
                        if has_text
                            && last_publish.is_none_or(|last| {
                                now.duration_since(last) >= Duration::from_millis(75)
                            })
                        {
                            last_publish = Some(now);
                            shared.update(id, |v| v.translation = partial.as_str().into());
                        }
                    },
                )?;
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
