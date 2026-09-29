"""Source contract: transient registry read/parse must not replace primary."""
from pathlib import Path

from test_m4_astra_stability import SRC, function


def test_source():
    reg = (SRC / 'apps/M4xRegistry.cpp').read_text()
    load = function(reg, 'RegistryLoadResult loadRegistryUnlocked(')
    parse = function(reg, 'RegistryParse parseRegistry(')
    read = function(reg, 'RegistryRead readRegistryFile(')
    ui = function(reg, 'std::vector<M4xInstalledApp> M4xRegistry::load(')
    write = function(reg, 'bool M4xRegistry::tryLoad(')
    assert 'loadRegistryUnlocked(false)' in ui
    assert 'loadRegistryUnlocked(true)' in write
    assert 'failClosed' in write
    assert 'DeserializationError::NoMemory' in parse
    assert 'RegistryParse::Transient' in parse
    assert 'RegistryReadKind::IoError' in read
    transient = load[load.index('RegistryParse::Transient'):load.index('RegistryReadKind::IoError')]
    repair_start = load.index('// Primary is ')
    io = load[load.index('RegistryReadKind::IoError'):repair_start]
    assert 'writeAllTextExact' not in transient
    assert 'SdMan.remove' not in transient
    assert 'writeAllTextExact' not in io
    assert 'SdMan.remove' not in io
    repair = load[repair_start:]
    assert 'writeAllTextExact' in repair
    print('registry transient load source: PASS')


if __name__ == '__main__':
    test_source()
