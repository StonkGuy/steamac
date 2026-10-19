#!/usr/bin/env bash
# Generate the app's complete host and guest third-party license inventory from the
# pinned source trees, binary Go build metadata and target-specific Cargo graphs.
# Rebuild work/out/licenses atomically; Go downloads are cached outside the bundle.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export STEAMAC_ROOT="$ROOT"
python3 - <<'PY'
import atexit
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(os.environ['STEAMAC_ROOT'])
out = root / 'work/out/licenses'
cache = root / 'work/cache/licenses/go-modcache'
cache.mkdir(parents=True, exist_ok=True)
stage = Path(tempfile.mkdtemp(prefix='.licenses.', dir=out.parent))
atexit.register(lambda: shutil.rmtree(stage) if stage.exists() else None)
entries = []
counts = {}

def run(*args, env=None, cwd=None):
    return subprocess.check_output(args, text=True, env=env, cwd=cwd).strip()

def require(path):
    if not path.is_file():
        raise RuntimeError(f'missing license input: {path}')
    return path
def pinned_text(filename, url):
    path = root / 'work/cache/licenses' / filename
    if not path.is_file():
        path.write_bytes(subprocess.check_output(['curl', '-fsSL', '--retry', '3', url]))
    return require(path).read_bytes()


def spdx(text):
    lower = text.lower()
    if 'cc0 1.0 universal' in lower:
        return 'CC0-1.0'
    if 'gnu general public license' in lower:
        return 'GPL-3.0-or-later' if 'version 3' in lower else 'GPL-2.0-only'
    if 'apache license' in lower or 'apache software license' in lower:
        return 'Apache-2.0'
    if 'redistribution and use in source and binary forms' in lower:
        return 'BSD-3-Clause' if 'neither the name' in lower or 'names of its contributors' in lower else 'BSD-2-Clause'
    if 'permission is hereby granted, free of charge' in lower:
        return 'MIT'
    if 'mozilla public license' in lower:
        return 'MPL-2.0'
    if 'isc license' in lower or 'permission to use, copy, modify, and/or distribute' in lower:
        return 'ISC'
    return 'LicenseRef-See-Included-Text'

def add(name, version, license_id, location, url, documents):
    # documents: sequence of (relative output filename, bytes). Missing notices are fatal.
    if not documents or any(not data or not data.strip() for _, data in documents):
        raise RuntimeError(f'{name} {version}: missing or empty license text')
    dest = stage / name
    dest.mkdir(parents=True)
    for filename, data in documents:
        target = dest / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    entries.append((name, version, license_id or 'LicenseRef-See-Included-Text', location, url))

def top_docs(path):
    return [(f.name, f.read_bytes()) for f in sorted(path.iterdir())
            if f.is_file() and re.match(r'(?i)^(LICENSE|LICENCE|COPYING|NOTICE)([.\-_].*)?$', f.name)]

def source(name, version, license_id, location, url, path, extra=()):
    docs = top_docs(path)
    docs += [(str(p), require(path / p).read_bytes()) for p in extra]
    add(name, version, license_id or spdx(b'\n'.join(data for _, data in docs).decode('utf-8', 'replace')), location, url, docs)

def archive(name, version, license_id, location, url, filename, prefixes):
    docs = []
    with tarfile.open(require(filename), 'r:*') as tar:
        for member in tar:
            parts = Path(member.name).parts
            if not member.isfile() or len(parts) < 2:
                continue
            relative = '/'.join(parts[1:])
            if any(relative == p or relative.startswith(p + '/') for p in prefixes):
                data = tar.extractfile(member).read()
                if data:
                    docs.append((relative, data))
    add(name, version, license_id, location, url, docs)

host = 'Contents/Frameworks'
res = 'Contents/Resources'
launcher = 'Contents/MacOS/steamac-vm'
krun = root / 'work/build/host-libkrun/src'
source('libkrun', '1.19.6', 'Apache-2.0', host + '/libkrun.1.dylib',
       'https://github.com/libkrun/libkrun/tree/v1.19.6', krun)
