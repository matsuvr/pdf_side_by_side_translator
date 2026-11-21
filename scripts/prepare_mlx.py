#!/usr/bin/env python3
"""Developer-only preparation of pinned MLX sources and the official 4bit model."""
import argparse
from pathlib import Path
import zipfile

from common import ROOT, WORK, checkout, download, lock


def prepare_dependencies():
    deps = lock()['dependencies']
    for name in ('mlx', 'json', 'fmt'):
        checkout(deps[name], WORK / 'src' / name)
    metal = deps['metal-cpp']
    archive = WORK / 'downloads/metal-cpp_26.zip'
    download(metal['url'], archive, metal['sha256'])
    with zipfile.ZipFile(archive) as source:
        source.extractall(WORK / 'src')


def prepare_model(directory):
    model = lock()['mlx_model_fixture']
    base = f"https://huggingface.co/{model['repository']}/resolve/{model['revision']}/"
    for name, digest in model['files'].items():
        download(base + name, directory / name, digest)
    print('MLX model:', directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--dependencies-only', action='store_true')
    mode.add_argument('--model-only', action='store_true')
    parser.add_argument('--model-dir', type=Path,
                        help='Destination; default: next to the release .app')
    args = parser.parse_args()
    if not args.model_only:
        prepare_dependencies()
    if not args.dependencies_only:
        model = lock()['mlx_model_fixture']
        directory = args.model_dir or ROOT / 'build/macos-arm64-release' / model['directory']
        prepare_model(directory)


if __name__ == '__main__':
    main()
