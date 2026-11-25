// Synchronous C ABI. All handles are owned by the C++ inference worker.
// No callbacks may unwind, retain borrowed buffers, or re-enter the engine.
use super::*;
use std::{ffi::c_void, panic::{catch_unwind, AssertUnwindSafe}, slice};

type Read = extern "C" fn(*mut c_void, u64, *mut u8, usize) -> usize;
type Cancel = extern "C" fn(*mut c_void) -> i32;
type Accept = extern "C" fn(*mut c_void, *const u8, usize) -> i32;
type Delta = extern "C" fn(*mut c_void, *const u8, usize) -> i32;
const CONTEXT: usize = 32768;

enum Output {
    Stop,
    Piece(Vec<u8>),
}

fn accepted_output(tokenizer: &Tokenizer, id: u32, accept: Accept, user: *mut c_void) -> Option<Output> {
    if [1, 2, 16].contains(&id) {
        return (accept(user, std::ptr::null(), 0) != 0).then_some(Output::Stop);
    }
    let piece = tokenizer.output_piece(id)?;
    (accept(user, piece.as_ptr(), piece.len()) != 0).then_some(Output::Piece(piece))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn opt_plamo_open(size: usize, threads: usize, read: Read, user: *mut c_void) -> *mut Engine {
    catch_unwind(AssertUnwindSafe(|| {
        if size != model::FILE_BYTES { return std::ptr::null_mut(); }
        let mut bytes = Vec::new();
        if bytes.try_reserve_exact(size).is_err() { return std::ptr::null_mut(); }
        bytes.resize(size, 0);
        let mut offset = 0;
        while offset < size {
            let n = (size - offset).min(1024 * 1024);
            let got = read(user, offset as u64, bytes[offset..].as_mut_ptr(), n);
            if got == 0 || got > n { return std::ptr::null_mut(); }
            offset += got;
        }
        match model::from_bytes(bytes).and_then(|g| Engine::from_model(g, threads, CONTEXT, false)) {
            Ok(engine) => Box::into_raw(Box::new(engine)),
            Err(_) => std::ptr::null_mut(),
        }
    })).unwrap_or(std::ptr::null_mut())
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn opt_plamo_close(engine: *mut Engine) {
    if !engine.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(engine) })));
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn opt_plamo_max_piece(engine: *const Engine) -> usize {
    unsafe { engine.as_ref() }.map_or(0, |e| e.tokenizer.max_piece_bytes())
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn opt_plamo_tokenize(engine: *const Engine, text: *const u8, len: usize, special: i32, bos: i32, out: *mut i32, cap: usize) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(e) = (unsafe { engine.as_ref() }) else { return i32::MIN };
        if len > 8 * 1024 * 1024 || text.is_null() || out.is_null() { return i32::MIN; }
        let Ok(text) = std::str::from_utf8(unsafe { slice::from_raw_parts(text, len) }) else { return i32::MIN };
        let mut ids = e.tokenizer.encode_mode(text, special != 0);
        if bos != 0 { ids.insert(0, 1); }
        if ids.len() > cap { return -(ids.len() as i32); }
        for (i, id) in ids.iter().enumerate() { unsafe { out.add(i).write(*id as i32) }; }
        ids.len() as i32
    })).unwrap_or(i32::MIN)
}
// Return codes: 0 stop, 1 cancelled, 2 output limit, 3 context limit,
// 4 invalid output, 5 decode error. Delta callback may reject a piece.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn opt_plamo_generate(engine: *const Engine, tokens: *const i32, count: usize,
    limit: usize, cancel: Cancel, accept: Accept, delta: Delta, user: *mut c_void) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(e) = (unsafe { engine.as_ref() }) else { return 5 };
        if tokens.is_null() || count == 0 || count.checked_add(limit).is_none_or(|n| n > CONTEXT) { return 3; }
        if cancel(user) != 0 { return 1; }
        let ids: Vec<u32> = unsafe { slice::from_raw_parts(tokens, count) }.iter().map(|&t| t as u32).collect();
        if ids.iter().any(|&t| t as usize >= V) { return 5; }
        let mut session = e.session();
        // Reuse only the known constant PFML instructions, never user tokens.
        let fixed = e.tokenizer.encode("<|plamo:bos|><|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:msg|>");
        let key = (ids.len() > fixed.len() && ids.starts_with(&fixed)).then_some(&fixed);
        let reused = e.restore_prefix(&mut session, key);
        let mut next;
        for chunk in ids[reused..].chunks(32) {
            if cancel(user) != 0 { return 1; }
            if e.pool.install(|| session.forward_batch(chunk)).is_err() { return 5; }
        }
        if reused == 0 { e.remember_prefix(&session, key); }
        match e.pool.install(|| session.readout(true)) { Ok(t) => next = t, Err(_) => return 5 }
        for i in 0..limit {
            if cancel(user) != 0 { return 1; }
            let mut output = accepted_output(&e.tokenizer, next, accept, user);
            if output.is_none() {
                // Score the full vocabulary when the approximate winner is rejected.
                if e.pool.install(|| session.readout(false)).is_err() { return 5; }
                if cancel(user) != 0 { return 1; }
                let mut best = f32::NEG_INFINITY;
                for (id, &score) in session.logits().iter().enumerate() {
                    if score > best {
                        if let Some(candidate) = accepted_output(&e.tokenizer, id as u32, accept, user) {
                            best = score;
                            next = id as u32;
                            output = Some(candidate);
                        }
                    }
                }
            }
            let piece = match output {
                Some(Output::Stop) => return 0,
                Some(Output::Piece(piece)) => piece,
                None => return 4,
            };
            if delta(user, piece.as_ptr(), piece.len()) != 0 { return 4; }
            if cancel(user) != 0 { return 1; }
            if i + 1 < limit {
                match session.step_internal(next, true, true) { Ok(Some(t)) => next = t, _ => return 5 }
            }
        }
        2
    })).unwrap_or(5)
}
