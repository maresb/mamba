# Prepared Replies to PR #4110 Review Comments

Replies for jjerphan. Post these MANUALLY after the corresponding code changes are pushed. Do NOT post via `gh` API.

**WARNING:** These drafts were written before implementation. Recheck each reply against the actual changes before posting — wording (especially past-tense "Done" phrasing) may not match what was implemented.

---

## Comment 3 — `SOLVABLE_KEYWORDS` extensibility (solvable.cpp:525)

> Does this encoding allow extending usages of `SOLVABLE_KEYWORDS` for other applications than transferring `default_keys` without impacting this use-case?

**Reply:**

Good question. The current encoding assumes exclusive ownership of `SOLVABLE_KEYWORDS` — it stores `defaulted_keys` as a plain comma-separated list with no namespace prefix. This means other data couldn't coexist in the same field without a format change.

In practice this is fine: `SOLVABLE_KEYWORDS` is unused in the conda ecosystem (libsolv maps it from RPM's `Keywords` tag, which has no conda equivalent), so there's no collision risk. The encoding is also purely internal — it only lives in the in-memory libsolv pool and `.solv` cache files, never in user-facing artifacts — so we can evolve the format later if a new use case arises.

I've added a comment documenting this assumption at the encoding site.

---

## Comment 4 — Test for non-`_initialized` values (test_solvable.cpp:356)

> I think having another test with at least another values for keys but `"_initialized"` would be useful to specify the behavior (e.g. would `"_initialized"` be retrieved or not in this case?).
>
> ```cpp
> SECTION("Missing `_initialized` in `defaulted_keys`")
> {
>     solv.set_defaulted_keys({ "other_value" });
>     repo.internalize();
>
>     auto retrieved = solv.defaulted_keys();
>     REQUIRE(retrieved.size() == 1);
>     REQUIRE_FALSE(retrieved[0] == "other_value");
> }
> ```

**Reply:**

Good idea to test this case — added.

One note on the assertion: the solvable layer is a dumb key-value store; it faithfully round-trips whatever strings it receives, so `{ "other_value" }` comes back as `{ "other_value" }`. The `_initialized` sentinel is not enforced at this layer — validation happens in `write_repodata_record()`, which checks for `_initialized` presence and throws `std::logic_error` if it's missing.

So the test asserts round-trip fidelity (`REQUIRE(retrieved[0] == "other_value")`) rather than rejection, with a comment explaining the layered design.

---

## Comment 6 — `defaulted_keys` doc comment (package_info.hpp:80)

> Could you also mention the different cases which exists (from explicit specification in lock files / "URL-derived packages" (?), from the conda-package's `index.json` and from the `repodata` records) and to refer to them consistently?

**Reply:**

Done — rewrote the doc comment to cover all the cases (URL-derived, solver-derived, lockfile-derived) and the three states (empty = invalid, `{"_initialized"}` = trust all, `{"_initialized", ...}` = listed fields are stubs). Used your backtick convention throughout.

Also consolidated the duplicated design explanations in `helpers.cpp`, `package_fetcher.cpp`, `solvable.hpp`, and `bindings/specs.cpp` to reference this canonical doc instead.
