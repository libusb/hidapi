# Coverity

`model.c` describes HIDAPI's nullable, owned device handles; cleanup, I/O and
threading remain source-derived. Upload it under **Analysis Settings → Modeling
File** to reanalyze the last build. It is analysis input, never runtime code.

`setup/` installs the capture tools. `merge.py` validates and combines native
Windows, macOS and Linux captures; `test_merge.py` checks its validation gates.
The [Checks workflow](../.github/workflows/checks.yml) runs capture and submission.
