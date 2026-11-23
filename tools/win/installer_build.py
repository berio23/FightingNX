"""Local FightingNX build steps used by the Windows installer."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from urllib.request import urlopen
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "out" / "installer"
PC = OUT / "capture"
SHADERS = OUT / "shaders"
SD_ROOT = OUT / "sdcard"
JOBS = str(min(8, os.cpu_count() or 4))
DXC = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2407/dxc_2024_07_31.zip"
DXC_SHA256 = "e2627f004f0f9424d8c71ea1314d04f38c5a5096884ae9217f1f18bd320267b5"
SPIRV_CROSS = "57e4efcc84112abcd426dc5ec248b5339bee04f2"


def validate_game(game):
    if not (game / "data").is_dir() or not (game / "databigs").is_dir():
        raise ValueError("Select the extracted game folder containing default.xex, data, and databigs.")
    try:
        with (game / "default.xex").open("rb") as xex:
            magic, _, _, _, _, count = struct.unpack(">4s5I", xex.read(24))
            if magic != b"XEX2" or count > 1024:
                raise ValueError("The selected default.xex does not have a valid XEX2 header.")
            for _ in range(count):
                kind, offset = struct.unpack(">II", xex.read(8))
                if kind == 0x00040006:
                    xex.seek(offset)
                    media, version, _, title = struct.unpack(">4I", xex.read(16))
                    if (media, version, title) == (0x2DC50868, 3, 0x45410915):
                        return
                    break
    except FileNotFoundError as error:
        raise ValueError("The selected folder is missing default.xex.") from error
    except struct.error as error:
        raise ValueError("The selected default.xex has an incomplete header.") from error
    raise ValueError("This build supports Fight Night Champion media 2DC50868, version 0.0.0.3.")


def run(*args, env=None, cwd=ROOT):
    subprocess.run([str(arg) for arg in args], cwd=cwd, env=env, check=True)


def stage(message):
    print(f"::stage::{message}", flush=True)


def build_environment(devkit):
    required = ("devkitA64/bin/aarch64-none-elf-g++.exe", "libnx/switch.specs",
                "libnx/include/deko3d.h", "libnx/lib/libdeko3d.a",
                "portlibs/switch/lib/libexpat.a", "portlibs/switch/lib/libzstd.a",
                "portlibs/switch/lib/libz.a",
                "msys2/clang64/bin/clang++.exe", "msys2/usr/bin/make.exe", "msys2/usr/bin/zstd.exe")
    missing = [file for file in required if not (devkit / file).is_file()]
    if missing:
        raise ValueError("Install the devkitPro packages in the README. Missing " + ", ".join(missing))

    env = os.environ.copy()
    env.update(DEVKITPRO=str(devkit), FNC_INSTALL_ROOT=str(ROOT), PYTHONUNBUFFERED="1")
    env["PATH"] = os.pathsep.join((str(Path(sys.executable).parent), env["PATH"]))
    clang = shutil.which("clang.exe", path=env["PATH"])
    if clang:
        env.setdefault("SA2_LLVM", str(Path(clang).parent.parent))
    if not env.get("SA2_LLVM") and not (ROOT / "tools/win/env.local.ps1").exists():
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        vswhere /= "Microsoft Visual Studio/Installer/vswhere.exe"
        if vswhere.is_file():
            vs = subprocess.check_output(
                [str(vswhere), "-latest", "-products", "*", "-requires",
                 "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                cwd=ROOT, text=True,
            ).strip()
            if vs:
                env.setdefault("SA2_VSDIR", vs)
                env["SA2_LLVM"] = str(Path(vs) / "VC/Tools/Llvm/x64")
    code = (
        "$ErrorActionPreference = 'Stop'; "
        ". (Join-Path $env:FNC_INSTALL_ROOT 'tools/win/env.ps1'); "
        "$values = @{}; Get-ChildItem Env: | ForEach-Object { $values[$_.Name] = $_.Value }; "
        "$values | ConvertTo-Json -Compress"
    )
    result = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", code],
                            cwd=ROOT, env=env, capture_output=True, text=True, errors="replace")
    if result.returncode:
        raise ValueError("Set up Visual Studio Build Tools and LLVM first. " + result.stderr.strip())
    lines = result.stdout.splitlines()
    if not lines:
        raise ValueError("The Windows build environment did not initialize.")
    env = {key.upper(): value for key, value in json.loads(lines[-1]).items()}
    env["DEVKITPRO"] = str(devkit)
    env["FNC_ZSTD"] = str(devkit / "msys2/usr/bin/zstd.exe")
    for executable in ("git", "cmake", "ninja", "clang", "clang++"):
        if not shutil.which(executable, path=env["PATH"]):
            raise ValueError(f"{executable} is missing from the Windows build environment. See the README.")
    return env


def link_game(game, env):
    assets = ROOT / "assets"
    assets.mkdir(exist_ok=True)
    linked = assets / "game_root"
    if linked.exists():
        if not linked.samefile(game):
            raise ValueError("This checkout already uses another game folder. Use a fresh checkout for a different dump.")
        return
    if os.path.lexists(linked):
        raise ValueError("assets/game_root is a broken link. Use a fresh checkout or fix the link.")
    env.update(FNC_INSTALL_GAME=str(game), FNC_INSTALL_LINK=str(linked))
    run("powershell.exe", "-NoProfile", "-Command",
        "$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction "
        "-Path $env:FNC_INSTALL_LINK -Target $env:FNC_INSTALL_GAME | Out-Null", env=env)


def install_dxc():
    folder = ROOT / "app/thirdparty/reblue-XenosRecomp/thirdparty/dxc-bin"
    files = ("bin/x64/dxc.exe", "bin/x64/dxcompiler.dll", "bin/x64/dxil.dll", "lib/x64/dxcompiler.lib")
    if all((folder / name).is_file() for name in files):
        return
    print("Downloading Microsoft's DirectX Shader Compiler", flush=True)
    with tempfile.TemporaryFile() as archive:
        with urlopen(DXC, timeout=60) as response:
            shutil.copyfileobj(response, archive)
        archive.seek(0)
        digest = hashlib.sha256()
        for chunk in iter(lambda: archive.read(65536), b""):
            digest.update(chunk)
        if digest.hexdigest() != DXC_SHA256:
            raise ValueError("The DXC archive checksum did not match.")
        archive.seek(0)
        with ZipFile(archive) as package:
            for name in (*files, "LICENSE-LLVM.txt", "LICENSE-MIT.txt", "LICENSE-MS.txt"):
                dest = folder / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                temporary = dest.with_suffix(dest.suffix + ".tmp")
                temporary.write_bytes(package.read(name))
                temporary.replace(dest)


def cmake(source, build, *options, env):
    run("cmake", "-S", source, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", *options, env=env)


def compile_target(build, *targets, env):
    options = ("--target", *targets) if targets else ()
    run("cmake", "--build", build, "--parallel", JOBS, *options, env=env)


def script(name, *args, env):
    run(sys.executable, ROOT / "tools" / name, *args, env=env)


def prepare(game, env):
    stage("Preparing build dependencies")
    if not (ROOT / "sdk/thirdparty/glslang/CMakeLists.txt").is_file():
        script("fetch_thirdparty.py", env=env)
    install_dxc()
    link_game(game, env)
    stage("Recompiling your game")
    host = OUT / "host"
    common = ("-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
              "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
    cmake(ROOT / "sdk", host, *common, env=env)
    compile_target(host, "rexglue", env=env)
    generator = ROOT / "sdk/out/win-amd64/rexglue.exe"
    generated = ROOT / "app/generated/rexglue.cmake"
    if not generated.is_file() or generated.stat().st_mtime < generator.stat().st_mtime:
        run("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            ROOT / "tools/codegen.ps1", "-Rexglue", generator, env=env)
    SHADERS.mkdir(parents=True, exist_ok=True)
    cmake(ROOT / "app", PC, *common, "-DSA2_RENUT_RENDERER=ON",
          "-DBANJO_RENUT_EARLY_ALPHA=OFF", f"-DFNC_SHADER_DUMP_DIR={SHADERS.as_posix()}",
          f"-DRENUT_SHADER_CAPTURE_DIR={SHADERS.as_posix()}", env=env)


def capture(game, env):
    prepare(game, env)
    stage("Building the Windows shader capture")
    compile_target(PC, "sa2", env=env)
    stage("Capturing shaders. Play the scenes you want, then close the game window.")
    run(PC / "sa2.exe", "--sa2_renderer=renut", "--renut_gpu_backend=d3d12",
        "--audio_mute=false", "--window_start_minimized=false", "--fnc_frame_rate=30",
        f"--game_data_root={game}", f"--renut_shader_capture_dir={SHADERS}",
        f"--user_data_root={OUT / 'capture-save'}", f"--cache_root={OUT / 'capture-cache'}",
        cwd=PC, env=env)


def host_shader_tools(devkit, env):
    tools = ROOT / "out/deko3d_tools"
    spirv_cross = tools / "spirv-cross-build"
    uam = tools / "uam-build"
    if (spirv_cross / "spirv-cross.exe").is_file() and (uam / "uam.exe").is_file():
        return
    stage("Building the shader tools")
    clang = devkit / "msys2/clang64/bin"
    compile_env = dict(env, PATH=os.pathsep.join((str(clang), env["PATH"])))
    common = (f"-DCMAKE_C_COMPILER={(clang / 'clang.exe').as_posix()}",
              f"-DCMAKE_CXX_COMPILER={(clang / 'clang++.exe').as_posix()}",
              "-DCMAKE_EXE_LINKER_FLAGS=-static", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
    if not (spirv_cross / "spirv-cross.exe").is_file():
        source = OUT / "SPIRV-Cross"
        if not (source / "CMakeLists.txt").is_file():
            source.mkdir(parents=True, exist_ok=True)
            run("git", "init", "-q", source, env=env)
            run("git", "-C", source, "fetch", "--depth=1",
                "https://github.com/KhronosGroup/SPIRV-Cross.git", SPIRV_CROSS, env=env)
            run("git", "-C", source, "checkout", "--detach", "FETCH_HEAD", env=env)
        cmake(source, spirv_cross, *common, "-DSPIRV_CROSS_ENABLE_TESTS=OFF",
              "-DSPIRV_CROSS_ENABLE_C_API=OFF", env=compile_env)
        compile_target(spirv_cross, "spirv-cross", env=compile_env)
    if not (uam / "uam.exe").is_file():
        cmake(ROOT / "app/thirdparty/uam", uam, *common, env=compile_env)
        compile_target(uam, "uam_tool", env=compile_env)


def shader_package(devkit, env):
    host_shader_tools(devkit, env)
    stage("Translating captured shaders for Switch")
    with tempfile.TemporaryDirectory(dir=OUT, prefix="shader-build-") as folder:
        folder = Path(folder)
        spirv = folder / "spirv"
        spirv.mkdir()
        shader_env = dict(env, BANJO_DEKO3D_SPIRV_DIR=str(spirv))
        compile_target(PC, "renut_xenos_recomp", env=env)
        recompiler = PC / "xenos_recomp/build/XenosRecomp/XenosRecomp.exe"
        output = PC / "renut_gpu/shader_cache.cpp"
        output.parent.mkdir(parents=True, exist_ok=True)
        run(recompiler, SHADERS, output,
            ROOT / "app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h", env=shader_env)
        failed = list(spirv.glob("*.fail"))
        if failed:
            raise ValueError(f"DXC could not translate {len(failed)} captured shaders.")
        script("renut_switch_shader_cache.py", "--pc", PC, env=env)
        script("deko3d_shaders.py", "--spirv", spirv, "--out", folder, "--jobs", JOBS, "--no-uam", env=env)
        if sum(1 for _ in spirv.glob("*.spv")) != sum(1 for _ in (folder / "glsl").glob("*.glsl")):
            raise ValueError("The shader conversion log reports shaders that could not be converted.")
        script("deko3d_glsl_opt.py", "--in", folder / "glsl", "--out", folder / "optimized", "--check", env=env)
        script("deko3d_shader_package.py", "--glsl", folder / "optimized", env=env)


def package(game, nro):
    dest = SD_ROOT / "switch/fnc-nx"
    dest.mkdir(parents=True, exist_ok=True)
    # Skip copying the disc again after recompiling the same dump.
    game_dest = dest / "game_root"
    if game_dest.resolve().is_relative_to(game):
        raise ValueError("Choose a game folder outside the install output.")
    if not game_dest.is_dir():
        with tempfile.TemporaryDirectory(dir=dest, prefix="game-copy-") as temporary:
            copied = Path(temporary) / "game_root"
            shutil.copytree(game, copied)
            copied.rename(game_dest)
    config = dest / "fnc-nx.toml"
    if not config.exists():
        shutil.copyfile(ROOT / "app/fnc-nx-switch.toml", config)
    staged = dest / "fnc-nx.nro.tmp"
    shutil.copyfile(nro, staged)
    staged.replace(dest / "fnc-nx.nro")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("capture", "build"))
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--devkitpro", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("The FightingNX installer requires Windows.")
    game, devkit = args.game.resolve(), args.devkitpro.resolve()
    try:
        validate_game(game)
        env = build_environment(devkit)
        if args.action == "capture":
            capture(game, env)
            stage(f"Captured {sum(1 for _ in SHADERS.glob('*.bin'))} shaders")
        else:
            if not any(SHADERS.glob("vs_*.bin")) or not any(SHADERS.glob("ps_*.bin")):
                raise ValueError("Capture shaders in the Windows game first.")
            prepare(game, env)
            shader_package(devkit, env)
            stage("Building the Switch app")
            run("powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                ROOT / "tools/win/build_switch.ps1", "-BuildDir", OUT / "switch",
                "-NoNvk", "-Jobs", JOBS, "-LtoJobs", JOBS, env=env)
            stage("Preparing the SD card folder")
            package(game, OUT / "switch/sa2.nro")
            stage("Switch folder ready")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"\n{error}\n")


if __name__ == "__main__":
    main()