source('libkrun-rutabaga-gfx', '1.19.6', 'BSD-3-Clause', host + '/libkrun.1.dylib',
       'https://github.com/google/crosvm/tree/main/rutabaga_gfx',
       krun / 'src/rutabaga_gfx', ('ffi/LICENSE',))
source('virglrenderer', '5d26f605+aafa9bd2', 'MIT', host + '/libvirglrenderer.1.dylib',
       'https://github.com/utmapp/virglrenderer', root / 'work/build/host-virglrenderer/src')
mvk = root / 'work/build/host-moltenvk/src'
source('MoltenVK', '05604465', 'Apache-2.0', host + '/libMoltenVK.dylib',
       'https://github.com/utmapp/MoltenVK', mvk)
revisions = mvk / 'ExternalRevisions'
external = mvk / 'External'
for name in ('SPIRV-Cross', 'Vulkan-Headers', 'SPIRV-Tools', 'cereal', 'Volk'):
    path = root / 'work/build/host-moltenvk/SPIRV-Cross' if name == 'SPIRV-Cross' else external / name
    version = require(revisions / (name + '_repo_revision')).read_text().strip()
    extras = [str(p.relative_to(path)) for p in (path / 'LICENSES').rglob('*') if p.is_file()]
    source('MoltenVK-' + name, version, 'Apache-2.0 OR MIT' if name == 'Vulkan-Headers' else None,
           host + '/libMoltenVK.dylib',
           'https://github.com/' + ('utmapp' if name == 'SPIRV-Cross' else 'KhronosGroup' if name != 'cereal' else 'USCiLab') + '/' + name,
           path, extras)
# SPIRV-Headers is checked out within SPIRV-Tools rather than External's top level.
spv_headers = external / 'SPIRV-Tools/external/spirv-headers'
source('MoltenVK-SPIRV-Headers', require(revisions / 'SPIRV-Headers_repo_revision').read_text().strip(),
       'Apache-2.0', host + '/libMoltenVK.dylib', 'https://github.com/KhronosGroup/SPIRV-Headers', spv_headers,
       [str(p.relative_to(spv_headers)) for p in (spv_headers / 'LICENSES').rglob('*') if p.is_file()])
# Vulkan-Tools is fetched by MoltenVK for SDK tooling, not linked into the dylib.
# KosmicKrisp (Mesa), bundled when host/kosmickrisp built it (macOS 26+ build hosts).
if (root / 'work/out/host/lib/libvulkan_kosmickrisp.dylib').is_file():
    mesa = root / 'work/build/host-kosmickrisp/src'
    mesa_commit = re.search(r'^MESA_COMMIT=([0-9a-f]{40})$', (root / 'host/kosmickrisp/build.sh').read_text(), re.M)
    if not mesa_commit:
        raise RuntimeError('missing pinned KosmicKrisp Mesa commit')
    source('KosmicKrisp-Mesa', mesa_commit.group(1)[:10], 'MIT', host + '/libvulkan_kosmickrisp.dylib',
           'https://gitlab.freedesktop.org/mesa/mesa/-/tree/' + mesa_commit.group(1), mesa,
           ['docs/license.rst'] + [str(p.relative_to(mesa)) for p in sorted((mesa / 'licenses').rglob('*')) if p.is_file()])
