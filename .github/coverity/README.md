# Coverity Scan

`checks.yml` captures Windows, macOS and Linux independently at the same commit.
Linux captures both hidraw and libusb. Each job archives its entire intermediate
directory; a collector uses `cov-manage-emit reset-host-name` and `add`, without
editing Coverity's host directories or lock files.

Before submission, `merge.py` checks tool versions, compatibility, integrity,
required backend sources and the exact multiset of captured source paths. Empty
captures and recoverable capture errors fail the job. Archives and a merge
summary remain available as workflow artifacts for seven days. Compiler setup
runs on every capture; downloads are checksum-verified and the tool cache contains
the unconfigured installation.

Run **Checks** manually on the desired branch, or let the weekly schedule run on
the default branch. All branches share a concurrency group because the Scan
project has one snapshot history; GitHub retains at most one pending run in that
group. Successful upload only means the service
accepted the archive: confirm the submitted commit in Scan's analysis history.
Combining captures does not isolate the analyzer's inferred backend models.

## Modeling file

[model.c](model.c) models HIDAPI's `hid_open` and `hid_open_path`: each returns
NULL or an initialized, owned opaque handle. `hid_close` remains source-derived
so its native-handle, nested-allocation and synchronization effects are retained,
including when it cleans up a partially constructed device inside HIDAPI.

The model deliberately leaves enumeration, borrowed strings/device information,
partial I/O and threading to source analysis. It does not override libusb or
standard-library functions, claim thread safety, or suppress particular defects.
Uninitialized local choices are intentional Coverity modeling constructs.
The file is self-contained because Scan's model compiler has no system-header
search path. Its local `wchar_t` declaration is only a placeholder for the unused
serial-number pointer; the model does not read characters or depend on their size.

Upload the single C file in **Analysis Settings -> Modeling File**. Committing it
does not activate it, and it must never be added to a build target. User models
override built-in and source-derived summaries, so inspect changes in findings
after each update. Check the service's model compilation result and analyze the
same captured revision when comparing behavior. A successful model upload
automatically queues the last uploaded build for reanalysis.

With a full Coverity installation, compile it locally with:

```sh
mkdir -p build/coverity-model
timeout --kill-after=3s 60s cov-make-library \
  -of build/coverity-model/user_models.xmldb .github/coverity/model.c
```

The Scan capture-only bundle does not contain `cov-make-library`. GCC/Clang
syntax checks validate declarations, not Coverity's generated summaries.

References: [Scan tuning](https://scan.coverity.com/tune),
[model semantics](https://scan.coverity.com/models),
[supported merge procedure](https://docs.blackduck.com/r/coverity/2026.9/coverity-documentation/copying-intermediate-directories-from-local-disks.html).
