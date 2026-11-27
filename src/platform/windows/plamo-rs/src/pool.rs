//! Low-latency spin-barrier thread pool with Windows hybrid P-core pinning.
//!
//! Mirrors the scheduling model that keeps decode barriers sub-microsecond and
//! avoids scheduling worker threads onto Intel E-cores during translation.
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

const SPIN_LIMIT: u32 = 40_000;

#[derive(Clone, Debug, Default)]
pub(crate) struct Topology {
    /// Logical CPU indices ordered: P-core physical -> P-core SMT -> E-cores.
    pub(crate) order: Vec<usize>,
    /// Number of logical CPUs belonging to the highest efficiency class (0 if homogeneous).
    pub(crate) n_performance_logical: usize,
    /// True when the system has multiple efficiency classes (hybrid P+E CPU).
    pub(crate) hybrid: bool,
}

#[cfg(target_os = "windows")]
mod win_affinity {
    use super::Topology;
    use std::ffi::c_void;

    const RELATION_PROCESSOR_CORE: u32 = 0;

    #[repr(C)]
    struct GroupAffinity {
        mask: usize,
        group: u16,
        reserved: [u16; 3],
    }

    #[repr(C)]
    struct ProcessorRelationship {
        flags: u8,
        efficiency_class: u8,
        reserved: [u8; 20],
        group_count: u16,
        group_mask: [GroupAffinity; 1],
    }

    #[repr(C)]
    struct SystemLogicalProcessorInfoExHeader {
        relationship: u32,
        size: u32,
        processor: ProcessorRelationship,
    }

    unsafe extern "system" {
        fn GetActiveProcessorGroupCount() -> u16;
        fn GetCurrentProcess() -> *mut c_void;
        fn GetCurrentThread() -> *mut c_void;
        fn GetProcessAffinityMask(
            process: *mut c_void,
            process_mask: *mut usize,
            system_mask: *mut usize,
        ) -> i32;
        fn GetLogicalProcessorInformationEx(
            relationship: u32,
            buffer: *mut u8,
            returned_length: *mut u32,
        ) -> i32;
        fn SetThreadAffinityMask(thread: *mut c_void, mask: usize) -> usize;
    }

    pub(super) fn detect_topology() -> Topology {
        let mut topo = Topology::default();
        unsafe {
            if GetActiveProcessorGroupCount() != 1 {
                return topo;
            }
            let mut allowed: usize = 0;
            let mut system_mask: usize = 0;
            if GetProcessAffinityMask(GetCurrentProcess(), &mut allowed, &mut system_mask) == 0
                || allowed == 0
            {
                return topo;
            }
            let mut len: u32 = 0;
            GetLogicalProcessorInformationEx(
                RELATION_PROCESSOR_CORE,
                std::ptr::null_mut(),
                &mut len,
            );
            if len == 0 {
                return topo;
            }
            // Ensure 8-byte alignment for reading SystemLogicalProcessorInfoExHeader.
            let words = (len as usize + 7) / 8;
            let mut storage = vec![0u64; words];
            let buf = storage.as_mut_ptr() as *mut u8;
            if GetLogicalProcessorInformationEx(RELATION_PROCESSOR_CORE, buf, &mut len) == 0 {
                return topo;
            }

            let mut min_class = u8::MAX;
            let mut max_class = 0u8;
            let mut cores: Vec<(u8, usize)> = Vec::new();
            let mut off = 0usize;
            while off + std::mem::size_of::<SystemLogicalProcessorInfoExHeader>() <= len as usize {
                let info = &*(buf.add(off) as *const SystemLogicalProcessorInfoExHeader);
                let size = info.size as usize;
                if size == 0 || off + size > len as usize {
                    break;
                }
                if info.relationship == RELATION_PROCESSOR_CORE && info.processor.group_count >= 1 {
                    let mask = info.processor.group_mask[0].mask & allowed;
                    if mask != 0 {
                        let eff = info.processor.efficiency_class;
                        min_class = min_class.min(eff);
                        max_class = max_class.max(eff);
                        cores.push((eff, mask));
                    }
                }
                off += size;
            }

            topo.hybrid = min_class < max_class;
            // Pass 0: first logical processor of each highest-efficiency core
            // Pass 1: SMT siblings of highest-efficiency cores
            // Pass 2: lower-efficiency (E) cores
            for pass in 0..3 {
                for &(eff, mut mask) in &cores {
                    let is_perf = !topo.hybrid || eff == max_class;
                    let mut first = true;
                    while mask != 0 {
                        let bit = mask.trailing_zeros() as usize;
                        mask &= mask - 1;
                        let target_pass = if is_perf {
                            if first { 0 } else { 1 }
                        } else {
                            2
                        };
                        if target_pass == pass && bit < usize::BITS as usize {
                            topo.order.push(bit);
                            if topo.hybrid && is_perf {
                                topo.n_performance_logical += 1;
                            }
                        }
                        first = false;
                    }
                }
            }
        }
        topo
    }

