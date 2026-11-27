use crate::{
    engine,
    session::{Reader, View},
};
use std::{
    ffi::{CStr, c_char},
    ptr,
};

#[repr(C)]
pub struct Bytes {
    data: *const u8,
    len: usize,
}
impl From<&[u8]> for Bytes {
    fn from(s: &[u8]) -> Self {
        Self {
            data: s.as_ptr(),
            len: s.len(),
        }
    }
}
#[repr(C)]
pub struct ReaderView {
    revision: u64,
    image_id: u64,
    page: u32,
    count: u32,
    phase: u32,
    source: Bytes,
    translation: Bytes,
    message: Bytes,
    png: Bytes,
}
unsafe fn utf8<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        unsafe { CStr::from_ptr(p) }.to_str().ok()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn reader_new() -> *mut Reader {
    engine::bundled_model()
        .ok()
        .and_then(|path| Reader::new(path).ok())
        .map_or(ptr::null_mut(), |s| Box::into_raw(Box::new(s)))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn reader_free(p: *mut Reader) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn reader_open(
    p: *const Reader,
    path: *const c_char,
    password: *const c_char,
) {
    if let (Some(r), Some(path)) = (unsafe { p.as_ref() }, unsafe { utf8(path) }) {
        r.open(path.into(), unsafe { utf8(password) }.unwrap_or("").into());
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn reader_page(p: *const Reader, page: u32) {
    if let Some(r) = unsafe { p.as_ref() } {
        r.page(page);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn reader_find(p: *const Reader, query: *const c_char) {
    if let (Some(r), Some(q)) = (unsafe { p.as_ref() }, unsafe { utf8(query) }) {
        r.find(q.into());
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn reader_poll(
    p: *const Reader,
    revision: u64,
    image: u64,
    out: *mut ReaderView,
) -> *mut View {
    let (Some(r), Some(out)) = (unsafe { p.as_ref() }, unsafe { out.as_mut() }) else {
        return ptr::null_mut();
    };
    let Some(view) = r.view(revision) else {
        return ptr::null_mut();
    };
    *out = ReaderView {
        revision: view.revision,
        image_id: view.image_id,
        page: view.page,
        count: view.count,
        phase: view.phase as u32,
        source: view.source.as_bytes().into(),
        translation: view.translation.as_bytes().into(),
        message: view.message.as_bytes().into(),
        png: if image == view.image_id {
            Bytes::from(&[][..])
        } else {
            view.png.as_ref().into()
        },
    };
    Box::into_raw(Box::new(view))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn reader_snapshot_free(p: *mut View) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}