# The DXMT graphics path (libwmtr_host.dylib, bundled when wmtr/ built it): DXMT with its airconv shader
# compiler, the statically linked LLVM, and wmtr itself.
if (root / 'work/out/host/lib/libwmtr_host.dylib').is_file():
    pin = dict(re.findall(r'^([A-Z][A-Z0-9_]*)=(\S+)$', require(root / 'host/dxmt/pin.env').read_text(), re.M))
    wmtr_lib = host + '/libwmtr_host.dylib'
    dxmt_tar = root / 'work/cache/dxmt' / ('dxmt-' + pin['DXMT_COMMIT'] + '.tar.gz')
    if not dxmt_tar.is_file():
        dxmt_tar.parent.mkdir(parents=True, exist_ok=True)
        dxmt_tar.write_bytes(subprocess.check_output(['curl', '-fsSL', '--retry', '3', pin['DXMT_URL']]))
    archive('DXMT', pin['DXMT_COMMIT'][:10], 'LGPL-2.1-or-later', wmtr_lib,
            pin['DXMT_REPO'] + '/tree/' + pin['DXMT_COMMIT'], dxmt_tar, ('COPYING.LIB', 'LICENSE', 'LICENSE.OLD'))
    add('LLVM', pin['LLVM_VERSION'], 'Apache-2.0 WITH LLVM-exception', wmtr_lib + ' (static, airconv)',
        'https://github.com/llvm/llvm-project/tree/llvmorg-' + pin['LLVM_VERSION'],
        [('LICENSE.TXT', require(root / 'LICENSES/LLVM-15-Apache-2.0-WITH-LLVM-exception.txt').read_bytes())])
    add('wmtr', '2026', 'BSD-3-Clause', wmtr_lib, 'https://github.com/StonkGuy/steamac/tree/main/wmtr',
        [('LICENSE', require(root / 'LICENSES/BSD-3-Clause.txt').read_bytes())])
# The experimental VR host (Contents/MacOS/steamac-vrhost, bundled when host/vrhost/build.sh built it): a separate
# GPL-3.0-or-later executable (WiVRn code) with a static libcrypto and header-only build dependencies.
vrhost_bin = root / 'work/out/host/bin/steamac-vrhost'
if vrhost_bin.is_file():
    vrhost = 'Contents/MacOS/steamac-vrhost'
    vr_info = dict(re.findall(r'^([a-z ]+): (.*)$', require(root / 'work/out/host/VRHOST.txt').read_text(), re.M))
    vr_build = Path(vr_info['build dir'])
    add('steamac-vrhost', '2026', 'GPL-3.0-or-later', vrhost,
        'https://github.com/StonkGuy/steamac/tree/main/host/vrhost (corresponding source: the GPL source archive)',
        [('COPYING', require(root / 'host/vrhost/COPYING').read_bytes())])
    add('WiVRn', 'v26.9', 'GPL-3.0-or-later', vrhost + ' (vendored, host/vrhost/third_party/wivrn)',
        'https://github.com/WiVRn/WiVRn/tree/v26.9', [('COPYING', require(root / 'host/vrhost/COPYING').read_bytes())])
    magic = require(root / 'host/vrhost/third_party/wivrn/external/magic_enum.hpp').read_text().split('\n')
    add('magic_enum', '0.9.5', 'MIT', vrhost + ' (header-only, vendored by WiVRn)',
        'https://github.com/Neargye/magic_enum/tree/v0.9.5',
        [('LICENSE', '\n'.join(line[3:] for line in magic[:30] if line.startswith('//')).encode())])
    add('OpenSSL-libcrypto', vr_info['openssl'], 'Apache-2.0', vrhost + ' (static)',
        'https://github.com/openssl/openssl/tree/openssl-' + vr_info['openssl'],
        [('LICENSE.txt', require(vr_build / 'openssl_static-prefix/src/openssl_static/LICENSE.txt').read_bytes())])
    add('Boost.PFR', 'boost-1.89.0', 'BSL-1.0', vrhost + ' (header-only)',
        'https://github.com/boostorg/pfr/tree/boost-1.89.0',
        [('LICENSE_1_0.txt', require(vr_build / '_deps/boost_pfr-src/LICENSE_1_0.txt').read_bytes())])
    add('OpenXR-SDK-headers', 'release-1.1.58', 'Apache-2.0 OR MIT', vrhost + ' (headers only)',
        'https://github.com/KhronosGroup/OpenXR-SDK/tree/release-1.1.58',
        [('LICENSE', require(vr_build / '_deps/openxr_sdk-src/LICENSE').read_bytes())])
epoxy_script = (root / 'host/libepoxy/build.sh').read_text()
epoxy_version = re.search(r'^VERSION=([0-9.]+)$', epoxy_script, re.M)
epoxy_url = re.search(r'^URL=(https://\S+)$', epoxy_script, re.M)
if not epoxy_version or not epoxy_url:
    raise RuntimeError('missing pinned libepoxy version or source URL')
