# Build Details

Due to the constant changing of build tools and often subtle differences that can be encountered on build machines,
the recommended method to compile is to use a Linux container as described in the primary
[Readme.md](https://github.com/OpenDevicePartnership/patina-qemu/blob/main/Readme.md) file. If however it is more
desirable to support building in Linux or Windows outside the container, the following steps are not guaranteed to
work in all scenarios, but do provide a setup baseline to work from.

## Prerequisites

The following tables list all tools necessary to compile in the respective environment.  The process has been put into a
script to prepare for building `QemuQ35Pkg` on a developer's host system, but it relies on [Python 3](https://www.python.org/downloads/)
being installed and is dependent on the environment.  It should be treated as a possible shortcut instead of a tool expected
to work in all scenarios.

```text
  python workspace_setup.py   <== Run in Linux 🐧
  py -3 workspace_setup.py    <== Run in Windows 🪟
```

<!-- markdownlint-disable MD033
MD033: Allow inline HTML for styling elements not easily handled by Markdown.-->

### 🪟 Windows 11

| Tool | Install Command |
| --- | --- |
| [Chocolatey](https://chocolatey.org/) | `winget install --id Chocolatey.Chocolatey -e` |
| [Python 3](https://www.python.org/) | `winget install --id Python.Python.3.12 -e` <br> **Note:** Disable any app execution alias defined for `python.exe` and `python3.exe` from Windows settings(Apps > Advanced app settings > App execution alias) |
| [Git](https://git-scm.com/) | `winget install --id Git.Git -e` |
| [Rust](https://rustup.rs/) | `winget install --id Rustlang.Rustup -e` <ol><li> **Add x86_64 uefi target:** `rustup target add x86_64-unknown-uefi` </li><li> **Add aarch64 uefi target:** `rustup target add aarch64-unknown-uefi`</li><li>**Install cargo make:** `cargo install cargo-make`</li><li>**Install cargo tarpaulin:** `cargo install cargo-tarpaulin`</li></ol> |
| [LLVM](https://llvm.org/) | `winget install --id LLVM.LLVM -e --override "/S /D=C:\LLVM"` <ul><li>**Note:** `/D=C:\LLVM` override(with no spaces) is needed for AArch64 build of `patina-qemu` repo on Windows.</li></ul> |
| [GNU Make](https://community.chocolatey.org/packages/make) | `choco  install make` <ul><li>**Note:** Needed for AArch64 build of `patina-qemu` repo on Windows.</li></ul> |
| [MSVC BuildTools](https://rust-lang.github.io/rustup/installation/windows-msvc.html#installing-only-the-required-components-optional) | `winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Component.VC.CoreBuildTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64  --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.Tools.ARM  --add Microsoft.VisualStudio.Component.VC.Tools.ARM64"` <br> **Note:** Only required when building for `std` |
| [Node](https://nodejs.org/en) | `winget install --id OpenJS.NodeJS.LTS -e` <ol><li> **Add cspell:** `npm install -g cspell@latest` </li><li> **Add markdown lint cli:** `npm install -g markdownlint-cli` </li></ol> |
| [QEMU](https://www.qemu.org/) | `winget install --id SoftwareFreedomConservancy.QEMU -e -v 10.0.0` |
| **Optional** | |
| [WinDBG](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/) | `winget install --id Microsoft.WinDbg -e` |
| [VSCode](https://code.visualstudio.com/) | `winget install --id Microsoft.VisualStudioCode -e` |
| [Rust Analyzer](https://marketplace.visualstudio.com/items?itemName=rust-lang.rust-analyzer) | `code --install-extension rust-lang.rust-analyzer` |

**Note:** Add the LLVM (`C:\LLVM\bin`) and QEMU (`C:\Program Files\qemu\bin`) directories to the `PATH` environment variable.

### 🐧 Linux/WSL

| Tool | Install Command |
| --- | --- |
| Build Essentials | `sudo apt update && sudo apt install -y build-essential git nasm m4 bison flex curl wget uuid-dev python3 python3-venv python-is-python3 unzip acpica-tools gcc-multilib mono-complete pkg-config libssl-dev mtools dosfstools device-tree-compiler` |
| [Rust](https://rustup.rs/) | `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \| sh` <br>**Note:** Might have to reopen the terminal <ol><li> **Add x86_64 uefi target:** `rustup target add x86_64-unknown-uefi` </li><li> **Add aarch64 uefi target:** `rustup target add aarch64-unknown-uefi`</li><li>**Install cargo make:** `cargo install cargo-make`</li><li>**Install cargo tarpaulin:** `cargo install cargo-tarpaulin`</li></ol> |
| [Node](https://nodejs.org/en) | `curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh \| bash` <br>`source ~/.bashrc`<br>`nvm install --lts` <ol><li> **Add cspell:** `npm install -g cspell@latest` </li><li> **Add markdown lint cli:** `npm install -g markdownlint-cli` </li></ol> |
| [QEMU](https://www.qemu.org/) | `sudo apt install -y qemu-system` |
| [LLVM](https://llvm.org/) | `sudo apt install -y clang llvm lld` |
| **Optional** | |
| [VSCode](https://code.visualstudio.com/docs/setup/linux#_debian-and-ubuntu-based-distributions) | `wget https://go.microsoft.com/fwlink/?LinkID=760868 -O code.deb` <br> `sudo apt install ./code.deb` |
| [Rust Analyzer](https://marketplace.visualstudio.com/items?itemName=rust-lang.rust-analyzer) | `code --install-extension rust-lang.rust-analyzer` |

<!-- markdownlint-enable MD033 -->

## Clone, Build, and Run

Clone the [patina-qemu](https://github.com/OpenDevicePartnership/patina-qemu/) repository and change to its directory to
be the workspace

```text
  git clone https://github.com/OpenDevicePartnership/patina-qemu
  cd ./patina-qemu
```

Setup the Python virtual environment and pull the pip requirements

```text
  python -m venv q35venv
  q35venv\Scripts\activate.bat  <== Run in Windows 🪟
  source q35venv/bin/activate   <== Run in Linux 🐧
  pip install --upgrade -r pip-requirements.txt
```

Run the Stuart commands to update the submodules, pull external dependencies, and compile.  The `--flashrom` switch on the
stuart_build command will automatically initiate the QEMU launch.

```text
  stuart_setup -c Platforms/QemuQ35Pkg/PlatformBuild.py
  stuart_update -c Platforms/QemuQ35Pkg/PlatformBuild.py
  stuart_build -c Platforms/QemuQ35Pkg/PlatformBuild.py --flashrom
```

## Advanced Options

- To compile the Patina DXE Core and integrate into this build instead of using the pre-compiled binary, see the
[Rust Integration](https://github.com/OpenDevicePartnership/patina-dxe-core-qemu) documentation
- To setup a debugger in UEFI, see the [WinDbg + QEMU + Patina UEFI Debugging Guide](Platforms/Docs/Common/windbg-qemu-uefi-debugging.md)
- To setup a debugger in Windows, see the [WinDbg + QEMU + Patina UEFI + Windows OS Debugging Guide](Platforms/Docs/Common/windbg-qemu-windows-debugging.md)
- Refer to the [Self Certification Test](https://github.com/OpenDevicePartnership/patina-qemu/blob/main/docs/SelfCertifcationTest.md)
documentation for information on how to configure and run the Tianocore Self Certification Tests (SCTs)
- To use a custom QEMU installation, add the `QEMU_PATH` argument to the stuart_build command

```text
  stuart_build -c Platforms/QemuQ35Pkg/PlatformBuild.py --flashonly QEMU_PATH="<path to qemu executable>
```

- To launch an OS in QEMU instead of the UEFI shell, add the path to the OS in the stuart_build command line

```text
  stuart_build -c Platforms/QemuQ35Pkg/PlatformBuild.py --flashrom PATH_TO_OS="C:\OS\Windows11.qcow2
```