    pub(super) fn pin_current_thread(cpu: usize) -> Option<usize> {
        if cpu >= usize::BITS as usize {
            return None;
        }
        let prev = unsafe { SetThreadAffinityMask(GetCurrentThread(), 1usize << cpu) };
        (prev != 0).then_some(prev)
    }

    pub(super) fn restore_current_thread(prev: usize) {
        if prev != 0 {
            unsafe {
                SetThreadAffinityMask(GetCurrentThread(), prev);
            }
        }
    }
}

#[cfg(not(target_os = "windows"))]
mod win_affinity {
    use super::Topology;
    pub(super) fn detect_topology() -> Topology {
        Topology::default()
    }
    pub(super) fn pin_current_thread(_cpu: usize) -> Option<usize> {
        None
    }
    pub(super) fn restore_current_thread(_prev: usize) {}
}

type WorkFn = *const (dyn Fn(usize, usize) + Sync);

#[repr(align(64))]
struct Shared {
    // Publish generation and width together. A worker may miss generations
    // in which it was inactive, but must never use a later generation's width.
    task: AtomicU64,
    done_count: AtomicUsize,
    worker_panicked: AtomicBool,
    sleeping: AtomicUsize,
    session_depth: AtomicUsize,
    shutdown: AtomicBool,
    work_fn: UnsafeCell<Option<WorkFn>>,
    sleep_mu: Mutex<()>,
    sleep_cv: Condvar,
}

unsafe impl Send for Shared {}
unsafe impl Sync for Shared {}

pub(crate) struct Pool {
    n_threads: usize,
    decode_threads: usize,
    topology: Topology,
    shared: Arc<Shared>,
    dispatch_lock: Mutex<()>,
    workers: Vec<JoinHandle<()>>,
}

pub(crate) struct SessionGuard<'a> {
    pool: &'a Pool,
    prev_affinity: Option<usize>,
}

impl Drop for SessionGuard<'_> {
    fn drop(&mut self) {
        if let Some(prev) = self.prev_affinity {
            win_affinity::restore_current_thread(prev);
        }
        self.pool
            .shared
            .session_depth
            .fetch_sub(1, Ordering::Release);
    }
}

