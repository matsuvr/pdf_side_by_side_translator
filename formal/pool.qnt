module pool {
  // Worker 0 runs on the caller. Workers 1 and 2 can miss dispatches or
  // pause after acquiring a packed (generation,width) and before using it.
  pure val workers = Set(1, 2)
  type Pool = { live: bool, width: int, unseen: Set[int], captured: int -> int,
                running: Set[int], finished: Set[int], count: int, callerDone: bool,
                panicked: bool, returned: str, invalidBorrow: bool }
  var s: Pool
  action init = s' = { live: false, width: 0, unseen: Set(),
    captured: workers.mapBy(_ => 0), running: Set(), finished: Set(), count: 0,
    callerDone: false, panicked: false, returned: "none", invalidBorrow: false }
  action dispatch(n: int): bool = all {
    not(s.live),
    s' = { ...s, live: true, width: n, unseen: workers, finished: Set(), count: 0,
           callerDone: false, panicked: false, returned: "none" },
  }
  action acquire(i: int): bool = all {
    s.unseen.contains(i), s.captured.get(i) == 0, not(s.running.contains(i)),
    s' = { ...s, unseen: s.unseen.exclude(Set(i)), captured: s.captured.set(i, s.width) },
  }
  action enter(i: int): bool = {
    val w = s.captured.get(i)
    val active = i < w
    all {
      w != 0,
      s' = { ...s, captured: s.captured.set(i, 0),
        running: if (active) s.running.union(Set(i)) else s.running,
        invalidBorrow: s.invalidBorrow or (active and
          (not(s.live) or w != s.width or s.finished.contains(i) or s.unseen.contains(i))) },
    }
  }
  action finish(i: int, panic: bool): bool = all {
    s.running.contains(i),
    s' = { ...s, running: s.running.exclude(Set(i)), finished: s.finished.union(Set(i)),
           count: s.count + 1, panicked: s.panicked or panic },
  }
  action caller(panic: bool): bool = all {
    s.live, not(s.callerDone), s' = { ...s, callerDone: true, panicked: s.panicked or panic },
  }
  action release = all {
    s.live, s.callerDone, s.count >= s.width - 1,
    s' = { ...s, live: false, returned: if (s.panicked) "panic" else "ok" },
  }
  action step = any {
    nondet n = Set(2, 3).oneOf()
    dispatch(n),
    nondet i = workers.oneOf()
    nondet panic = Bool.oneOf()
    any { acquire(i), enter(i), finish(i, panic), caller(panic) },
    release,
  }
  invariant borrowedOnlyDuringDispatch = not(s.invalidBorrow) and
    (s.live or s.running == Set())
  invariant exactlyOnce = s.count == s.finished.size() and
    s.running.intersect(s.finished) == Set()
  invariant joined = s.returned != "none" implies
    (s.callerDone and s.finished == workers.filter(i => i < s.width) and
     ((s.returned == "panic") == s.panicked))
  // During a dispatch the finite worker phases only advance. Weak fairness
  // rules out an infinite stutter while a runnable step is continuously enabled.
  temporal dispatchReturns = step.weakFair(s) implies s.live.leadsTo(not(s.live))

  run lateInactiveWorkerTest = init.then(dispatch(2)).then(acquire(2))
    .then(acquire(1)).then(enter(1)).then(finish(1, true)).then(caller(false))
    .then(release).expect(s.returned == "panic").then(dispatch(3))
    .then(enter(2)).expect(s.running == Set() and s.unseen.contains(2))
    .then(acquire(2)).then(enter(2)).expect(s.running == Set(2))
}
