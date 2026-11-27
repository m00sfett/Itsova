"""Exercise migration and conflict handling with an isolated XDG profile."""

import runpy
from pathlib import Path
import tempfile

migrate = runpy.run_path(
    Path(__file__).resolve().parents[1] / "scripts/migrate-from-glimply.py"
)["migrate"]

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    config, data, state = (root / name for name in ("config", "data", "state"))
    widget = "io.github.m00sfett.glimply.widget"
    fixtures = {
        config / "glimplyrc": b"[General]\nAutostart=false\nOutputDirectory=/old/captures\n",
        config / "kglobalshortcutsrc": b"[glimply]\n_k_friendly_name=glimply\nprint=Print,none,Capture\n"
        b"[spectacle]\nFullScreen=none,Print,Full Screen\n",
        config / "plasma-org.kde.plasma.desktop-appletsrc": f"plugin={widget}\nknownItems=other,{widget}\n".encode(),
        config / "autostart/io.github.m00sfett.glimply.desktop": b"Exec=/old/glimply\n",
        state / "glimply/history.json": b'[{"path":"/old/captures/screenshot.png"}]',
        data / "glimply/pins/pins.json": b'{"pins":[]}',
        data / "glimply/pins/pin.png": b"image bytes",
    }
    for path, value in fixtures.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)

    migrate(config, data, state, False)
    assert all(path.read_bytes() == value for path, value in fixtures.items())
    assert not (config / "itsovarc").exists()

    migrate(config, data, state, True)
    migrate(config, data, state, True)
    assert (config / "itsovarc").read_bytes() == fixtures[config / "glimplyrc"]
    assert (data / "itsova/pins/pin.png").read_bytes() == b"image bytes"
    assert (state / "itsova/history.json").read_bytes() == fixtures[state / "glimply/history.json"]
    shortcuts = (config / "kglobalshortcutsrc").read_text()
    assert "[itsova]\n_k_friendly_name=Itsova\nprint=Print,none,Capture\n" in shortcuts
    assert "[spectacle]\nFullScreen=none,Print,Full Screen\n" in shortcuts
    assert "[glimply]" not in shortcuts
    plasma = config / "plasma-org.kde.plasma.desktop-appletsrc"
    assert plasma.read_text() == "plugin=io.github.m00sfett.itsova.widget\nknownItems=other,io.github.m00sfett.itsova.widget\n"
    for path in (config / "kglobalshortcutsrc", plasma, config / "autostart/io.github.m00sfett.glimply.desktop"):
        assert path.with_name(path.name + ".pre-itsova").read_bytes() == fixtures[path]
    assert not (config / "autostart/io.github.m00sfett.glimply.desktop").exists()

    (config / "itsovarc").write_bytes(b"new settings")
    try:
        migrate(config, data, state, True)
    except ValueError:
        pass
    else:
        raise AssertionError("An existing Itsova profile was not detected")
    assert (config / "itsovarc").read_bytes() == b"new settings"