source('libepoxy', epoxy_version.group(1), 'MIT', host + '/libepoxy.0.dylib',
       epoxy_url.group(1), root / 'work/build/host-libepoxy/src')
sentry = root / 'host/launcher/.build/checkouts/sentry-cocoa'
source('sentry-cocoa', '9.30.0', None, launcher,
       'https://github.com/getsentry/sentry-cocoa/tree/9.30.0', sentry)
zstd = root / 'host/launcher/Sources/CZstd/zstd'
zstd_version = require(zstd / 'VERSION').read_text().split()[0]
source('zstd', zstd_version, 'BSD-3-Clause', launcher,
       'https://github.com/facebook/zstd/tree/v' + zstd_version, zstd)

# Only resolved target-specific packages count; build dependencies and proc macros are
# included since their license conditions may apply to generated/embedded code.
def rust_graph(manifest, features, target, prefix, location):
    env = dict(os.environ, CARGO_HOME=str(root / 'work/cache/cargo-home')) if prefix == 'progress-agent-crate-' else None
    metadata = json.loads(run('cargo', 'metadata', '--locked', '--offline', '--format-version', '1',
                              '--manifest-path', str(manifest), '--filter-platform', target,
                              *(['--features', features] if features else []), env=env))
    resolved = {node['id'] for node in metadata['resolve']['nodes']}
    packages = [p for p in metadata['packages'] if p['id'] in resolved and (p.get('source') or '').startswith('registry+')]
    for pkg in sorted(packages, key=lambda p: (p['name'], p['version'])):
        path = Path(pkg['manifest_path']).parent
        source(prefix + pkg['name'] + '-' + pkg['version'], pkg['version'], pkg.get('license'), location,
               pkg['homepage'] or pkg['repository'] or f"https://crates.io/crates/{pkg['name']}/{pkg['version']}", path)
    counts[prefix] = len(packages)
    print(f'{prefix}: {len(packages)} registry crates / {len(packages)} resolved registry crates')

rust_graph(krun / 'Cargo.toml', 'blk,net,gpu,input,snd', 'aarch64-apple-darwin', 'libkrun-crate-', host + '/libkrun.1.dylib')
rust_graph(root / 'guest/progress-agent/Cargo.toml', '', 'aarch64-unknown-linux-musl',
           'progress-agent-crate-', res + '/steamac-layer.img:/usr/bin/fx-progress-agent')

# go version -m describes the exact binary (including versions other than the current
# module graph). Download each module to an isolated, persistent cache, never a guess.
goenv = dict(os.environ, GOMODCACHE=str(cache), GOWORK='off')
for binary in ('gvproxy', 'desync'):
    location = res + '/' + binary
    lines = run('go', 'version', '-m', str(root / 'work/out/host/bin' / binary)).splitlines()
    go_version = lines[0].split()[-1]
    if not re.fullmatch(r'go[0-9]+\.[0-9]+(?:\.[0-9]+)?', go_version):
        raise RuntimeError(f'{binary}: unknown embedded Go toolchain version: {go_version}')
    add('go-runtime-' + binary, go_version, 'BSD-3-Clause', location,
        'https://github.com/golang/go/tree/' + go_version,
        [('LICENSE', pinned_text('go-' + go_version + '-LICENSE',
          'https://raw.githubusercontent.com/golang/go/' + go_version + '/LICENSE'))])
    modules = [line.strip().split() for line in lines if line.strip().startswith(('mod\t', 'dep\t'))]
    for kind, module, version, *remainder in modules:
        info = json.loads(run('go', 'mod', 'download', '-json', module + '@' + version, env=goenv, cwd=root))
        if info.get('Error'):
            raise RuntimeError(f'{module}@{version}: {info["Error"]}')
        path = Path(info['Dir'])
        docs = top_docs(path)
        # Some Go submodules inherit the parent repository's license. Go modules
        # omit parent files from their zip, so fetch the repository at the same
        # revision using the module source origin when there is no root license.
        if not docs:
            candidate = path.parent / 'LICENSE'
            if candidate.is_file():
                docs = [('LICENSE', candidate.read_bytes())]
        add('go-' + binary + '-' + module.replace('/', '__') + '-' + version,
            version, spdx(b'\n'.join(data for _, data in docs).decode('utf-8', 'replace')),
            location, (info.get('Origin') or {}).get('URL', 'https://' + module) +
            '/tree/' + (info.get('Origin') or {}).get('Hash', version), docs)
    counts[binary] = len(modules)
    print(f'{binary}: {len(modules)} modules / {len(modules)} go version -m modules')

