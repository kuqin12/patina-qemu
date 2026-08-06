##
# This plugin runs the QEMU command and monitors for asserts.
# It can also possibly run tests and parse the results
#
# Copyright (c) Microsoft Corporation
# SPDX-License-Identifier: BSD-2-Clause-Patent
##

import logging
import io
import os
import re
import subprocess
import time
from edk2toolext.environment.plugintypes import uefi_helper_plugin
from edk2toollib import utility_functions

from QemuCommandBuilder import QemuCommandBuilder
from QemuCommandBuilder import QemuArchitecture


class QemuRunner(uefi_helper_plugin.IUefiHelperPlugin):

    def __init__(self):
        self.logger = logging.getLogger(__name__)

    def RegisterHelpers(self, obj):
        fp = os.path.abspath(__file__)
        obj.Register("QemuRun", QemuRunner.Runner, fp)
        return 0

    @staticmethod
    # raw helper function to extract version number from QEMU
    def QueryQemuVersion(exec):
        if exec is None:
            return None

        result = io.StringIO()
        ret = utility_functions.RunCmd(exec, "--version", outstream=result)
        if ret != 0:
            return None

        # expected version string will be "QEMU emulator version maj.min.rev"
        res = result.getvalue()
        ver_str = re.search(r"version\s*([\d.]+)", res).group(1)

        return ver_str.split(".")

    @staticmethod
    def GetBuildBool(env, key: str, default: bool = False) -> bool:
        val = env.GetBuildValue(key)
        if val is None:
            return default
        return val.strip().lower() in ("true", "yes", "y", "1")

    @staticmethod
    def GetBuildStr(env, key: str, default: str | None = None) -> str | None:
        return env.GetBuildValue(key) or default

    @staticmethod
    def GetBool(env, key: str, default: bool = False) -> bool:
        val = env.GetValue(key)
        if val is None:
            return default
        return val.strip().lower() in ("true", "yes", "y", "1")

    @staticmethod
    def GetStr(env, key: str, default: str | None = None) -> str | None:
        return env.GetValue(key) or default

    @staticmethod
    def ParseCpuList(cpu_list: str) -> set[int]:
        """Parses a Linux CPU list such as ``0-3,8-11``."""
        cpus = set()
        for entry in cpu_list.strip().split(","):
            if not entry:
                continue
            if "-" in entry:
                first, last = (int(value) for value in entry.split("-", 1))
                cpus.update(range(first, last + 1))
            else:
                cpus.add(int(entry))
        return cpus

    @staticmethod
    def FormatCpuList(cpus: set[int]) -> str:
        """Formats CPU indices using Linux CPU-list notation."""
        ranges = []
        first = None
        last = None
        for cpu in sorted(cpus):
            if first is None:
                first = last = cpu
            elif cpu == last + 1:
                last = cpu
            else:
                ranges.append(str(first) if first == last else f"{first}-{last}")
                first = last = cpu

        if first is not None:
            ranges.append(str(first) if first == last else f"{first}-{last}")
        return ",".join(ranges)

    @staticmethod
    def GetPreferredPmuAffinity() -> set[int] | None:
        """Selects the fastest homogeneous Arm PMU CPU domain on Linux."""
        if os.name != "posix" or not hasattr(os, "sched_getaffinity"):
            return None

        event_source_path = "/sys/bus/event_source/devices"
        try:
            allowed_cpus = set(os.sched_getaffinity(0))
            with os.scandir(event_source_path) as event_sources:
                entries = sorted(event_sources, key=lambda entry: entry.name)
        except OSError:
            return None

        pmu_domains = []
        for entry in entries:
            if not entry.name.startswith("armv8_pmuv3_"):
                continue
            try:
                with open(os.path.join(entry.path, "cpus"), encoding="ascii") as cpus_file:
                    cpus = QemuRunner.ParseCpuList(cpus_file.read()) & allowed_cpus
            except (OSError, ValueError):
                continue
            if cpus:
                pmu_domains.append((entry.name, cpus))

        if len(pmu_domains) < 2:
            return None

        metrics = (
            ("cpu_capacity", "scheduler capacity"),
            ("acpi_cppc/highest_perf", "CPPC highest performance"),
            ("cpufreq/cpuinfo_max_freq", "maximum frequency"),
        )
        selected_domain = None
        selected_metric = None
        selected_score = None
        for metric_path, metric_name in metrics:
            domain_scores = []
            for domain_name, cpus in pmu_domains:
                scores = []
                for cpu in cpus:
                    path = os.path.join(
                        "/sys/devices/system/cpu", f"cpu{cpu}", metric_path
                    )
                    try:
                        with open(path, encoding="ascii") as metric_file:
                            scores.append(int(metric_file.read().strip(), 0))
                    except (OSError, ValueError):
                        pass
                domain_scores.append(
                    (max(scores) if scores else None, domain_name, cpus)
                )

            if all(score is not None for score, _, _ in domain_scores):
                selected_score, domain_name, selected_domain = max(domain_scores)
                selected_metric = metric_name
                break

        if selected_domain is None:
            domain_name, selected_domain = pmu_domains[0]
            logging.warning(
                "Unable to rank heterogeneous Arm PMU domains; selecting %s",
                domain_name,
            )
        else:
            logging.info(
                "Selecting Arm PMU domain %s on CPUs %s (%s: %d)",
                domain_name,
                QemuRunner.FormatCpuList(selected_domain),
                selected_metric,
                selected_score,
            )

        return selected_domain

    @staticmethod
    def StartSwTpm(tpm_dir, tpm_sock):
        """Starts the swtpm emulator and returns its Popen handle.

        swtpm is a long-lived daemon, so it is launched directly with Popen
        (rather than a blocking helper run in a thread) to keep a handle for
        explicit teardown.
        """
        cmd = [
            "swtpm", "socket",
            "--tpmstate", f"dir={tpm_dir}",
            "--ctrl", f"type=unixio,path={tpm_sock},terminate",
            "--tpm2",
            "--log", "level=1",
        ]
        try:
            return subprocess.Popen(cmd)
        except FileNotFoundError as error:
            raise FileNotFoundError(
                "swtpm executable not found on PATH. Install it (e.g. "
                "'sudo apt install swtpm' on Debian/Ubuntu, 'sudo dnf install "
                "swtpm' on Fedora) or disable SWTPM by setting SWTPM_ENABLE=FALSE."
            ) from error

    @staticmethod
    def StopSwTpm(swtpm_proc):
        """Terminates the swtpm subprocess if it is still running."""
        if swtpm_proc is None or swtpm_proc.poll() is not None:
            return
        try:
            swtpm_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                swtpm_proc.terminate()
            except PermissionError:
                logging.warning("swtpm did not exit after QEMU disconnected "
                                "and cannot be signaled by this process.")

    @staticmethod
    def Runner(env):
        """Runs QEMU"""

        alt_boot_enable = QemuRunner.GetBool(env, "ALT_BOOT_ENABLE", False)
        boot_to_front_page = QemuRunner.GetBool(env, "BOOT_TO_FRONT_PAGE", False)
        cpu_model = QemuRunner.GetStr(env, "CPU_MODEL")
        executable = QemuRunner.GetStr(env, "QEMU_PATH")
        gdb_server_port = QemuRunner.GetStr(env, "GDB_SERVER")
        headless = QemuRunner.GetBool(env, "QEMU_HEADLESS", False)
        monitor_port = QemuRunner.GetStr(env, "MONITOR_PORT")
        output_path = QemuRunner.GetStr(env, "BUILD_OUTPUT_BASE")
        path_to_os = QemuRunner.GetStr(env, "PATH_TO_OS")
        os_boot_device = QemuRunner.GetStr(env, "OS_BOOT_DEVICE", "SSD")
        path_to_seed = QemuRunner.GetStr(env, "PATH_TO_SEED")
        qemu_accelerator = QemuRunner.GetStr(env, "QEMU_ACCEL")
        qemu_executable_path = QemuRunner.GetStr(env, "QEMU_PATH")
        qemu_ext_dep_dir = QemuRunner.GetStr(env, "QEMU_DIR")
        repo_version = QemuRunner.GetStr(env, "VERSION", "Unknown")
        serial_port = QemuRunner.GetStr(env, "SERIAL_PORT")
        sw_tpm_enable = QemuRunner.GetBool(env, "SWTPM_ENABLE", True)
        virtual_drive = QemuRunner.GetStr(env, "VIRTUAL_DRIVE_PATH")

        secure_fd = os.path.join(output_path, "FV", "SECURE_FLASH0.fd")
        ns_fd = os.path.join(output_path, "FV", "QEMU_EFI.fd")

        # SWTPM is only available on Linux builds, exclude Windows
        if os.name == 'nt':
            logging.warning("SWTPM is not available on Windows builds.")
            sw_tpm_enable = False

        # Use a provided QEMU path. Otherwise use what is provided through the extdep
        if not qemu_executable_path:
            if qemu_ext_dep_dir:
                qemu_executable_path = os.path.join(qemu_ext_dep_dir, "qemu-system-aarch64")
            else:
                qemu_executable_path = "qemu-system-aarch64"

        # If we are using the QEMU external dependency, we need to tell it
        # where to look for roms
        rom_path = None
        if qemu_ext_dep_dir:
            rom_path = os.path.join(qemu_ext_dep_dir, "share")

        boot_selection = ""
        if boot_to_front_page:
            boot_selection += "Vol+"

        if alt_boot_enable:
            boot_selection += "Vol-"

        qemu_version = QemuRunner.QueryQemuVersion(qemu_executable_path)
        qemu_cmd_builder = (
            QemuCommandBuilder(qemu_executable_path, QemuArchitecture.ARM_VIRT)
            .with_cpu(cpu_model, 2)
            .with_machine(qemu_accelerator)
            .with_memory(8192 if path_to_os else 2048)
            .with_firmware(secure_fd, ns_fd)
            .with_rom_path(rom_path)
            .with_usb_controller()
            .with_usb_mouse()
            .with_usb_keyboard()
            .with_storage(path_to_os, os_boot_device)
            .with_virtual_drive(None if path_to_os else virtual_drive)
            .with_display(not headless)
            .with_network(False)
            .with_smbios(
                smbios_values={
                    # Type 0 (BIOS Information)
                    "smbios0_vendor": "Patina",
                    "smbios0_version": repo_version,
                    # Type 1 (System Information)
                    "smbios1_manufacturer": "OpenDevicePartnership",
                    "smbios1_product": "QEMU ARM Virt",
                    "smbios1_family": "QEMU",
                    "smbios1_version": str.join(".", qemu_version),
                    "smbios1_serial": "42-42-42-42",
                    "smbios1_uuid": "99fb60e2-181c-413a-a3cf-0a5fea8d87b0",
                    # Type 3 (Chassis Information)
                    "smbios3_manufacturer": "OpenDevicePartnership",
                    "smbios3_serial": "42-42-42-42",
                    "smbios3_asset": "ARM Virt",
                    "smbios3_sku": "ARM Virt",
                    "smbios3_version": boot_selection,
                }
            )
            .with_tpm(sw_tpm_enable, tpm_dir=output_path)
            .with_gdb_server(gdb_server_port)
            .with_serial_port(None, log_files=["secure_mm.log"])
            .with_virtio_serial(serial_port)
            .with_monitor_port(monitor_port)
        )

        if path_to_seed:
            qemu_cmd_builder = qemu_cmd_builder.with_custom("-drive", f"file=\"{path_to_seed}\",format=raw,if=virtio")

        (executable, args) = qemu_cmd_builder.build()
        logging.info(f"Running QEMU: {executable} {args}")

        swtpm_proc = None
        if sw_tpm_enable:
            tpm_dir = env.GetValue("BUILD_OUTPUT_BASE")
            tpm_sock = os.path.join(tpm_dir, "swtpm-sock")
            logging.info("Starting swtpm emulator.")
            swtpm_proc = QemuRunner.StartSwTpm(tpm_dir, tpm_sock)

            # Wait for swtpm to create the control socket before launching QEMU.
            # Otherwise QEMU may try to connect before the socket exists and fail.
            tpm_sock_timeout = 30
            tpm_sock_poll_start = time.monotonic()
            while not os.path.exists(tpm_sock):
                if swtpm_proc.poll() is not None:
                    logging.critical("swtpm exited before creating its socket.")
                    return -1
                if time.monotonic() - tpm_sock_poll_start > tpm_sock_timeout:
                    logging.critical(f"Timed out waiting for swtpm socket at {tpm_sock}.")
                    QemuRunner.StopSwTpm(swtpm_proc)
                    return -1
                time.sleep(0.1)

        ## TODO: Save the console mode. The original issue comes from: https://gitlab.com/qemu-project/qemu/-/issues/1674
        if os.name == "nt" and qemu_version[0] >= "8":
            import win32console

            std_handle = win32console.GetStdHandle(win32console.STD_INPUT_HANDLE)
            try:
                console_mode = std_handle.GetConsoleMode()
            except Exception:
                std_handle = None

        # Run QEMU. KVM requires vCPUs to remain in one hardware PMU domain on
        # heterogeneous Arm hosts; child processes inherit this affinity.
        original_affinity = None
        try:
            pmu_affinity = QemuRunner.GetPreferredPmuAffinity()
            if pmu_affinity is not None:
                original_affinity = set(os.sched_getaffinity(0))
                os.sched_setaffinity(0, pmu_affinity)
            ret = utility_functions.RunCmd(executable, str.join(" ", args))
        finally:
            if original_affinity is not None:
                try:
                    os.sched_setaffinity(0, original_affinity)
                except OSError as error:
                    logging.warning("Failed to restore runner CPU affinity: %s", error)
            QemuRunner.StopSwTpm(swtpm_proc)

        ## TODO: restore the customized RunCmd once unit tests with asserts are figured out
        if ret == 0xC0000005:
            ret = 0

        ## TODO: remove this once we upgrade to newer QEMU
        if ret == 0x8B and qemu_version[0] == "4":
            # QEMU v4 will return segmentation fault when shutting down.
            # Tested same FDs on QEMU 6 and 7, not observing the same.
            ret = 0

        if os.name == "nt" and qemu_version[0] >= "8" and std_handle is not None:
            # Restore the console mode for Windows on QEMU v8+.
            std_handle.SetConsoleMode(console_mode)
        elif os.name != "nt":
            # Linux version of QEMU will mess with the print if its run failed, let's just restore it anyway
            utility_functions.RunCmd("stty", "sane", capture=False)

        return ret
