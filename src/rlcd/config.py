"""User-owned settings and materials live in ~/.config/rlcd."""
import json
import os
from pathlib import Path
import shutil
import tempfile
from urllib.parse import urlsplit


def home():
    return Path.home() / '.config' / 'rlcd'


def read():
    path = home() / 'config.json'
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(data, dict) or any(k not in ('device', 'font') or not isinstance(v, str) for k,v in data.items()):
        raise ValueError('配置格式无效：' + str(path))
    return data


def set_value(key, value):
    if key == 'device':
        parts = urlsplit(value)
        if parts.scheme != 'http' or not parts.hostname or parts.username or parts.password or parts.query or parts.fragment or parts.path not in ('','/'):
            raise ValueError('device 必须为 http://IP[:PORT]，不能包含路径或凭据')
        value = value.rstrip('/')
    elif key == 'font':
        value = str(Path(value).expanduser().resolve())
        if not Path(value).is_file():
            raise ValueError('字体文件不存在')
    else:
        raise ValueError('未知配置项')
    values = read(); values[key] = value
    home().mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=home(), prefix='.config-', suffix='.json')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as output:
            json.dump(values, output, ensure_ascii=False, indent=2)
            output.write('\n')
        os.replace(temporary, home()/'config.json')
    finally:
        Path(temporary).unlink(missing_ok=True)


def seed_pets():
    # Wheel data lives beside this module; source checkouts keep materials at repo root.
    bundled = Path(__file__).parent/'bundled_pets'
    if not bundled.is_dir():
        bundled = Path(__file__).resolve().parents[2]/'pets'
    destination = home()/'pets'
    destination.mkdir(parents=True, exist_ok=True)
    for source in sorted(bundled.iterdir()):
        if not source.is_dir() or not (source/'pet.json').is_file():
            continue
        target = destination/source.name
        if target.exists():
            continue  # Never overwrite user edits during upgrades.
        staging = Path(tempfile.mkdtemp(dir=destination, prefix='.seed-'))
        try:
            shutil.copytree(source, staging, dirs_exist_ok=True)
            try:
                staging.rename(target)
            except FileExistsError:
                pass
        finally:
            if staging.exists(): shutil.rmtree(staging)
    docs = Path(__file__).parent/'bundled_docs'
    if not docs.is_dir():
        docs = Path(__file__).resolve().parents[2]/'docs'
    target_docs = home()/'docs'
    target_docs.mkdir(exist_ok=True)
    for source in docs.glob('*.md'):
        target = target_docs/source.name
        if not target.exists(): shutil.copy2(source, target)
    return destination


def pet_folder(value):
    from .pet_assets import identifier
    directory = seed_pets()
    # A bare ID is always resolved from the user's library, independent of cwd.
    if identifier(value):
        candidate = directory/value
        if candidate.is_dir(): return candidate
    candidate = Path(value).expanduser().resolve()
    if not candidate.is_dir(): raise ValueError('找不到宠物素材：' + value)
    return candidate