config = {}
for line in (root / 'scripts/config.env').read_text().splitlines():
    match = re.match(r'^([A-Z][A-Z0-9_]*)=(.*)$', line)
    if match:
        config[match[1]] = match[2]
host_rust = re.search(r'^RUST_TOOLCHAIN=\$\{RUST_TOOLCHAIN:-([0-9.]+)\}',
                      (root / 'host/libkrun/build.sh').read_text(), re.M)
if not host_rust:
    raise RuntimeError('missing pinned host libkrun Rust toolchain')
# The guest Rust version and the builder's busybox-static come from the build containers. Without
# Docker (the app assembled on another machine than the one that built the guest images) they are
# read from the record scripts/guest-toolchain.sh wrote there; it must name the same pinned images.
if shutil.which('docker'):
    guest = None
    guest_rust = run('docker', 'run', '--rm', '--platform', 'linux/arm64',
                     config['RUST_IMAGE'], 'rustc', '--version').split()[1]
else:
    guest = json.loads(require(root / 'work/out/guest-toolchain.json').read_text())
    if (guest.get('version') != 1 or guest.get('rust_image') != config['RUST_IMAGE']
            or guest.get('builder_image') != config['BUILDER_IMAGE']):
        raise RuntimeError('work/out/guest-toolchain.json does not describe the images pinned in '
                           'scripts/config.env: rerun scripts/guest-toolchain.sh where the guest was built')
    guest_rust = guest['rust_version']
for kind, version, location in (
    ('host', host_rust.group(1), host + '/libkrun.1.dylib'),
    ('guest', guest_rust, res + '/steamac-layer.img:/usr/bin/fx-progress-agent')):
    url = 'https://github.com/rust-lang/rust/tree/' + version
    add('rust-runtime-' + kind, version, 'Apache-2.0 OR MIT', location, url,
        [(license, pinned_text('rust-' + version + '-' + license,
          'https://raw.githubusercontent.com/rust-lang/rust/' + version + '/' + license))
         for license in ('LICENSE-APACHE', 'LICENSE-MIT')])
image = res + '/Image'
init = res + '/initramfs.cpio.gz'
layer = res + '/steamac-layer.img'
kernel_version = '7.2.9'
archive('linux-kernel', kernel_version, 'GPL-2.0-only WITH Linux-syscall-note', image,
        'https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-' + kernel_version + '.tar.xz',
        root / f'work/cache/kernel/linux-{kernel_version}.tar.xz', ('COPYING', 'LICENSES/exceptions/Linux-syscall-note', 'LICENSES/preferred/GPL-2.0'))
# busybox is the Debian snapshot's busybox-static; retrieve the matching installed
# package copyright instead of pretending a GPL text from another release is sufficient.
builder = config['BUILDER_IMAGE']
if guest:
    builder_hash, busybox_version = guest['builder_id'], guest['busybox_version']
else:
    builder_hash = run('docker', 'image', 'inspect', '-f', '{{.Id}}', builder).split(':')[-1][:16]
    busybox_version = run('docker', 'run', '--rm', '--platform', 'linux/arm64', builder,
                          'dpkg-query', '-W', '-f=${Version}', 'busybox-static')
