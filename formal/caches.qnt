module caches {
  // Entries are ordered oldest to newest. A key's final digit determines its
  // cost. Small examples exercise count pressure, byte pressure and oversize.
  pure def cost(k: int): int = if (k % 10 <= 2) 1 else k % 10
  pure def fits(xs: List[int]): bool =
    xs.length() <= 2 and xs.foldl(0, (total, k) => total + cost(k)) <= 3
  pure def has(xs: List[int], k: int): bool = xs.indices().exists(i => xs[i] == k)
  pure def admit(xs: List[int], k: int): List[int] = {
    if (cost(k) > 3) xs else {
      val touched = xs.select(x => x != k).append(k)
      // Remove the shortest oldest prefix necessary to meet both limits.
      val cuts = 0.to(touched.length()).filter(i => fits(touched.slice(i, touched.length())))
      val cut = cuts.fold(touched.length(), (a, b) => if (a < b) a else b)
      touched.slice(cut, touched.length())
    }
  }
  pure def distinct(xs: List[int]): bool =
    xs.indices().forall(i => xs.indices().forall(j => i == j or xs[i] != xs[j]))

  var doc: int
  var recent: List[int]  // (document,page) identifies raster and effective source
  var priorPages: List[int]  // witness for an attempted oversize insertion
  var translations: List[int]  // exact source identity, with a fixed model/style
  var shown: int         // source identity carried by the completed translation
  var pageKey: int
  var inferred: bool
  var wasResident: bool
  // Page 24 has an oversize raster but the same short text as page 21.
  pure def source(k: int): int = if (k % 10 == 1 or k == 24) 1 else k
  action init = all {
    doc' = 0, recent' = [], priorPages' = [], translations' = [], shown' = 0,
    pageKey' = 0, inferred' = false, wasResident' = false,
  }
  action openDoc(d: int): bool = all {
    doc' = d, recent' = [], priorPages' = [], translations' = if (d == 0) [] else translations, shown' = 0,
    pageKey' = 0, inferred' = false, wasResident' = false,
  }
  action readPage(p: int, success: bool): bool = {
    val k = doc * 10 + p
    // pdf_loop indexes recent by page alone, relying on clear() on Open.
    val matches = recent.select(x => x % 10 == p)
    val raster = if (matches.length() == 0) k else matches[0]
    val input = source(raster)
    val hit = has(translations, input)
    val complete = hit or success
    all {
      doc != 0, doc' = doc, pageKey' = k, recent' = admit(recent, raster),
      priorPages' = if (cost(k) > 3) recent else [],
      shown' = if (complete) input else 0, inferred' = not(hit),
      wasResident' = has(translations, source(k)),
      translations' = if (complete) admit(translations, input) else translations,
    }
  }
  action step = any {
    nondet d = Set(0, 1, 2).oneOf()  // 0 drops the session
    openDoc(d),
    nondet p = 1.to(4).oneOf()
    nondet success = Bool.oneOf()
    readPage(p, success),
  }
  invariant budgets = fits(recent) and fits(translations)
  invariant oversizedPageKeepsResidents = cost(pageKey) <= 3 or recent == priorPages
  invariant uniqueKeys = distinct(recent) and distinct(translations)
  invariant documentLocal = recent.indices().forall(i => recent[i] / 10 == doc)
  invariant correctResult = shown == 0 or shown == source(pageKey)
  invariant reuse = pageKey == 0 or
    (inferred != wasResident and (wasResident implies shown != 0))
  invariant released = doc != 0 or (recent == [] and translations == [] and shown == 0)

  run recencyTest = init.then(openDoc(1)).then(readPage(1, true))
    .then(readPage(2, true)).then(readPage(1, false)).expect(translations == [12, 1])
    .then(openDoc(2)).expect(recent == [] and translations == [12, 1])
    .then(readPage(1, false)).expect(shown == 1 and not(inferred))
    .then(readPage(2, true)).expect(translations == [1, 22])
    .then(readPage(3, true)).expect(translations == [23] and recent == [23])
    .then(readPage(4, true)).expect(translations == [1] and recent == [23] and shown == 1)
    .then(readPage(4, false)).expect(shown == 1 and not(inferred) and recent == [23])
  run oversizePreservesOrderTest = init.then(openDoc(1))
    .then(readPage(2, true)).then(readPage(1, true)).then(readPage(4, true))
    .expect(recent == [12, 11] and translations == [12, 1] and shown == 14)
  run failedMissTest = init.then(openDoc(1)).then(readPage(2, false))
    .expect(translations == [] and shown == 0).then(readPage(2, true))
    .expect(inferred and shown == 12)
}
