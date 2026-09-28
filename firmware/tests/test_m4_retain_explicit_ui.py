"""User-reachable retain-journal release: settings entry, confirm gate, archive bytes.

The release call is only wired from PluginJournalReleaseActivity after the
confirm page requests it. Archive failure and same-id continuation stay in
the journal host harness (test_m4_astra_final_p1.py).
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware' / 'src'


def test_source_contract():
    settings = (SRC / 'activities/settings/SettingsActivity.cpp').read_text()
    activity = (SRC / 'activities/settings/PluginJournalReleaseActivity.cpp').read_text()
    ui = (SRC / 'activities/settings/M4PluginJournalReleaseUi.h').read_text()
    catalog = (SRC / 'activities/settings/M4SettingsRootUi.h').read_text()
    installer = (SRC / 'apps/M4xInstaller.cpp').read_text()
    main = (SRC / 'main.cpp').read_text()

    assert '修复未完成插件安装记录' in catalog
    assert 'pluginJournalRelease' in catalog
    assert 'new PluginJournalReleaseActivity' in settings
    assert 'listPendingJournalIds' in activity
    assert '未登记' in activity
    assert 'archiveAndReleasePending' in activity
    assert activity.index('if (!ui_.releaseRequested) return;') < activity.index(
        'M4xInstaller::archiveAndReleasePending')
    assert 'm4JournalReleaseActivate' in activity
    assert 'Confirm activate is the only path' in ui

    install = installer.split('M4xInstallResult M4xInstaller::install(', 1)[1].split(
        '\nbool M4xInstaller::uninstall(', 1)[0]
    uninstall = installer.split('bool M4xInstaller::uninstall(', 1)[1].split(
        '\nstd::string M4xInstaller::entryScriptPath(', 1)[0]
    recover = installer.split('void M4xInstaller::recoverInterrupted(', 1)[1].split(
        '\nM4xInstallResult M4xInstaller::probe(', 1)[0]
    assert 'refuseIfPendingJournal' in install and 'refuseIfPendingJournal' in uninstall
    for body in (install, uninstall, recover, main):
        assert 'archiveAndReleasePending' not in body
        assert 'PluginJournalReleaseActivity' not in body
    print('retain explicit ui source contract: PASS')


def test_ui_state():
    code = r'''
#include <cassert>
#include "activities/settings/M4SettingsRootUi.h"
#include "activities/settings/M4PluginJournalReleaseUi.h"
int main() {
  bool found = false;
  int boot = -1;
  const int n = m4SettingsChildCount("maintenance");
  for (int i = 0; i < n; ++i) {
    const M4SettingsRow* row = m4SettingsChildAt("maintenance", i);
    assert(row && row->key);
    if (std::strcmp(row->key, "switchBootSlot") == 0) boot = i;
    if (std::strcmp(row->key, "pluginJournalRelease") == 0) {
      found = true;
      assert(std::strcmp(row->titleZh, "修复未完成插件安装记录") == 0);
      assert(row->control == M4SettingsControl::Navigate);
    }
  }
  assert(found);
  assert(boot == 4);

  M4JournalReleaseUi ui;
  m4JournalReleaseSetCount(ui, 2);
  m4JournalReleaseActivate(ui);
  assert(ui.page == M4JournalReleasePage::Confirm);
  assert(!ui.releaseRequested);
  m4JournalReleaseCancel(ui);
  assert(ui.page == M4JournalReleasePage::List);
  assert(!ui.releaseRequested);
  assert(!ui.leave);

  m4JournalReleaseActivate(ui);
  m4JournalReleaseActivate(ui);
  assert(ui.releaseRequested);
  assert(ui.releaseIndex == 0);

  M4JournalReleaseUi cancel;
  m4JournalReleaseSetCount(cancel, 1);
  m4JournalReleaseCancel(cancel);
  assert(cancel.leave);
  assert(!cancel.releaseRequested);

  M4JournalReleaseUi empty;
  m4JournalReleaseSetCount(empty, 0);
  m4JournalReleaseActivate(empty);
  assert(empty.page == M4JournalReleasePage::List);
  assert(!empty.releaseRequested);
  return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='m4-retain-ui-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(
            ['c++', '-std=c++17', '-DCROSSPOINT_MURPHY_M4=1', f'-I{SRC}', str(cpp), '-o', str(exe)],
            check=True)
        subprocess.run([str(exe)], check=True)
    print('retain explicit ui state: PASS')


def test_archive_bytes_and_same_id():
    script = ROOT / 'firmware' / 'tests' / 'test_m4_astra_final_p1.py'
    subprocess.run(['python3', str(script)], check=True, cwd=ROOT)


if __name__ == '__main__':
    test_source_contract()
    test_ui_state()
    test_archive_bytes_and_same_id()