busybox_copyright = root / 'work/cache/licenses' / ('busybox-' + builder_hash + '-copyright')
if not guest and not busybox_copyright.is_file():
    data = subprocess.check_output(['docker', 'run', '--rm', '--platform', 'linux/arm64',
                                    builder, 'cat', '/usr/share/doc/busybox-static/copyright'])
    busybox_copyright.parent.mkdir(parents=True, exist_ok=True)
    busybox_copyright.write_bytes(data)
add('busybox-static', busybox_version, 'GPL-2.0-only', init + ':/bin/busybox',
    'https://sources.debian.org/src/busybox/',
    [('copyright', require(busybox_copyright).read_bytes()),
     ('GPL-2.0', require(stage / 'linux-kernel/LICENSES/preferred/GPL-2.0').read_bytes())])
archive('dosfstools', config['DOSFSTOOLS_VERSION'], 'GPL-3.0-or-later', init + ':/bin/{fsck.fat,mkfs.fat}',
        config['DOSFSTOOLS_URL'], root / f"work/cache/dosfstools/{config['DOSFSTOOLS_VERSION']}/dosfstools-{config['DOSFSTOOLS_VERSION']}.tar.gz",
        ('COPYING', 'LICENSE'))
archive('e2fsprogs', config['E2FSPROGS_VERSION'], 'GPL-2.0-only AND LGPL-2.0-only AND BSD-3-Clause AND LicenseRef-MIT-SIPB',
        init + ':/bin/mke2fs (static libext2fs/libuuid/libblkid/libcom_err)', config['E2FSPROGS_URL'],
        root / f"work/cache/e2fsprogs/{config['E2FSPROGS_VERSION']}/e2fsprogs-{config['E2FSPROGS_VERSION']}.tar.xz",
        ('NOTICE', 'debian/copyright', 'lib/uuid/COPYING'))
archive('btrfs-progs', config['BTRFSPROGS_VERSION'], 'GPL-2.0-only', init + ':/bin/btrfstune',
        config['BTRFSPROGS_URL'], root / f"work/cache/btrfs-progs/{config['BTRFSPROGS_VERSION']}/btrfs-progs-v{config['BTRFSPROGS_VERSION']}.tar.xz",
        ('COPYING', 'LICENSE'))
versions = {}
for line in (root / 'guest/mesa/versions.env').read_text().splitlines():
    match = re.match(r'^([A-Z][A-Z0-9_]*)=(.*)$', line)
    if match:
        versions[match[1]] = match[2]
add('Mesa', versions['MESA_COMMIT'], 'MIT', layer + ':/usr/lib and /usr/share/guestos/fex-mesa',
    versions['MESA_URL'], [('license.rst', pinned_text(
        'mesa-' + versions['MESA_COMMIT'] + '-license.rst',
        'https://gitlab.freedesktop.org/mesa/mesa/-/raw/' + versions['MESA_COMMIT'] + '/docs/license.rst'))])
own_docs = []
for filename in ('LICENSE', 'NOTICE'):
    path = root / filename
    if path.is_file():
        own_docs.append((filename, path.read_bytes()))
    else:
        print(f'warning: {path} not yet present; skipping', file=sys.stderr)
if own_docs:
    add('steamac', '2026', 'Apache-2.0', res + '/licenses',
        'https://github.com/fxgl/steamac', own_docs)

index = ['Third-party license notices for FX Steam Launcher',
         'Each listed directory contains the upstream license, copyright and notice files for this component.',
         'The SteamOS root filesystem acquired by the launcher is distributed separately by Valve.', '']
for name, version, license_id, location, url in sorted(entries):
    index += [f'{name} | {version} | {license_id}', f'  In bundle: {location}', f'  Source: {url}', f'  License texts: {name}/', '']
index += [f"Totals: {len(entries)} components; " + ', '.join(f'{k}{v}' for k, v in sorted(counts.items())), '']
(stage / 'THIRD-PARTY-NOTICES.txt').write_text('\n'.join(index))
old = out.with_name(out.name + '.previous')
if old.exists():
    shutil.rmtree(old)
if out.exists():
    out.rename(old)
stage.rename(out)
if old.exists():
    shutil.rmtree(old)
print(f'licenses: {len(entries)} components -> {out}')
PY
