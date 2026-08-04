# Hybrid KVM/TCG TrustZone Plan

## Status

This document records a proposed implementation plan. The architecture and the
temporary bring-up decisions are not commitments to a stable QEMU interface.

## Goal

Run a usable Arm64 normal-world operating system with KVM while continuing to
execute the existing secure firmware stack with TCG:

- TF-A BL31 and SPMD at EL3
- Hafnium as the S-EL2 SPMC
- MSSP, including the TPM service, at S-EL1
- The QEMU TPM device connected to `swtpm`

The primary validation target is the normal-world TPM driver and its CRB-over-FF-A
communication with the real MSSP implementation. This design does not claim to
reproduce the security or timing properties of a physical TrustZone system.

## Verified Constraints

- Arm KVM does not expose a guest EL3 or guest secure state.
- QEMU rejects `virt,secure=on` with KVM and requires TCG for full TrustZone
  emulation.
- QEMU selects one machine-wide accelerator. A KVM-realized `ARMCPU` cannot be
  passed directly to `cpu_exec()` because it has not been realized as a TCG CPU.
- Linux arm64 KVM can forward selected guest SMC/HVC function ranges to userspace
  with an SMCCC filter and `KVM_EXIT_HYPERCALL`.
- The current QEMU Arm KVM backend does not install SMCCC filters or handle
  `KVM_EXIT_HYPERCALL`.
- KVM acceleration of an Arm64 guest requires an Arm64 KVM host. The current
  x86-64 development host cannot provide that acceleration.
- Microsoft does not support an HLK test system running in a third-party
  hypervisor. This design can support driver development and pre-HLK testing,
  but it is not an authoritative HLK DUT.

## Proposed Architecture

Use one QEMU process and one `virt` machine device model, but maintain two CPU
execution domains:

```text
Normal-world UEFI and OS
    KVM ARM vCPUs, NS-EL1
             |
             | matching FF-A SMC causes KVM_EXIT_HYPERCALL
             v
Hybrid dispatch layer in QEMU
             |
             | copy FF-A registers and run secure worker
             v
Shadow TCG Arm CPU state
    TF-A EL3 -> Hafnium S-EL2 -> MSSP S-EL1
             |
             v
Secure-only TPM MMIO -> QEMU TPM device -> swtpm
```

The KVM CPU state remains authoritative for normal-world execution. The shadow
TCG CPU state remains authoritative for EL3 and the secure world. The same CPU
object must never alternate between KVM and TCG execution.

### Memory Views

The two domains share selected host memory backing through QEMU `MemoryRegion`
objects:

- Normal RAM and the internal CRB at `0x40200000` are visible to KVM and TCG.
- Secure RAM, secure flash and secure-only devices are visible only through the
  shadow TCG secure address space.
- The external TPM interface at `0x0c000000` remains secure-only.
- The KVM vCPUs are created without EL3 or a secure address space.

This requires separating "create the secure machine view" from the existing
`secure=on` behavior that also enables EL3 on every machine CPU.

### Secure Bootstrapping

Before starting the KVM vCPUs:

1. Initialize TCG translation support and the shadow Arm CPU state.
2. Boot the existing secure flash through TF-A and Hafnium.
3. Stop shadow execution at the first normal-world BL33 handoff.
4. Transfer only the required BL33 entry registers and entry address into the
   initial KVM vCPU.
5. Start normal-world UEFI under KVM.

The shadow normal-world context is a boundary context used by TF-A for world
switching. It is not a copy of Windows EL1 system state.

### SMC Dispatch

QEMU installs `KVM_SMCCC_FILTER_FWD_TO_USER` ranges for the FF-A function IDs
before any KVM vCPU runs. A matching call follows this path:

1. A KVM vCPU executes `SMC` with the FF-A function ID in `x0`.
2. `KVM_RUN` returns `KVM_EXIT_HYPERCALL` to that QEMU vCPU thread.
3. QEMU reads `x0-x17` with `KVM_GET_ONE_REG`.
4. QEMU records the originating KVM vCPU and submits the register payload to the
   secure worker.
5. The secure worker injects the request at the shadow normal/secure boundary
   and runs TCG.
6. TCG stops when TF-A returns to the synthetic normal-world boundary.
7. QEMU writes the response in `x0-x17` with `KVM_SET_ONE_REG`.
8. QEMU re-enters `KVM_RUN`; the guest PC already points after the `SMC`.

The first supported call set should be limited to the calls needed by the TPM
path, including partition discovery, `FFA_MSG_SEND_DIRECT_REQ2`, direct response,
and `FFA_RUN`. Unsupported calls must return an architectural FF-A error rather
than silently succeeding.

## CPU and Concurrency Model

The normal-world caller and secure-partition vCPU are different identities:

- Any Windows KVM vCPU may issue a TPM FF-A call.
- MSSP has `execution-ctx-count = <1>`, so its only secure vCPU index is `0`.
- `FFA_RUN` therefore targets MSSP vCPU `0`; this does not imply normal-world
  vCPU0.
- The response must be returned to the KVM vCPU that issued the request.

Hafnium normally migrates a UP secure partition to the physical CPU that issued
the direct request. Initially, the hybrid backend will model that behavior with
one serialized secure worker and an explicit originating-vCPU continuation.

If another KVM vCPU calls while secure execution is active, the initial backend
will return `FFA_BUSY`. It will not hide contention by indefinitely queueing a
second TPM transaction. Later work may implement fair scheduling if the Windows
driver requires it.

## Interrupt Model

The current MSSP manifest uses `ns-interrupts-action = <0>`, so non-secure
interrupts are queued while the secure partition runs.

For the initial implementation:

- Normal-world interrupt state remains entirely in KVM's VGIC.
- Secure interrupt state remains entirely in the TCG secure model.
- QEMU devices and irqfds may continue asserting normal interrupts while a
  calling KVM vCPU waits for the secure worker.
- Interrupts targeting other KVM vCPUs can be serviced immediately.
- Interrupts targeting the blocked caller remain pending in the VGIC and are
  delivered after QEMU returns the FF-A response and re-enters KVM.

This preserves the selected FF-A queued-interrupt policy, but secure-call latency
is added to interrupt latency on the calling vCPU.

A later phase may support the signaled-interrupt policy:

1. Stop shadow TCG execution when a normal interrupt must preempt the call.
2. Return `FFA_INTERRUPT` to the originating KVM vCPU.
3. Allow the OS to service the interrupt.
4. Resume MSSP vCPU `0` through `FFA_RUN`.

## Temporary Bring-Up Decisions

These decisions deliberately reduce the first implementation's scope. They must
remain visible as temporary compatibility gaps.

| Area | Temporary decision | Completion requirement |
| --- | --- | --- |
| Normal caller | Accept FF-A calls from any KVM vCPU | Keep per-caller continuations and test calls from every vCPU |
| Secure execution | One dedicated, serialized TCG worker | Preserve one UP MSSP context and evaluate matched shadow pCPU contexts |
| Concurrent TPM calls | Return `FFA_BUSY` | Validate Windows retry/serialization behavior |
| Normal interrupts | Queue in KVM VGIC | Measure latency; add `FFA_INTERRUPT` preemption only if required |
| Secure interrupts | Do not support asynchronous secure interrupts initially | Add explicit routing and wakeup before claiming general FF-A support |
| Power management | Let KVM handle PSCI initially | Model TF-A/Hafnium CPU power notifications for full SMP fidelity |
| FF-A surface | Implement only the synchronous TPM-required subset | Reject unsupported calls and expand from observed requirements |
| Host support | Develop and run accelerated mode on Arm64 Linux | Keep TCG-only mode available for x86-64 development |

