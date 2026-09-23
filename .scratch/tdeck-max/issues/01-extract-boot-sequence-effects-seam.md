# 01: Extract the board profile boot sequence behind injected effects

**What to build:** Nothing a user can see. This is a prefactor that makes every later ticket verifiable.

Today the board profile boot sequence is only partly testable. Bus initialisation is generic over injected closures and has a real test asserting call order and early abort. The other phases — board-level GPIO outputs and driver loading — call their externs directly and are compiled out of test builds entirely, so the existing tests in that module verify parsing but cannot observe sequencing at all.

The whole sequence moves behind one function generic over its side effects, with production supplying the real implementations and tests supplying a recorder. Ordering becomes assertable, which matters because the IO expander work in ticket 02 turns boot ordering into the feature's most likely failure mode.

Follow the existing injected-closure pattern rather than inventing a new one; this subsumes the current bus seam rather than sitting beside it.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] The board profile boot sequence runs through a single function generic over its side effects, covering board-level GPIO outputs, bus initialisation and driver loading
- [ ] Production behaviour is unchanged on every board currently in the catalogue
- [ ] A test can inject a recorder and assert the order in which phases occur
- [ ] The existing behaviour where a failed bus stops initialisation before dependent buses is preserved and still covered
- [ ] The existing kernel test suite passes with no regressions
