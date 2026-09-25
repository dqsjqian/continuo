#!/usr/bin/env python3
"""显式构建固定版本的 H2/H3 静态依赖；不由项目 CMake 自动调用。

支持 Linux/macOS、Python 3.9+、CMake 和本机 C/C++ 工具链；Windows 未验证，
因此拒绝运行。另需预先安装提供 SSL_set_quic_tls_cbs 的 OpenSSL 3.5+。
仅固定这三个依赖的源码，不保证不同编译器/OpenSSL 下产物逐字节一致。
默认所有写入均位于仓库 build/protocol-deps，不安装或修改系统 OpenSSL。
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


REPO = Path(__file__).resolve().parents[2]
# 官方 GitHub release asset 的 SHA256，亦与本地归档逐字节核验；许可证均为 MIT。
# 每份发行归档根目录的 COPYING 是许可证原文。
DEPENDENCIES = (
    ("nghttp2", "1.70.0", "nghttp2/nghttp2",
     "e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2"),
    ("nghttp3", "1.15.0", "ngtcp2/nghttp3",
     "6da0cd06b428d32a54c58137838505d9dc0371a900bb8070a46b29e1ceaf2e0f"),
    ("ngtcp2", "1.22.1", "ngtcp2/ngtcp2",
     "dfd2c68bd64b89847c611425b9487105c46e8447b5c21e6aeb00642c8fbe2ca8"),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(cache: Path, name: str, version: str, project: str,
             expected: str, offline: bool) -> Path:
    filename = f"{name}-{version}.tar.xz"
    archive = cache / filename
    if archive.exists():
        if not archive.is_file() or archive.is_symlink() or sha256(archive) != expected:
            raise ValueError(f"缓存 SHA256 校验失败，未覆盖文件：{archive}")
        print(f"复用已校验缓存：{filename}", flush=True)
        return archive
    if offline:
        raise ValueError(f"离线缓存缺失：{archive}")
    url = f"https://github.com/{project}/releases/download/v{version}/{filename}"
    print(f"下载：{url}\nSHA256：{expected}", flush=True)
    # 下载中断或校验失败只删除本次临时文件，不覆盖已有归档。
    with tempfile.TemporaryDirectory(prefix="download-", dir=cache) as temporary:
        candidate = Path(temporary) / filename
        request = urllib.request.Request(url, headers={"User-Agent": "continuo-protocol-deps"})
        with urllib.request.urlopen(request, timeout=60) as response, candidate.open("wb") as output:
            shutil.copyfileobj(response, output)
        if sha256(candidate) != expected:
            raise ValueError(f"下载 SHA256 校验失败：{url}")
        candidate.replace(archive)
    return archive


def extract(archive: Path, destination: Path, root_name: str) -> Path:
    # destination 是本次运行独占的空目录；先验证全部成员，再写入普通文件。
    with tarfile.open(archive, "r:xz") as package:
        members = package.getmembers()
        for member in members:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or not path.parts or path.parts[0] != root_name
                    or not (member.isdir() or member.isfile())):
                raise ValueError(f"拒绝不安全归档成员：{member.name}")
            target = (destination / member.name).resolve()
            if destination.resolve() not in target.parents:
                raise ValueError(f"拒绝路径越界：{member.name}")
        for member in members:
            target = destination / member.name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            source = package.extractfile(member)
            if source is None:
                raise ValueError(f"无法读取归档成员：{member.name}")
            with source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o755 if member.mode & 0o111 else 0o644)
    return destination / root_name


def positive_jobs(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("--jobs 必须是 1 到 256 的整数") from error
    if not 1 <= number <= 256:
        raise argparse.ArgumentTypeError("--jobs 必须是 1 到 256 的整数")
    return number


def output_path(value: Path) -> Path:
    path = value.expanduser().resolve()
    if path in (Path.home(), REPO) or path in REPO.parents:
        raise ValueError(f"拒绝以主目录或仓库根目录作为输出目录：{path}")
    for system in ("/usr", "/bin", "/sbin", "/etc", "/System", "/Library", "/opt"):
        root = Path(system)
        if path == root or root in path.parents:
            raise ValueError(f"拒绝向系统目录安装：{path}")
    if path.exists() and not path.is_dir():
        raise ValueError(f"输出路径不是目录：{path}")
    if ";" in str(path):
        raise ValueError("输出路径不能包含 CMake 列表分隔符 ';'")
    return path


def run(command: list[str]) -> None:
    print("+ " + shlex.join(command), flush=True)
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", type=Path, default=REPO / "build/protocol-deps",
                        help="缓存和临时构建根目录，默认仓库 build/protocol-deps")
    parser.add_argument("--prefix", type=Path,
                        help="安装目录，默认 <path>/prefix；不得指向系统目录")
    parser.add_argument("--openssl-root", type=Path,
                        help="已安装的 OpenSSL 3.5+ 根目录；省略则由 CMake 查找")
    parser.add_argument("--jobs", type=positive_jobs,
                        default=min(os.cpu_count() or 1, 8), help="并行任务数，1 到 256")
    parser.add_argument("--offline", action="store_true",
                        help="禁止下载，只使用 <path>/cache 中通过哈希校验的归档")
    args = parser.parse_args()
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        parser.error("仅支持 Linux/macOS；Windows 尚未验证")
    if shutil.which("cmake") is None:
        parser.error("请先安装 CMake 和 C/C++ 工具链")
    work = output_path(args.path)
    prefix = output_path(args.prefix or work / "prefix")
    openssl = args.openssl_root.expanduser().resolve() if args.openssl_root else None
    if openssl and (";" in str(openssl) or not (openssl / "include/openssl/ssl.h").is_file()):
        parser.error("--openssl-root 必须包含 include/openssl/ssl.h，且路径不能包含 ';'")
    cache = work / "cache"
    if prefix == cache or cache in prefix.parents or prefix in cache.parents:
        parser.error("--prefix 不得与归档缓存目录重叠")
    cache.mkdir(parents=True, exist_ok=True)
    archives = [(name, version, download(cache, name, version, project, digest, args.offline))
                for name, version, project, digest in DEPENDENCIES]
    common = [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
              "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
              "-DENABLE_LIB_ONLY=ON", "-DBUILD_TESTING=OFF"]
    if openssl:
        common.append(f"-DOPENSSL_ROOT_DIR={openssl}")
    # 每次从固定归档重新解压和构建，避免复用被修改的源码或旧 CMakeCache。
    with tempfile.TemporaryDirectory(prefix="build-", dir=work) as temporary:
        staging = Path(temporary)
        for name, version, archive in archives:
            source = extract(archive, staging, f"{name}-{version}")
            build = staging / f"{name}-build"
            options = (["-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON",
                        "-DENABLE_DOC=OFF", "-DENABLE_FAILMALLOC=OFF"] if name == "nghttp2"
                       else ["-DENABLE_SHARED_LIB=OFF", "-DENABLE_STATIC_LIB=ON"])
            if name == "ngtcp2":
                options += ["-DENABLE_OPENSSL=ON", "-DENABLE_GNUTLS=OFF",
                            "-DENABLE_BORINGSSL=OFF", "-DENABLE_PICOTLS=OFF",
                            "-DENABLE_WOLFSSL=OFF"]
            run(["cmake", "-S", str(source), "-B", str(build), *common, *options])
            if name == "ngtcp2":
                configuration = (build / "CMakeCache.txt").read_text()
                if "HAVE_SSL_SET_QUIC_TLS_CBS:INTERNAL=1" not in configuration:
                    raise ValueError("ngtcp2 ossl 后端需要 OpenSSL 3.5+ 的 SSL_set_quic_tls_cbs")
            run(["cmake", "--build", str(build), "--parallel", str(args.jobs)])
            run(["cmake", "--install", str(build)])
            license_dir = prefix / "share/licenses" / name
            license_dir.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / "COPYING", license_dir / "COPYING")
    for library in ("nghttp2", "nghttp3", "ngtcp2", "ngtcp2_crypto_ossl"):
        artifact = prefix / "lib" / f"lib{library}.a"
        if not artifact.is_file():
            raise ValueError(f"未生成预期静态库：{artifact}")
    print(f"完成。静态库、头文件及 MIT 许可证位于：{prefix}", flush=True)
    print(f"项目配置时显式传入 -DCMAKE_PREFIX_PATH={shlex.quote(str(prefix))}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"错误：{error}", file=sys.stderr)
        sys.exit(1)