impl Pool {
    pub(crate) fn new(threads: usize) -> std::io::Result<Self> {
        let n_threads = threads.max(1);
        if n_threads > u16::MAX as usize {
            return Err(std::io::Error::new(std::io::ErrorKind::InvalidInput, "too many workers"));
        }
        let topology = win_affinity::detect_topology();
        let decode_threads = if topology.hybrid && topology.n_performance_logical > 0 {
            n_threads.min(topology.n_performance_logical)
        } else {
            n_threads
        };

        let shared = Arc::new(Shared {
            task: AtomicU64::new(0),
            done_count: AtomicUsize::new(0),
            worker_panicked: AtomicBool::new(false),
            sleeping: AtomicUsize::new(0),
            session_depth: AtomicUsize::new(0),
            shutdown: AtomicBool::new(false),
            work_fn: UnsafeCell::new(None),
            sleep_mu: Mutex::new(()),
            sleep_cv: Condvar::new(),
        });

        let mut pool = Self {
            n_threads,
            decode_threads,
            topology,
            shared,
            dispatch_lock: Mutex::new(()),
            workers: Vec::with_capacity(n_threads.saturating_sub(1)),
        };
        for ith in 1..n_threads {
            let shared_w = Arc::clone(&pool.shared);
            let cpu = pool.topology.order.get(ith).copied();
            let handle = thread::Builder::new()
                .name(format!("plamo-w{ith}"))
                .spawn(move || {
                    if let Some(c) = cpu {
                        let _ = win_affinity::pin_current_thread(c);
                    }
                    worker_loop(&shared_w, ith);
                })?;
            pool.workers.push(handle);
        }

        Ok(pool)
    }

    #[inline]
    pub(crate) fn num_threads(&self) -> usize {
        self.n_threads
    }

    #[inline]
    pub(crate) fn decode_threads(&self) -> usize {
        self.decode_threads
    }

    /// Enter a hot inference session: keeps workers spinning briefly between
    /// barriers and pins the calling thread (worker 0) to the first P-core.
    pub(crate) fn enter(&self) -> SessionGuard<'_> {
        self.shared.session_depth.fetch_add(1, Ordering::Acquire);
        let prev_affinity = self
            .topology
            .order
            .first()
            .copied()
            .and_then(win_affinity::pin_current_thread);
        SessionGuard {
            pool: self,
            prev_affinity,
        }
    }

    /// Execute `f(ith, nth)` across `nth = max_threads.min(self.n_threads).max(1)`
    /// threads, with thread 0 running inline on the caller thread.
    #[inline]
    pub(crate) fn dispatch<F>(&self, max_threads: usize, f: F)
    where
        F: Fn(usize, usize) + Sync,
    {
        let nth = max_threads.min(self.n_threads).max(1);
        if nth <= 1 {
            f(0, 1);
            return;
        }
        let _lock = self.dispatch_lock.lock().unwrap_or_else(|e| e.into_inner());
        let trait_obj: &(dyn Fn(usize, usize) + Sync) = &f;
        // Safety: workers only access work_fn while this function blocks until
        // done_count reaches nth - 1, strictly before `f` goes out of scope.
        let erased: WorkFn = unsafe { std::mem::transmute(trait_obj) };
        unsafe {
            *self.shared.work_fn.get() = Some(erased);
        }
        self.shared.worker_panicked.store(false, Ordering::Relaxed);
        self.shared.done_count.store(0, Ordering::Release);
        let generation = (self.shared.task.load(Ordering::Relaxed) >> 16) + 1;
        self.shared.task.store((generation << 16) | nth as u64, Ordering::SeqCst);

        if self.shared.sleeping.load(Ordering::SeqCst) > 0 {
            let _g = self.shared.sleep_mu.lock().unwrap_or_else(|e| e.into_inner());
            self.shared.sleep_cv.notify_all();
        }

        // Run slice 0 on the calling thread; catch panic so we always wait for
        // workers before `f` is dropped.
        let main_res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            f(0, nth);
        }));

        let target = nth - 1;
        while self.shared.done_count.load(Ordering::Acquire) < target {
            std::hint::spin_loop();
        }
        unsafe {
            *self.shared.work_fn.get() = None;
        }

        if let Err(payload) = main_res {
            std::panic::resume_unwind(payload);
        }
        assert!(!self.shared.worker_panicked.load(Ordering::Relaxed), "inference worker panicked");
    }
}

impl Drop for Pool {
    fn drop(&mut self) {
        self.shared.session_depth.store(0, Ordering::Release);
        self.shared.shutdown.store(true, Ordering::SeqCst);
        {
            let _g = self.shared.sleep_mu.lock().unwrap_or_else(|e| e.into_inner());
            self.shared.sleep_cv.notify_all();
        }
        for handle in self.workers.drain(..) {
            let _ = handle.join();
        }
    }
}

