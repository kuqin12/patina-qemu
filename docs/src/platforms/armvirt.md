# QEMU Arm Virt

`QemuArmVirtPkg`...

- Is a derivative of [ArmVirtPkg](https://github.com/tianocore/edk2/tree/master/ArmVirtPkg) based on the QEMU ARM-Virt machine type.
- Will not support Legacy BIOS or CSM.
- Will not support S3 sleep functionality.
- Has 64-bit SEC and DXE phases.
- Targets a tightly constrained virtual platform based on the QEMU ARM CPUs.

By focusing solely on the ARM chipset, this package is allowed to break compatibility with other QEMU supported
chipsets. The ARM chipset can be paired with an AArch64 processor to emulate ARM-based hardware with industry standard
features like TrustZone and PCI-E.

## Building and Running

`QemuArmVirtPkg` uses the Patina repositories and EDK II PyTools for its build operations. See
[Building the Firmware](../building/building.md) for full instructions.

## Host TAP Networking

The ARM Virt QEMU launcher expects a pre-created TAP interface named `tap-hlk`:

```text
-netdev tap,id=net0,ifname=tap-hlk,script=no,downscript=no
```

The working host configuration uses a NetworkManager bridge. The physical Ethernet interface and `tap-hlk` are bridge
ports, while the host's DHCP address and default route belong to `br-hlk`:

```text
physical Ethernet --+
					+-- br-hlk -- LAN/DHCP
tap-hlk ----------+
```

This is a layer-2 configuration. It does not require IP forwarding, NAT, `iptables`, or `nftables`. The Windows guest
obtains its own address from the LAN DHCP server and is directly reachable by an HLK controller on that network.

### Create the NetworkManager Profiles

Use a local console or out-of-band management when changing the physical interface. Moving the interface into the
bridge can interrupt an SSH connection and may change the host's DHCP lease.

Set `PHYSICAL_NIC` to the wired interface that currently reaches the LAN. The known working host uses
`enx9c69d339f87e`; another system will normally have a different name.

```bash
export PHYSICAL_NIC=enx9c69d339f87e
export BRIDGE=br-hlk
export TAP=tap-hlk
export QEMU_USER="${SUDO_USER:-$USER}"
export QEMU_UID="$(id -u "$QEMU_USER")"

nmcli -t -f NAME,TYPE,DEVICE connection show

sudo nmcli connection add \
	type bridge \
	ifname "$BRIDGE" \
	con-name "$BRIDGE" \
	bridge.stp no \
	ipv4.method auto \
	ipv6.method auto

sudo nmcli connection add \
	type ethernet \
	ifname "$PHYSICAL_NIC" \
	con-name "$BRIDGE-port" \
	controller "$BRIDGE" \
	port-type bridge

sudo nmcli connection add \
	type tun \
	ifname "$TAP" \
	con-name "$TAP" \
	mode tap \
	owner "$QEMU_UID" \
	controller "$BRIDGE" \
	port-type bridge

sudo nmcli connection modify "$BRIDGE" connection.autoconnect yes
sudo nmcli connection modify "$BRIDGE-port" connection.autoconnect yes
sudo nmcli connection modify "$TAP" connection.autoconnect yes

sudo nmcli connection up "$BRIDGE-port"
sudo nmcli connection up "$TAP"
```

Activating the bridge port normally activates its `br-hlk` controller automatically. If it does not, run
`sudo nmcli connection up br-hlk` before activating the two ports.

The TAP owner must be the user that runs QEMU. Using the numeric UID makes the ownership survive QEMU restarts and
allows QEMU to open the persistent interface without running as root.

### Verify the Host

```bash
ip -details address show tap-hlk
bridge link show
ip address show br-hlk
ip route show
nmcli -t -f NAME,TYPE,DEVICE connection show --active
```

Verify all of the following:

- `tap-hlk` reports `tun type tap`, `persist on`, and the QEMU user's UID.
- Both `tap-hlk` and the physical Ethernet interface report `master br-hlk` and `state forwarding`.
- The host address and default route use `br-hlk`, not the physical interface.
- The guest obtains a LAN address after the VirtIO network driver is installed.

The launcher mounts the VirtIO driver ISO for Windows. Guest firewall and LAN policy must permit the connections needed
by the HLK controller.

### Migration Notes

- Use wired Ethernet. Most Wi-Fi client interfaces cannot transparently bridge multiple guest MAC addresses.
- Replace the physical interface name and TAP owner UID on the destination host; do not copy those values verbatim.
- Switch port security, 802.1X, or DHCP policies may reject the guest's additional MAC address.
- Create and activate `tap-hlk` before starting QEMU because `script=no,downscript=no` prevents QEMU from configuring it.
- Do not pass user-network `hostfwd` options with this TAP backend.

To remove the setup, first reactivate the physical interface's original NetworkManager profile from a local console,
then delete the three profiles:

```bash
sudo nmcli connection down tap-hlk
sudo nmcli connection down br-hlk
sudo nmcli connection up "Wired connection 1"
sudo nmcli connection delete tap-hlk br-hlk-port br-hlk
```

Replace `Wired connection 1` with the original profile name shown before creating the bridge.
