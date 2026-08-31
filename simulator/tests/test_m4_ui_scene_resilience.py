from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_home_snapshot_render_boundary_is_backend_free():
    source = (ROOT / "firmware/src/activities/home/HomeActivity.cpp").read_text()
    render = source.split("void HomeActivity::renderSnapshotScene()", 1)[1].split(
        "#endif", 1
    )[0]
    handle = source.split("void HomeActivity::handleSnapshotInput()", 1)[1].split(
        "void HomeActivity::renderSnapshotScene()", 1
    )[0]
    forbidden = ("SdMan", "FsFile", "M4xRegistry", "Epub", "Xtc", "Txt", "powerManager")
    assert "GfxSceneRenderer" in render
    assert "UiSceneRuntime::hitTestScene" in handle
    for token in forbidden:
        assert token not in render
        assert token not in handle


def test_home_keeps_non_murphy_path_separate():
    source = (ROOT / "firmware/src/activities/home/HomeActivity.cpp").read_text()
    header = (ROOT / "firmware/src/activities/home/HomeActivity.h").read_text()
    assert "#ifdef CROSSPOINT_MURPHY_M4" in source
    assert "#else" in source
    assert "loadRecentBooks(metrics.homeRecentBooksCount)" in source
    assert "UiScene::UiSceneActionQueue sceneActionQueue" in header
    assert "UiScene::UiSceneActionDispatcher sceneActionDispatcher" in header
    assert "pendingSceneAction" not in header


def test_home_backend_has_stack_budget_for_sd_and_registry_loading():
    source = (ROOT / "firmware/src/activities/home/HomeActivity.cpp").read_text()
    assert "constexpr uint32_t kHomeSceneBackendStackBytes = 16 * 1024" in source
    backend_create = source.split('"HomeSceneBackend"', 1)[1].split(
        "sceneBackendTaskHandle", 1
    )[0]
    assert "kHomeSceneBackendStackBytes" in backend_create