fn worker_loop(shared: &Shared, ith: usize) {
    let mut last_task = 0u64;
    loop {
        let mut spins = 0u32;
        let cur = loop {
            let cur = shared.task.load(Ordering::Acquire);
            if cur != last_task {
                break cur;
            }
            if shared.shutdown.load(Ordering::Acquire) {
                return;
            }
            if shared.session_depth.load(Ordering::Relaxed) > 0 && spins < SPIN_LIMIT {
                spins += 1;
                std::hint::spin_loop();
            } else {
                let mut guard = shared.sleep_mu.lock().unwrap_or_else(|e| e.into_inner());
                shared.sleeping.fetch_add(1, Ordering::SeqCst);
                let found = loop {
                    let cur = shared.task.load(Ordering::SeqCst);
                    if cur != last_task || shared.shutdown.load(Ordering::SeqCst) {
                        break cur;
                    }
                    guard = shared
                        .sleep_cv
                        .wait(guard)
                        .unwrap_or_else(|e| e.into_inner());
                };
                shared.sleeping.fetch_sub(1, Ordering::SeqCst);
                drop(guard);
                if found != last_task {
                    break found;
                }
                if shared.shutdown.load(Ordering::Acquire) {
                    return;
                }
            }
        };

        last_task = cur;
        let active = cur as u16 as usize;
        if ith < active {
            struct DoneGuard<'a>(&'a AtomicUsize);
            impl Drop for DoneGuard<'_> {
                fn drop(&mut self) {
                    self.0.fetch_add(1, Ordering::Release);
                }
            }
            let _done = DoneGuard(&shared.done_count);
            let f_ptr = unsafe { (*shared.work_fn.get()).unwrap_unchecked() };
            let f: &(dyn Fn(usize, usize) + Sync) = unsafe { &*f_ptr };
            if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(ith, active))).is_err() {
                shared.worker_panicked.store(true, Ordering::Relaxed);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pool_dispatches_and_joins() {
        let pool = Pool::new(4).unwrap();
        let _guard = pool.enter();
        let hits = AtomicUsize::new(0);
        for _ in 0..100 {
            pool.dispatch(4, |ith, nth| {
                assert!(ith < nth);
                hits.fetch_add(1, Ordering::Relaxed);
            });
        }
        assert_eq!(hits.load(Ordering::Relaxed), 400);
    }

    #[test]
    fn alternating_width_dispatches_each_partition_once() {
        let pool = Pool::new(4).unwrap();
        let _guard = pool.enter();
        for width in [2, 4, 1, 3, 2, 4].into_iter().cycle().take(600) {
            let hits = [const { AtomicUsize::new(0) }; 4];
            pool.dispatch(width, |i, n| {
                assert_eq!(n, width);
                hits[i].fetch_add(1, Ordering::Relaxed);
            });
            for (i, hit) in hits.iter().enumerate() {
                assert_eq!(hit.load(Ordering::Relaxed), usize::from(i < width));
            }
        }
    }

    #[test]
    fn worker_failure_reaches_caller_and_pool_remains_reusable() {
        let pool = Pool::new(2).unwrap();
        let failed = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            pool.dispatch(2, |i, _| assert_eq!(i, 0));
        }));
        assert!(failed.is_err());
        let hits = AtomicUsize::new(0);
        pool.dispatch(2, |_, _| { hits.fetch_add(1, Ordering::Relaxed); });
        assert_eq!(hits.load(Ordering::Relaxed), 2);
    }

    #[test]
    fn pool_single_thread() {
        let pool = Pool::new(1).unwrap();
        let _guard = pool.enter();
        let mut ran = 0;
        let r = AtomicUsize::new(0);
        pool.dispatch(4, |ith, nth| {
            assert_eq!(ith, 0);
            assert_eq!(nth, 1);
            r.fetch_add(1, Ordering::Relaxed);
        });
        ran += r.load(Ordering::Relaxed);
        assert_eq!(ran, 1);
    }
}
