# API comment conventions

> Applies to public headers under `include/thyme/**`. **This repository does not
> generate HTML documentation** (no MkDocs, no Doxygen `doc` target): comments
> are read directly as part of the header and the review, keeping them
> grep-able, diff-able, and free of extra build dependencies. Design and
> architecture: [ARCHITECTURE.md](../../ARCHITECTURE.md); terminology:
> [glossary.md](../reference/glossary.md).

## 1. Comment rules

1. **Every public API needs a brief.** Use `/** \brief One sentence. */` for
   every public type and function, describing *what it does*, not *how it is
   implemented*.
2. **Handles must document ownership and lifetime.** Any API that hands out a
   handle (pool slot, ring view, `PinnedSlot` lease, mapped SHM region, owned
   fd/file) must state:
   - who owns it (caller / container / producer);
   - the validity window (until `release()` / `CommitWrite()` / `Reset()`, or
     "borrowed, do not delete");
   - what happens on out-of-window use (dangling, overwrite, steal).
3. **Threading contracts belong next to the code.** SPSC = single producer,
   single consumer; `LatestSlot` = single writer; `LocalIndexRing` =
   drop-oldest. Read the type's comments before sharing an instance.
4. **Comments track behaviour.** When behaviour changes, update the comment in
   the same change; do not keep commented-out old code.
5. **Do not mass-add comments.** Add comments incrementally when changing an
   API or filling a key gap; never generate vague comments to chase coverage.

Example (`thyme::ObjectPool`):

```cpp
/** \brief Fixed-capacity pool of cacheline-isolated slots.
 *  \note acquire() returns a borrowed slot index; it stays valid only until
 *        release()/abort_write(). The pool owns the storage. */
```

## 2. Review and self-check

There is no generated output to inspect after rendering, so the convention is
enforced through the review checklist and header self-review:

- New public header: write the brief before merging.
- New public handle type: document ownership/lifetime in the type comment, and
  check the API item (D1) in
  [review-checklist.md](../reference/review-checklist.md).
- When documentation and implementation conflict, the implementation wins, and
  the documentation is fixed in the same change.
- Use `grep` for bulk checks (for example, `\brief` coverage is a hint, not a
  gate); do not add Doxygen/MkDocs dependencies.

## 3. Why HTML generation was removed

- An HTML site and a `doc` target would add toolchains (MkDocs/Material,
  Doxygen) that conflict with the "zero third-party runtime dependencies,
  reproducible builds" positioning.
- Generated output does not track code review and drifts from the headers. In
  this repository, the contract is the headers, `static_assert`s, capability
  tables, and ADRs.
- For reading convenience, use an IDE, `grep`, or GitHub's Markdown rendering;
  publishing a site is outside this repository's scope.
