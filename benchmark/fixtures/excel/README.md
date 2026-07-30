# Synthetic Excel report fixture

These files exercise `benchmark/create_excel.py`. All performance values,
hardware labels, match counts, and cache statistics are synthetic. They must
not be cited as experimental results.

The fixture intentionally includes:

- consecutive and cardinality semantic groups;
- pass, fail, missing, timeout, and error states;
- a repeat-count inconsistency;
- Gopher cache and ablation metrics;
- 1/2/4-thread CPU scaling rows;
- query-size and `k` sensitivity axes;
- an unknown future column and formula-like text for compatibility/security
  checks; and
- an explicitly unavailable synthetic GPU.

See `evaluation/EXCEL_REPORT.md` for the input and workbook contract.
