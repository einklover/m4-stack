"""Host contract: registry load and save share one non-recursive transaction lock.

Limitation: this does not interleave two threads inside SdMan. A deterministic
interleave shim would have to stub every SD call and freeze one side mid
rename. This test checks that both functions take registryTxnMu for the whole
body, that load does not call save, and that the lock is not installGate.
test_m4_registry_journal.py still compiles and runs the real save body.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_m4_astra_stability import function

SRC = Path(__file__).resolve().parents[1] / 'src'
REG = (SRC / 'apps/M4xRegistry.cpp').read_text()


def body_after_brace(fn):
    return fn[fn.index('{') + 1:].lstrip()


def test_lock_spans_load_and_save():
    load = function(REG, 'std::vector<M4xInstalledApp> M4xRegistry::load(')
    save = function(REG, 'bool M4xRegistry::save(')
    lock_stmt = 'std::lock_guard<std::mutex> registryTxnLock(registryTxnMu());'
    assert body_after_brace(load).startswith(lock_stmt)
    assert body_after_brace(save).startswith(lock_stmt)
    assert load.count(lock_stmt) == 1
    assert save.count(lock_stmt) == 1
    assert 'unlock' not in load and 'unlock' not in save
    assert 'M4xRegistry::save' not in load
    assert 'installGate' not in load and 'installGate' not in save
    assert 'recursive_mutex' not in function(REG, 'std::mutex& registryTxnMu(')
    # Write-back and both rename phases stay after the lock is taken.
    assert load.index(lock_stmt) < load.index('writeAllTextExact')
    assert save.index(lock_stmt) < save.index('kRegistryBak')
    assert save.index(lock_stmt) < save.index('kRegistryTmp')
    assert re.search(r'registryTxnMu\(\)\s*\{[^}]*static std::mutex mu;', REG)


if __name__ == '__main__':
    test_lock_spans_load_and_save()
    print('registry txn lock boundary: PASS')