Restricting normal-world calls to vCPU0 is not part of the intended design. A
single-vCPU smoke test may be useful during bring-up, but SMP caller support is a
required milestone.

## Implementation Phases

### Phase 1: KVM Exit Round Trip

- Add a capability probe for the Arm SMCCC filter attribute.
- Install narrowly scoped FF-A forwarding ranges before the first `KVM_RUN`.
- Handle `KVM_EXIT_HYPERCALL` in `target/arm/kvm.c`.
- Read and write `x0-x17` and validate a synthetic FF-A call round trip.
- Preserve existing KVM PSCI and unrelated SMCCC behavior.

Exit criterion: a test guest calls a forwarded function from each vCPU and
receives the expected register response without affecting interrupts or PSCI.

#### Current Phase 1 Status

The QEMU implementation is opt-in with:

```text
-accel kvm,arm-ffa-forward=on
```

When enabled, QEMU probes `KVM_ARM_VM_SMCCC_FILTER`, forwards the FF-A function
number range `0x60-0x8e` for both SMC32 and SMC64, and handles the resulting
`KVM_EXIT_HYPERCALL` on the calling vCPU. The temporary handler synchronizes
`x0-x17` and returns `FFA_ERROR` with zero-extended `FFA_NOT_SUPPORTED` in `x2`.
The `kvm_arm_ffa_stub` trace event records the calling vCPU, function ID, `x1`,
and `x2`.

Runtime validation completed on a four-vCPU Arm64 KVM host running Linux
7.0.0-28-generic. The generic `kvm_vm_check_attr()` helper incorrectly rejected
the filter because `KVM_CAP_VM_ATTRIBUTES` reported zero, even though the Arm
VM-fd `KVM_HAS_DEVICE_ATTR` ioctl supports `KVM_ARM_VM_SMCCC_FILTER`. Probing the
VM-fd attribute directly allowed both filters to be installed.

A bare-metal guest started vCPUs 1-3 with PSCI `CPU_ON`, then issued one SMC32
call (`0x84000063`) and one SMC64 call (`0xc400006f`) from every vCPU. All eight
calls exited to the QEMU handler on the originating CPU. Every caller received
`FFA_ERROR` in `x0`, zero-extended `FFA_NOT_SUPPORTED` in `x2`, and zero in
`x1` and `x3-x17`. All PSCI `CPU_ON` calls and the final PSCI `SYSTEM_OFF`
succeeded without producing FF-A userspace exits. Calls immediately below and
above the SMC32 and SMC64 filter ranges also remained in KVM and produced no
userspace exit.

The reproducible test is `tests/arm-kvm/run-ffa-smccc-smp.sh` in the QEMU tree.
Ten consecutive four-vCPU iterations passed with exactly two FF-A traces per
vCPU and clean PSCI shutdown.

Interrupt validation uses the opt-in test delay:

```text
-accel kvm,arm-ffa-forward=on,arm-ffa-stub-delay-ms=250
```

The `tests/arm-kvm/run-ffa-smccc-irq.sh` guest configures the GICv3 virtual timer
PPI on all four vCPUs. CPU0 arms a timer to expire during the delayed FF-A exit;
the interrupt is delivered before the guest executes the SMC continuation.
Meanwhile, vCPUs 1-3 continue servicing periodic virtual timers, with each
recording at least four interrupts while CPU0 is in userspace. A zero-delay
negative control fails with no interrupt observed at the SMC continuation,
confirming that the positive test covers the intended overlap. Ten consecutive
positive iterations passed.

The Phase 1 SMCCC filter, register round trip, PSCI isolation, SMP caller, and
interrupt behavior gates now pass on this host. Phase 2 has not begun.

### Phase 2: Shadow TCG Bootstrap

- Introduce a hybrid Arm execution component without changing the active KVM
  accelerator for normal machine CPUs.
