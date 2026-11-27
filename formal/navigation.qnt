module navigation {
  // Public C ABI at quiescent points. File and page contents refer to the
  // existing synthetic fixtures. Each action is replayable on a real Reader.
  type View = { pdf: str, page: int, count: int, marker: str }
  pure def countPages(pdf: str): int =
    if (pdf == "simple") 3 else if (Set("crop", "unlocked").contains(pdf)) 1 else 0
  pure def marker(pdf: str, p: int): str =
    if (pdf == "simple" and p == 1) "3 sensors"
    else if (pdf == "simple" and p == 2) "12 samples"
    else if (pdf == "crop") "Visible in the crop box."
    else if (pdf == "unlocked") "11 samples"
    else ""
  pure def atPage(pdf: str, p: int): View = {
    pdf: pdf, page: p, count: countPages(pdf), marker: marker(pdf, p)
  }
  var v: View
  var op: str
  var argument: int
  var changed: bool
  var searchMiss: bool
  action init = all {
    v' = atPage("idle", 0), op' = "init", argument' = 0, changed' = false, searchMiss' = false
  }
  pure val files = ["simple", "crop", "locked", "unlocked", "missing"]
  action openFile(i: int): bool = {
    val pdf = files[i]
    all { v' = atPage(pdf, if (countPages(pdf) == 0) 0 else 1),
          op' = "open", argument' = i, changed' = true, searchMiss' = false }
  }
  action go(p: int): bool = {
    val valid = p > 0 and p <= v.count and p != v.page
    all { v' = if (valid) atPage(v.pdf, p) else v,
          op' = "page", argument' = p, changed' = valid, searchMiss' = false }
  }
  // The UI submits page 2, a search and another PDF without polling between
  // them. Only the final document may be visible when the workers settle.
  action replaceWhileBusy = all {
    v.pdf == "simple", v.page == 1, v' = atPage("crop", 1),
    op' = "replace", argument' = 0, changed' = true, searchMiss' = false,
  }
  // Queries are whitespace, SENSORS, samples, and a miss. An empty result
  // keeps the starting page. A valid search still advances the revision.
  pure def matchesQuery(pdf: str, p: int, q: int): bool =
    (pdf == "simple" and ((p == 1 and q == 1) or (p == 2 and q == 2))) or
    (pdf == "unlocked" and q == 2)
  action findText(q: int): bool = {
    val valid = q != 0 and v.count > 0
    val offsets = 1.to(v.count).filter(i =>
      matchesQuery(v.pdf, 1 + (v.page - 1 + i) % v.count, q))
    val first = offsets.fold(v.count, (a, b) => if (a < b) a else b)
    val target = if (valid) 1 + (v.page - 1 + first) % v.count else v.page
    all { v' = atPage(v.pdf, target), op' = "find", argument' = q, changed' = valid,
          searchMiss' = (valid and offsets == Set()) }
  }
  action step = any {
    nondet i = files.indices().oneOf()
    openFile(i),
    nondet p = 0.to(4).oneOf()
    go(p),
    nondet q = 0.to(3).oneOf()
    findText(q),
    replaceWhileBusy,
  }
  invariant pageRange = (v.count == 0 and v.page == 0 and v.marker == "") or
    (v.page >= 1 and v.page <= v.count)
  invariant fixtureSource = v.marker == marker(v.pdf, v.page)

  run navigationTest = init.then(go(1)).then(findText(2)).then(openFile(0))
    .then(go(0)).then(go(4)).then(go(1)).then(findText(0)).then(findText(2))
    .expect(v.page == 2 and not(searchMiss)).then(findText(3)).expect(v.page == 2 and searchMiss)
    .then(findText(1)).expect(v.page == 1).then(replaceWhileBusy)
    .then(openFile(0)).then(go(3)).then(openFile(2))
    .then(findText(1)).then(openFile(3)).then(openFile(4)).then(openFile(1))
    .expect(v.marker == "Visible in the crop box.")
}
