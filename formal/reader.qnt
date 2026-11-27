module reader {
  // Documents 1 and 2 have two pages and one page respectively. A nonzero
  // body or image carries its originating (document,page), even across Find.
  pure def pages(doc: int): int = if (doc == 1) 2 else 1
  pure def origin(doc: int, page: int): int = 10 * doc + page
  type View = { doc: int, page: int, count: int, phase: str,
                image: int, source: int, translation: int, ocrFailed: bool }
  pure val idle = { doc: 0, page: 0, count: 0, phase: "Idle",
                    image: 0, source: 0, translation: 0, ocrFailed: false }
  var view: View
  var work: str

  // One worker can be paused immediately before Shared::update. Other
  // publications can continue. Only equality with the latest generation is
  // observable, so a superseded ticket stays stale even on revisiting a page.
  type Reply = { view: View, nextWork: str }
  var pending: Reply
  var ticket: str
  var staleChangedView: bool
  pure val noReply = { view: idle, nextWork: "none" }
  action init = all {
    view' = idle, work' = "none", pending' = noReply,
    ticket' = "none", staleChangedView' = false,
  }
  action request(v: View, stage: str): bool = all {
    work != "closed", view' = v, work' = stage,
    ticket' = if (ticket == "current") "stale" else ticket,
    pending' = pending, staleChangedView' = staleChangedView,
  }
  action openDoc(d: int): bool =
    request({ ...idle, doc: d, phase: "Loading" }, "open")
  pure def loading(v: View, p: int): View = {
    ...idle, doc: v.doc, page: p, count: pages(v.doc), phase: "Loading"
  }
  action navigate(p: int): bool = all {
    p > 0, p <= view.count, p != view.page, request(loading(view, p), "render")
  }
  // Search keeps the old, coherent snapshot until the worker resolves the
  // match (or returns to the starting page on a miss).
  action search = all {
    view.page > 0, request(view, "find")
  }
  action close = all {
    view' = idle, work' = "closed", ticket' = "none",
    pending' = noReply, staleChangedView' = staleChangedView,
  }

  // Compute the result using the issuing worker's data, before the possible
  // delay. Delivering a deferred result must never retag it to a newer page.
  action immediate(v: View, stage: str): bool = all {
    view' = v, work' = stage, pending' = pending, ticket' = ticket,
    staleChangedView' = staleChangedView,
  }
  action defer(v: View, stage: str): bool = all {
    ticket == "none", pending' = { view: v, nextWork: stage },
    ticket' = "current", view' = view, work' = "waiting",
    staleChangedView' = staleChangedView,
  }
  action publish(v: View, stage: str, delayed: bool): bool =
    if (delayed) defer(v, stage) else immediate(v, stage)
  action deliver = all {
    ticket != "none",
    val updated = if (ticket == "current") pending.view else view
    val stage = if (ticket == "current") pending.nextWork else work
    all {
      view' = updated,
      work' = stage,
      staleChangedView' = (staleChangedView or
        (ticket == "stale" and (updated != view or stage != work))),
      pending' = noReply, ticket' = "none",
    },
  }
  val key = origin(view.doc, view.page)
  // Search status is separate from the displayed page. Resume unfinished
  // work if needed; a settled page can keep its existing snapshot.
  val resumeWork =
    if (view.phase == "Loading") (if (view.image == 0) "render" else "extract")
    else if (view.phase == "ReadingImage") (if (view.source == 0) "ocrBlank" else "ocrDamaged")
    else if (view.phase == "Translating") "translate" else "none"
  action searchResult(p: int, delayed: bool): bool = all {
    work == "find", p > 0, p <= view.count,
    if (p == view.page) publish(view, resumeWork, delayed)
    else publish(loading(view, p), "render", delayed),
  }

  // Events describe what the PDF, OCR or inference engine returned. Tests
  // and the checker both go through the worker that handles that event.
  pure val events = Set("opened", "password", "rendered", "text", "blank", "damaged",
                        "recognized", "noText", "failed", "inferred", "publishResult")
  action onResult(event: str, delayed: bool): bool = any {
    all { work == "open", event == "opened", publish(loading(view, 1), "render", delayed) },
    all { work == "open", event == "password",
          publish({ ...view, phase: "Password" }, "none", delayed) },
    all { work == "render", event == "rendered",
          publish({ ...view, image: key }, "extract", delayed) },
    all { work == "extract", Set("blank", "damaged").contains(event),
          val readable = event == "damaged"
          publish({ ...view, phase: "ReadingImage", source: if (readable) key else 0 },
                  if (readable) "ocrDamaged" else "ocrBlank", delayed) },
    all { (work == "extract" and event == "text") or
          (Set("ocrBlank", "ocrDamaged").contains(work) and event == "recognized"),
          publish({ ...view, phase: "Translating", source: key }, "translate", delayed) },
    all { Set("ocrBlank", "ocrDamaged").contains(work), Set("noText", "failed").contains(event),
          val failed = event == "failed"
          val phase = if (view.source != 0) "Translating" else if (failed) "Error" else "Empty"
          publish({ ...view, phase: phase, ocrFailed: failed },
                  if (view.source != 0) "translate" else "none", delayed) },
    all { Set("open", "render", "extract", "translate").contains(work), event == "failed",
          publish({ ...view, phase: "Error" }, "none", delayed) },
    // A successful engine reply creates an obligation before the display
    // handler runs. Publication may still be delayed or superseded.
    all { work == "translate", event == "inferred", immediate(view, "publishTranslation") },
    all { work == "publishTranslation", event == "publishResult",
          publish({ ...view, phase: "Ready", translation: view.source }, "none", delayed) },
  }
  action worker = {
    nondet delayed = Bool.oneOf()
    any {
      nondet event = events.oneOf()
      onResult(event, delayed),
      nondet p = 1.to(view.count).oneOf()
      searchResult(p, delayed),
    }
  }
  action step = any {
    nondet d = Set(1, 2).oneOf()
    openDoc(d),
    nondet p = Set(1, 2).oneOf()
    navigate(p),
    search, worker, deliver, close,
  }

  invariant pageBounds = (view.page == 0 and view.count == 0) or
    (view.page >= 1 and view.page <= view.count and view.count == pages(view.doc))
  invariant coherentSnapshot = all {
    view.image == 0 or (view.page > 0 and view.image == key),
    view.source == 0 or (view.image != 0 and view.source == view.image),
    view.translation == 0 or view.translation == view.source,
    view.phase == "Ready" implies view.translation != 0,
    view.phase == "Translating" implies view.source != 0,
    view.phase == "Empty" implies (view.image != 0 and view.source == 0),
    view.phase == "Password" implies (view.page == 0 and view.image == 0),
  }
  invariant generationIsolation = not(staleChangedView) and
    ((work == "waiting") == (ticket == "current"))
  invariant released = work == "closed" implies
    (view == idle and ticket == "none" and pending == noReply)

  run renderedPage = init.then(openDoc(1)).then(onResult("opened", false))
    .then(onResult("rendered", false))
  run successfulInference = renderedPage.then(onResult("text", false)).then(onResult("inferred", false))
  run currentTranslationTest = successfulInference.then(onResult("publishResult", true))
    .then(deliver).expect(view.phase == "Ready" and view.translation == origin(1, 1))
    .then(search).then(searchResult(1, false))
    .expect(view.phase == "Ready" and view.translation == origin(1, 1) and work == "none")
  run lateTranslationTest = successfulInference.then(onResult("publishResult", true))
    .expect(ticket == "current").then(navigate(2)).then(navigate(1))
    .then(deliver).expect(view.phase == "Loading" and view.translation == 0)
  run ocrFailureKeepsTextTest = renderedPage.then(onResult("damaged", false))
    .then(onResult("failed", false))
    .expect(view.ocrFailed and view.source == origin(1, 1) and work == "translate")
    .then(onResult("inferred", false)).then(onResult("publishResult", false))
    .expect(view.ocrFailed and view.phase == "Ready" and view.translation == origin(1, 1))
  run ocrFailureWithoutTextTest = renderedPage.then(onResult("blank", false))
    .then(onResult("failed", false)).expect(view.ocrFailed and view.phase == "Error" and view.source == 0)
}