- Realize shadow TCG CPU state and its secure address space.
- Boot TF-A and Hafnium to the BL33 handoff boundary.
- Add deterministic stop conditions for normal-world handoff and secure return.

Exit criterion: TF-A and Hafnium reach their waiting state under shadow TCG,
then process a synthetic direct request and return to the boundary.

### Phase 3: Shared CRB and TPM

- Share the internal CRB backing between KVM and the shadow TCG address space.
- Keep external TPM MMIO secure-only.
- Connect the existing QEMU TPM device and `swtpm`.
- Route the real TPM service UUID and `DIRECT_REQ2` payload.

Exit criterion: a TPM capability command initiated by normal-world firmware or
an OS completes through MSSP and `swtpm`.

### Phase 4: SMP and Contention

- Track the originating KVM vCPU for every in-flight call.
- Test TPM calls from every Windows CPU.
- Return and validate `FFA_BUSY` for simultaneous requests.
- Add per-vCPU logging and assertions for continuation ownership.

Exit criterion: repeated calls from all vCPUs return to the correct caller, and
concurrent calls fail or retry predictably without corrupting CRB state.

### Phase 5: Interrupts and Timing

- Verify VGIC pending behavior during secure execution.
- Measure TPM call duration, IRQ latency and Windows watchdog behavior.
- Add secure-call timeouts and diagnostics.
- Implement `FFA_INTERRUPT`/`FFA_RUN` preemption only if queued latency is not
  acceptable.

Exit criterion: Windows remains stable under device and timer interrupt load
while TPM operations run repeatedly.

### Phase 6: Windows Driver Validation

- Boot the full Windows image with KVM on an Arm64 host.
- Validate ACPI TPM2/FF-A discovery and locality handling.
- Run driver stress, suspend/reboot and negative-path tests.
- Compare results with the existing full-TCG platform.

Exit criterion: the same driver-visible TPM contract passes on full TCG and the
hybrid backend, with documented differences.

## Initial QEMU Change Areas

Expected QEMU implementation surfaces include:

- `target/arm/kvm.c`: filter setup, hypercall exit handling and register transfer
- `accel/kvm/kvm-all.c`: existing generic exit dispatch and vCPU wait behavior
- `accel/tcg/`: explicit secondary TCG initialization and secure worker execution
- `hw/arm/virt.c`: secure memory/device construction independent of KVM CPU EL3
- Arm CPU realization: creation of shadow CPU state that is not a KVM vCPU
- Migration/reset paths: hybrid state must either be supported or explicitly
  rejected

The implementation should introduce an explicit machine option for the hybrid
mode. It must not silently change the meaning of existing `secure=on`.

## Risks and Open Questions

- TCG initialization currently assumes it is the active machine accelerator and
  uses process-global state. A clean secondary initialization path may require a
  new hybrid accelerator rather than a local Arm-only helper.
- QEMU's `current_cpu`, translation cache, timer and CPU-thread assumptions must
  be audited before running a shadow CPU on a dedicated worker.
- The BL33 handoff and later secure return need distinct, robust stop conditions.
- KVM-handled PSCI does not automatically notify shadow TF-A/Hafnium of CPU power
  events. The temporary single-shadow-pCPU model is not full SMP firmware
  fidelity.
- Shared RAM does not model TrustZone access control. QEMU must enforce which
  address-space view exposes secure-only regions.
- Migration, snapshots and deterministic replay are out of scope until all
  shadow state has an explicit save format.
- Performance depends on how much secure firmware executes per TPM command. The
  normal OS becomes fast under KVM, but TPM latency remains TCG-bound.

## Non-Goals for the First Version

- General-purpose virtual TrustZone support for arbitrary guests
- Hardware-equivalent security isolation
- Multiple independent normal-world VMs sharing one secure world
- Complete FF-A notification and secure-interrupt support
- Live migration or snapshot compatibility
- Treating a virtual machine as an officially supported HLK certification DUT