module readerProgress {
  import reader as r
  var quiet: bool
  action init = all { r::init, quiet' = false }
  action advance = any { r::worker, r::deliver }
  action step = any {
    all { not(quiet), r::step, quiet' = false },
    // Stop submitting UI requests at any reachable point.
    all { not(quiet), r::immediate(r::view, r::work), quiet' = true },
    all { advance, quiet' = quiet },
  }
  val settled = Set("none", "closed").contains(r::work) and r::ticket == "none"
  temporal finishesAfterLastRequest =
    advance.weakFair((r::work, r::view, r::ticket, r::pending)) implies quiet.leadsTo(settled)
  temporal publishesSuccessfulTranslation =
    advance.weakFair((r::work, r::view, r::ticket, r::pending)) implies
      (quiet and r::work == "publishTranslation").leadsTo(
        r::view.phase == "Ready" and r::view.translation == r::key)
}

// A readable source and successful inference must lead to publication, even
// if optional OCR fails. Removing the success handler must fail this check.
module readableDocument {
  import reader as r
  action init = r::init
  action advance = any {
    all { r::view.phase == "Idle", r::openDoc(1) },
    nondet event = Set("opened", "rendered", "text", "damaged", "failed", "inferred", "publishResult").oneOf()
    nondet delayed = Bool.oneOf()
    all { event != "failed" or r::work == "ocrDamaged", r::onResult(event, delayed) },
    r::deliver,
  }
  action step = any { advance, r::immediate(r::view, r::work) }
  temporal showsTranslation =
    advance.weakFair((r::work, r::view, r::ticket, r::pending)) implies
      eventually(r::view.phase == "Ready" and r::view.translation == r::origin(1, 1))
}
