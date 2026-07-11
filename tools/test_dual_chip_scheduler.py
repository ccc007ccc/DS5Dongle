#!/usr/bin/env python3
"""Deterministic host model for the dual-chip realtime scheduler contract.

This is deliberately a behavior model, not a source-contract test.  It keeps
the model small enough to audit while exercising the fixed capacities,
replacement policies, arbitration, generation changes, and conservation laws
from docs/DUAL_CHIP_REALTIME_SCHEDULER_SPEC.md.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from enum import Enum, auto
import random
import unittest


RT_CAPACITY = 2
RT_MAX_AGE_US = 64_000
CONTROL_CAPACITY = 8


@dataclass(frozen=True)
class Item:
    generation: int
    sequence: int
    created_us: int
    payload: object = None


@dataclass
class ClassMetrics:
    accepted: int = 0
    transmitted: int = 0
    pending: int = 0
    highwater: int = 0
    coalesced: int = 0
    replaced: int = 0
    stale: int = 0
    admission_rejected: int = 0
    transport_failed: int = 0
    age_last_us: int = 0
    age_max_us: int = 0
    age_sum_us: int = 0
    age_samples: int = 0

    def record_age(self, age_us: int) -> None:
        assert age_us >= 0
        self.age_last_us = age_us
        self.age_max_us = max(self.age_max_us, age_us)
        self.age_sum_us += age_us
        self.age_samples += 1


class SlotState(Enum):
    FREE = auto()
    WRITING = auto()
    READY = auto()
    SENDING = auto()


@dataclass
class RtSlot:
    state: SlotState = SlotState.FREE
    item: Item | None = None


@dataclass(frozen=True)
class RtHandle:
    index: int
    item: Item


class RtRing:
    """Two-slot FREE/WRITING/READY/SENDING realtime store."""

    def __init__(self, capacity: int = RT_CAPACITY) -> None:
        self.capacity = capacity
        self.slots = [RtSlot() for _ in range(capacity)]
        self.metrics = ClassMetrics()

    def _refresh_pending(self) -> None:
        self.metrics.pending = sum(
            slot.state is not SlotState.FREE for slot in self.slots
        )
        self.metrics.highwater = max(
            self.metrics.highwater, self.metrics.pending
        )

    def publish(self, item: Item) -> bool:
        target = next(
            (
                index
                for index, slot in enumerate(self.slots)
                if slot.state is SlotState.FREE
            ),
            None,
        )
        if target is None:
            ready = [
                (slot.item.created_us, slot.item.sequence, index)
                for index, slot in enumerate(self.slots)
                if slot.state is SlotState.READY and slot.item is not None
            ]
            if not ready:
                self.metrics.admission_rejected += 1
                return False
            _, _, target = min(ready)
            self.metrics.replaced += 1

        slot = self.slots[target]
        assert slot.state is not SlotState.SENDING
        slot.state = SlotState.WRITING
        slot.item = item
        slot.state = SlotState.READY
        self.metrics.accepted += 1
        self._refresh_pending()
        return True

    def expire_ready(self, now_us: int, max_age_us: int = RT_MAX_AGE_US) -> int:
        expired = 0
        for slot in self.slots:
            if (
                slot.state is SlotState.READY
                and slot.item is not None
                and now_us - slot.item.created_us > max_age_us
            ):
                slot.state = SlotState.FREE
                slot.item = None
                self.metrics.stale += 1
                expired += 1
        self._refresh_pending()
        return expired

    def select_oldest(self, generation: int) -> RtHandle | None:
        while True:
            ready = [
                (slot.item.created_us, slot.item.sequence, index)
                for index, slot in enumerate(self.slots)
                if slot.state is SlotState.READY and slot.item is not None
            ]
            if not ready:
                return None
            _, _, index = min(ready)
            slot = self.slots[index]
            assert slot.item is not None
            if slot.item.generation != generation:
                slot.state = SlotState.FREE
                slot.item = None
                self.metrics.stale += 1
                self._refresh_pending()
                continue
            slot.state = SlotState.SENDING
            return RtHandle(index, slot.item)

    def complete(self, handle: RtHandle, now_us: int, success: bool) -> None:
        slot = self.slots[handle.index]
        assert slot.state is SlotState.SENDING
        assert slot.item == handle.item
        if success:
            self.metrics.transmitted += 1
            self.metrics.record_age(now_us - handle.item.created_us)
        else:
            self.metrics.transport_failed += 1
        slot.state = SlotState.FREE
        slot.item = None
        self._refresh_pending()

    def clear_generation(self, generation: int) -> None:
        for slot in self.slots:
            if slot.item is not None and slot.item.generation != generation:
                self.metrics.stale += 1
                slot.state = SlotState.FREE
                slot.item = None
        self._refresh_pending()

    @property
    def ready(self) -> int:
        return sum(slot.state is SlotState.READY for slot in self.slots)

    def assert_invariants(self) -> None:
        active = [slot for slot in self.slots if slot.state is not SlotState.FREE]
        assert len(active) <= self.capacity
        assert self.metrics.pending == len(active)
        assert self.metrics.accepted == (
            self.metrics.transmitted
            + self.metrics.replaced
            + self.metrics.stale
            + self.metrics.transport_failed
            + self.metrics.pending
        )


@dataclass(frozen=True)
class MailboxSnapshot:
    item: Item


class StateMailbox:
    def __init__(self) -> None:
        self.item: Item | None = None
        self.dirty = False
        self.metrics = ClassMetrics()

    def publish(self, item: Item) -> None:
        if self.dirty:
            self.metrics.coalesced += 1
        self.item = item
        self.dirty = True
        self.metrics.accepted += 1
        self.metrics.pending = 1
        self.metrics.highwater = 1

    def snapshot(self, generation: int) -> MailboxSnapshot | None:
        if not self.dirty or self.item is None:
            return None
        if self.item.generation != generation:
            self.clear_generation(generation)
            return None
        return MailboxSnapshot(self.item)

    def complete(
        self, snapshot: MailboxSnapshot, now_us: int, success: bool
    ) -> None:
        if success:
            self.metrics.transmitted += 1
            self.metrics.record_age(now_us - snapshot.item.created_us)
            if self.item == snapshot.item:
                self.item = None
                self.dirty = False
                self.metrics.pending = 0
        else:
            self.metrics.transport_failed += 1

    def clear_generation(self, generation: int) -> None:
        if self.item is not None and self.item.generation != generation:
            self.metrics.stale += 1
            self.item = None
            self.dirty = False
            self.metrics.pending = 0


class ControlQueue:
    def __init__(self, capacity: int = CONTROL_CAPACITY) -> None:
        self.capacity = capacity
        self.items: deque[Item] = deque()
        self.metrics = ClassMetrics()
        self.completed_latencies: list[int] = []

    def submit(self, item: Item) -> bool:
        if len(self.items) == self.capacity:
            self.metrics.admission_rejected += 1
            return False
        self.items.append(item)
        self.metrics.accepted += 1
        self._refresh_pending()
        return True

    def _refresh_pending(self) -> None:
        self.metrics.pending = len(self.items)
        self.metrics.highwater = max(self.metrics.highwater, len(self.items))

    def complete_one(self, now_us: int, success: bool = True) -> Item | None:
        if not self.items:
            return None
        item = self.items.popleft()
        if success:
            self.metrics.transmitted += 1
            age = now_us - item.created_us
            self.completed_latencies.append(age)
            self.metrics.record_age(age)
        else:
            self.metrics.transport_failed += 1
        self._refresh_pending()
        return item

    def clear_generation(self, generation: int) -> None:
        kept: deque[Item] = deque()
        for item in self.items:
            if item.generation == generation:
                kept.append(item)
            else:
                self.metrics.transport_failed += 1
        self.items = kept
        self._refresh_pending()

    def assert_conservation(self) -> None:
        assert self.metrics.accepted == (
            self.metrics.transmitted
            + self.metrics.transport_failed
            + self.metrics.pending
        )


@dataclass
class InputMetrics:
    received: int = 0
    usb_sent: int = 0
    coalesced: int = 0
    stale: int = 0
    dropped: int = 0
    highwater: int = 0


class UsbInputPump:
    """Latest-pending input plus one in-flight USB endpoint transfer."""

    def __init__(self, generation: int = 1) -> None:
        self.generation = generation
        self.pending: Item | None = None
        self.in_flight: Item | None = None
        self.endpoint_busy = False
        self.metrics = InputMetrics()
        self.submitted: list[Item] = []

    def receive(self, item: Item) -> None:
        self.metrics.received += 1
        if item.generation != self.generation:
            self.metrics.stale += 1
            return
        if self.pending is not None:
            self.metrics.coalesced += 1
        self.pending = item
        self.metrics.highwater = max(self.metrics.highwater, self.pending_count)
        self.pump()

    def pump(self) -> Item | None:
        if self.endpoint_busy or self.pending is None:
            return None
        item = self.pending
        self.pending = None
        self.in_flight = item
        self.endpoint_busy = True
        self.submitted.append(item)
        return item

    def endpoint_complete(self, success: bool = True) -> Item | None:
        assert self.endpoint_busy and self.in_flight is not None
        if success:
            self.metrics.usb_sent += 1
        else:
            self.metrics.dropped += 1
        self.in_flight = None
        self.endpoint_busy = False
        return self.pump()

    def change_generation(self, generation: int) -> None:
        self.generation = generation
        for item in (self.pending, self.in_flight):
            if item is not None and item.generation != generation:
                self.metrics.stale += 1
        self.pending = None
        self.in_flight = None
        self.endpoint_busy = False

    @property
    def pending_count(self) -> int:
        return int(self.pending is not None) + int(self.in_flight is not None)

    def assert_conservation(self) -> None:
        assert self.metrics.received == (
            self.metrics.usb_sent
            + self.metrics.coalesced
            + self.metrics.stale
            + self.metrics.dropped
            + self.pending_count
        )


class TelemetrySnapshot:
    def __init__(self) -> None:
        self.item: Item | None = None
        self.coalesced = 0
        self.highwater = 0

    def publish(self, item: Item) -> None:
        if self.item is not None:
            self.coalesced += 1
        self.item = item
        self.highwater = 1

    def clear_generation(self, generation: int) -> None:
        if self.item is not None and self.item.generation != generation:
            self.item = None

    @property
    def pending(self) -> int:
        return int(self.item is not None)


class BtInterruptScheduler:
    """ESP canonical HID interrupt scheduler and CAN_SEND latch model."""

    def __init__(self, generation: int = 1) -> None:
        self.generation = generation
        self.rt = RtRing()
        self.state31 = StateMailbox()
        self.state32 = StateMailbox()
        self.channel_open = True
        self.can_send_requested = False
        self.can_send_requests = 0
        self.rt_burst = 0
        self.selected: tuple[str, object] | None = None
        self.transmitted_generations: list[int] = []

    @property
    def has_pending(self) -> bool:
        return bool(
            self.rt.ready or self.state31.dirty or self.state32.dirty
        )

    def request_can_send(self) -> bool:
        if (
            self.channel_open
            and self.has_pending
            and not self.can_send_requested
        ):
            self.can_send_requested = True
            self.can_send_requests += 1
            return True
        return False

    def on_can_send(self, now_us: int) -> tuple[str, object] | None:
        assert self.can_send_requested
        assert self.selected is None
        self.can_send_requested = False
        self.rt.expire_ready(now_us)
        state_dirty = self.state31.dirty or self.state32.dirty

        selection: tuple[str, object] | None = None
        if self.rt.ready and (self.rt_burst < 3 or not state_dirty):
            handle = self.rt.select_oldest(self.generation)
            if handle is not None:
                selection = ("rt", handle)
        elif self.state31.dirty:
            snapshot = self.state31.snapshot(self.generation)
            if snapshot is not None:
                selection = ("state31", snapshot)
        elif self.state32.dirty:
            snapshot = self.state32.snapshot(self.generation)
            if snapshot is not None:
                selection = ("state32", snapshot)
        elif self.rt.ready:
            handle = self.rt.select_oldest(self.generation)
            if handle is not None:
                selection = ("rt", handle)

        self.selected = selection
        return selection

    def send_result(self, now_us: int, success: bool) -> None:
        assert self.selected is not None
        kind, selected = self.selected
        if kind == "rt":
            assert isinstance(selected, RtHandle)
            self.rt.complete(selected, now_us, success)
            self.rt_burst += 1
            if success:
                self.transmitted_generations.append(selected.item.generation)
        else:
            assert isinstance(selected, MailboxSnapshot)
            mailbox = self.state31 if kind == "state31" else self.state32
            mailbox.complete(selected, now_us, success)
            if success:
                self.rt_burst = 0
                self.transmitted_generations.append(selected.item.generation)
        self.selected = None
        self.request_can_send()

    def change_generation(self, generation: int) -> None:
        assert self.selected is None
        self.generation = generation
        self.rt.clear_generation(generation)
        self.state31.clear_generation(generation)
        self.state32.clear_generation(generation)
        self.can_send_requested = False
        self.rt_burst = 0


class M61Arbiter:
    """M61 transaction selection model, including aged control progress."""

    def __init__(self, generation: int = 1) -> None:
        self.generation = generation
        self.rt = RtRing()
        self.control = ControlQueue()
        self.input_pending: deque[Item] = deque(maxlen=4)
        self.input_serviced = 0

    def submit_input(self, item: Item) -> None:
        if len(self.input_pending) == self.input_pending.maxlen:
            self.input_pending.popleft()
        self.input_pending.append(item)

    def service(self, now_us: int) -> str | None:
        self.rt.expire_ready(now_us)
        handle = self.rt.select_oldest(self.generation)
        if handle is not None:
            self.rt.complete(handle, now_us, True)
            return "rt"
        if self.input_pending:
            self.input_pending.popleft()
            self.input_serviced += 1
            return "input"
        if (
            self.control.items
            and now_us - self.control.items[0].created_us >= 2_000
        ):
            self.control.complete_one(now_us)
            return "aged_control"
        if self.control.items:
            self.control.complete_one(now_us)
            return "control"
        return None


class GenerationStores:
    """All typed classes that must be invalidated on a generation change."""

    def __init__(self, generation: int = 1) -> None:
        self.generation = generation
        self.bt = BtInterruptScheduler(generation)
        self.control = ControlQueue()
        self.usb_input = UsbInputPump(generation)
        self.queues: dict[str, deque[Item]] = {
            "mic": deque(maxlen=4),
            "feature": deque(maxlen=8),
            "link": deque(maxlen=1),
        }
        self.telemetry = TelemetrySnapshot()
        self.audio_epochs: dict[int, Item] = {}

    def pending_generations(self) -> set[int]:
        generations: set[int] = set()
        for slot in self.bt.rt.slots:
            if slot.item is not None:
                generations.add(slot.item.generation)
        for mailbox in (self.bt.state31, self.bt.state32):
            if mailbox.item is not None:
                generations.add(mailbox.item.generation)
        generations.update(item.generation for item in self.control.items)
        for item in (self.usb_input.pending, self.usb_input.in_flight):
            if item is not None:
                generations.add(item.generation)
        for queue in self.queues.values():
            generations.update(item.generation for item in queue)
        if self.telemetry.item is not None:
            generations.add(self.telemetry.item.generation)
        generations.update(item.generation for item in self.audio_epochs.values())
        return generations

    def change_generation(self, generation: int) -> None:
        self.generation = generation
        self.bt.change_generation(generation)
        self.control.clear_generation(generation)
        self.usb_input.change_generation(generation)
        for name, queue in self.queues.items():
            self.queues[name] = deque(
                (item for item in queue if item.generation == generation),
                maxlen=queue.maxlen,
            )
        self.telemetry.clear_generation(generation)
        self.audio_epochs = {
            epoch: item
            for epoch, item in self.audio_epochs.items()
            if item.generation == generation
        }


@dataclass
class AudioEpoch:
    generation: int
    epoch: int
    captured_us: int
    haptics: bytes | None = None
    speaker: bytes | None = None
    speaker_enabled: bool = False

    @property
    def complete(self) -> bool:
        return self.haptics is not None and self.speaker is not None


class AudioEpochAssembler:
    def __init__(self, generation: int = 1, capacity: int = 4) -> None:
        self.generation = generation
        self.capacity = capacity
        self.epochs: dict[int, AudioEpoch] = {}
        self.last_emitted_epoch: int | None = None
        self.discontinuities = 0
        self.dropped = 0
        self.reports: list[tuple[AudioEpoch, AudioEpoch]] = []

    def _get(self, epoch: int, captured_us: int) -> AudioEpoch:
        if epoch not in self.epochs:
            if len(self.epochs) == self.capacity:
                oldest = min(self.epochs)
                del self.epochs[oldest]
                self.dropped += 1
            self.epochs[epoch] = AudioEpoch(
                self.generation, epoch, captured_us
            )
        return self.epochs[epoch]

    def add_haptics(self, epoch: int, data: bytes, captured_us: int) -> None:
        self._get(epoch, captured_us).haptics = data

    def add_speaker(
        self,
        epoch: int,
        data: bytes,
        captured_us: int,
        enabled: bool,
    ) -> None:
        item = self._get(epoch, captured_us)
        item.speaker = data
        item.speaker_enabled = enabled

    def assemble(self) -> list[tuple[AudioEpoch, AudioEpoch]]:
        while True:
            complete = sorted(
                epoch for epoch, item in self.epochs.items() if item.complete
            )
            if len(complete) < 2:
                break
            first = complete[0]
            second = complete[1]
            if second != first + 1:
                del self.epochs[first]
                self.dropped += 1
                self.discontinuities += 1
                continue
            if (
                self.last_emitted_epoch is not None
                and first != self.last_emitted_epoch + 1
            ):
                self.discontinuities += 1
            pair = (self.epochs.pop(first), self.epochs.pop(second))
            assert pair[0].epoch + 1 == pair[1].epoch
            self.last_emitted_epoch = second
            self.reports.append(pair)
        return self.reports


class DeferredUsbControl:
    """State model proving ISR capture and worker execution are separated."""

    def __init__(self, capacity: int = 8) -> None:
        self.capacity = capacity
        self.descriptors: deque[bytes] = deque()
        self.rearmed = 0
        self.notifications = 0
        self.worker_actions: list[str] = []
        self.isr_actions: list[str] = []

    def isr_callback(self, packet: bytes) -> bool:
        self.isr_actions.extend(("bounded_copy", "rearm", "notify"))
        accepted = len(self.descriptors) < self.capacity
        if accepted:
            self.descriptors.append(bytes(packet[:64]))
        self.rearmed += 1
        self.notifications += 1
        return accepted

    def worker(self) -> None:
        while self.descriptors:
            self.descriptors.popleft()
            self.worker_actions.extend(("config", "reconnect", "flash"))


@dataclass
class HciL2capModel:
    scheduler: BtInterruptScheduler
    hci_buffer_available: bool = True
    failed_by_class: dict[str, int] = field(default_factory=dict)

    def callback(self, now_us: int) -> str | None:
        selected = self.scheduler.on_can_send(now_us)
        if selected is None:
            return None
        kind, _ = selected
        success = self.hci_buffer_available
        if not success:
            self.failed_by_class[kind] = self.failed_by_class.get(kind, 0) + 1
        self.scheduler.send_result(now_us, success)
        return kind


def elapsed_mod(now: int, then: int, bits: int) -> int:
    return (now - then) & ((1 << bits) - 1)


def time_after_eq_mod(now: int, deadline: int, bits: int) -> bool:
    delta = elapsed_mod(now, deadline, bits)
    return delta < (1 << (bits - 1))


def spi_transaction_us(window_bytes: int, clock_mhz: int) -> float:
    return window_bytes * 8 / clock_mhz


def spi_capacity_hz(
    window_bytes: int, clock_mhz: int, turnaround_us: float = 1_000.0
) -> float:
    return 1_000_000.0 / (
        spi_transaction_us(window_bytes, clock_mhz) + turnaround_us
    )


class SchedulerBehaviorTests(unittest.TestCase):
    def test_01_ten_minute_realtime_with_periodic_stalls(self) -> None:
        scheduler = BtInterruptScheduler()
        duration_us = 600_000_000
        produced = 0
        next_production_us = 0

        for now_us in range(0, duration_us, 1_000):
            while next_production_us <= now_us:
                scheduler.rt.publish(
                    Item(1, produced, next_production_us, b"rt")
                )
                produced += 1
                next_production_us = produced * 64_000 // 3

            in_stall = now_us % 2_000_000 < 30_000
            if not in_stall and scheduler.has_pending:
                scheduler.request_can_send()
                selected = scheduler.on_can_send(now_us)
                if selected is not None:
                    scheduler.send_result(now_us, True)

        now_us = duration_us
        while scheduler.has_pending:
            scheduler.request_can_send()
            selected = scheduler.on_can_send(now_us)
            if selected is not None:
                scheduler.send_result(now_us, True)
            now_us += 1_000

        self.assertEqual(produced, 28_125)
        self.assertEqual(scheduler.rt.metrics.replaced, 0)
        self.assertEqual(scheduler.rt.metrics.stale, 0)
        self.assertEqual(scheduler.rt.metrics.admission_rejected, 0)
        self.assertLessEqual(scheduler.rt.metrics.highwater, RT_CAPACITY)
        self.assertLessEqual(scheduler.rt.metrics.age_max_us, RT_MAX_AGE_US)
        scheduler.rt.assert_invariants()

    def test_02_state_flood_coalesces_without_rejecting_rt(self) -> None:
        scheduler = BtInterruptScheduler()
        rt_sequence = 0
        next_rt_us = 0

        for now_us in range(0, 1_000_000, 1_000):
            scheduler.state31.publish(Item(1, now_us // 1_000, now_us))
            if next_rt_us <= now_us:
                scheduler.rt.publish(Item(1, rt_sequence, next_rt_us))
                rt_sequence += 1
                next_rt_us = rt_sequence * 64_000 // 3
            if now_us % 5_000 == 0:
                scheduler.request_can_send()
                selected = scheduler.on_can_send(now_us)
                if selected is not None:
                    scheduler.send_result(now_us, True)

        now_us = 1_000_000
        while scheduler.has_pending:
            scheduler.request_can_send()
            selected = scheduler.on_can_send(now_us)
            if selected is not None:
                scheduler.send_result(now_us, True)
            now_us += 1_000

        self.assertGreater(scheduler.state31.metrics.coalesced, 0)
        self.assertLessEqual(scheduler.state31.metrics.highwater, 1)
        self.assertEqual(scheduler.rt.metrics.admission_rejected, 0)
        self.assertEqual(
            scheduler.rt.metrics.accepted, scheduler.rt.metrics.transmitted
        )
        scheduler.rt.assert_invariants()

    def test_03_control_flood_makes_bounded_progress(self) -> None:
        scheduler = M61Arbiter()
        control_sequence = 0
        input_sequence = 0
        rt_sequence = 0
        next_rt_us = 0

        for now_us in range(0, 200_000, 500):
            scheduler.control.submit(Item(1, control_sequence, now_us))
            control_sequence += 1
            if now_us % 2_000 == 0:
                scheduler.submit_input(Item(1, input_sequence, now_us))
                input_sequence += 1
            if next_rt_us <= now_us:
                scheduler.rt.publish(Item(1, rt_sequence, next_rt_us))
                rt_sequence += 1
                next_rt_us = rt_sequence * 64_000 // 3
            scheduler.service(now_us)

        self.assertGreater(scheduler.control.metrics.transmitted, 0)
        self.assertGreater(scheduler.input_serviced, 0)
        # Eight slots at a 500 us service quantum, plus input/RT priority,
        # gives a deterministic bound below 6 ms for this overload trace.
        self.assertLessEqual(max(scheduler.control.completed_latencies), 6_000)
        self.assertLessEqual(scheduler.rt.metrics.age_max_us, 500)
        self.assertEqual(scheduler.rt.metrics.admission_rejected, 0)
        scheduler.control.assert_conservation()
        scheduler.rt.assert_invariants()

    def test_04_usb_busy_completion_retries_newest_without_new_input(self) -> None:
        pump = UsbInputPump()
        first = Item(1, 1, 0, "first")
        second = Item(1, 2, 1_000, "second")
        third = Item(1, 3, 2_000, "third")

        pump.receive(first)
        self.assertTrue(pump.endpoint_busy)
        pump.receive(second)
        pump.receive(third)
        self.assertEqual(pump.pending, third)

        retried = pump.endpoint_complete()
        self.assertEqual(retried, third)
        self.assertEqual(pump.submitted, [first, third])
        pump.endpoint_complete()
        pump.assert_conservation()

    def test_05_generation_change_clears_every_typed_store(self) -> None:
        stores = GenerationStores()
        stores.bt.rt.publish(Item(1, 1, 0))
        stores.bt.rt.publish(Item(1, 2, 1))
        stores.bt.state31.publish(Item(1, 3, 2))
        stores.bt.state32.publish(Item(1, 4, 3))
        stores.control.submit(Item(1, 5, 4))
        stores.usb_input.receive(Item(1, 6, 5))
        for sequence, queue in enumerate(stores.queues.values(), 7):
            queue.append(Item(1, sequence, sequence))
        stores.telemetry.publish(Item(1, 10, 10))
        stores.audio_epochs[11] = Item(1, 11, 11)
        self.assertEqual(stores.pending_generations(), {1})

        stores.change_generation(2)
        self.assertEqual(stores.pending_generations(), set())
        self.assertFalse(stores.bt.has_pending)

        stores.bt.rt.publish(Item(2, 12, 12))
        stores.bt.request_can_send()
        selected = stores.bt.on_can_send(13)
        self.assertIsNotNone(selected)
        stores.bt.send_result(13, True)
        self.assertEqual(stores.bt.transmitted_generations, [2])
        stores.bt.rt.assert_invariants()
        stores.control.assert_conservation()
        stores.usb_input.assert_conservation()

    def test_06_timer_wrap_vectors_for_32_and_64_bits(self) -> None:
        for bits in (32, 64):
            mask = (1 << bits) - 1
            then = mask - 99
            now = 150
            self.assertEqual(elapsed_mod(now, then, bits), 250)
            deadline = 50
            self.assertTrue(time_after_eq_mod(now, deadline, bits))
            self.assertFalse(time_after_eq_mod(deadline - 1, deadline, bits))
            self.assertTrue(time_after_eq_mod(deadline, deadline, bits))

        metrics = ClassMetrics(accepted=(1 << 32) + 7)
        metrics.transmitted = 1 << 32
        metrics.pending = 7
        self.assertEqual(
            metrics.accepted, metrics.transmitted + metrics.pending
        )

    def test_07_randomized_traces_preserve_conservation(self) -> None:
        rng = random.Random(0xD505)
        scheduler = BtInterruptScheduler()
        control = ControlQueue()
        pump = UsbInputPump()
        telemetry = TelemetrySnapshot()
        generation = 1
        sequence = 0

        for step in range(20_000):
            now_us = step * 250
            choice = rng.randrange(12)
            item = Item(generation, sequence, now_us, sequence)
            sequence += 1

            if choice <= 2:
                scheduler.rt.publish(item)
            elif choice == 3:
                scheduler.state31.publish(item)
            elif choice == 4:
                scheduler.state32.publish(item)
            elif choice == 5:
                control.submit(item)
            elif choice == 6:
                pump.receive(item)
            elif choice == 7 and pump.endpoint_busy:
                pump.endpoint_complete(success=rng.randrange(20) != 0)
            elif choice == 8 and control.items:
                control.complete_one(now_us, success=rng.randrange(20) != 0)
            elif choice == 9:
                telemetry.publish(item)
            elif choice == 10 and scheduler.has_pending:
                scheduler.request_can_send()
                selected = scheduler.on_can_send(now_us)
                if selected is not None:
                    scheduler.send_result(now_us, rng.randrange(20) != 0)
            elif choice == 11:
                generation += 1
                scheduler.change_generation(generation)
                control.clear_generation(generation)
                pump.change_generation(generation)
                telemetry.clear_generation(generation)

            scheduler.rt.assert_invariants()
            control.assert_conservation()
            pump.assert_conservation()
            self.assertLessEqual(scheduler.state31.metrics.pending, 1)
            self.assertLessEqual(scheduler.state32.metrics.pending, 1)
            self.assertLessEqual(telemetry.pending, 1)

    def test_08_spi_capacity_at_4_and_8_mhz_for_all_windows(self) -> None:
        windows = {"small": 128, "medium": 256, "large": 692}
        for size in windows.values():
            time_4 = spi_transaction_us(size, 4)
            time_8 = spi_transaction_us(size, 8)
            self.assertAlmostEqual(time_8 * 2, time_4)
            self.assertGreater(spi_capacity_hz(size, 8), 500)
            self.assertGreater(spi_capacity_hz(size, 8), spi_capacity_hz(size, 4))

        self.assertAlmostEqual(spi_transaction_us(windows["large"], 4), 1_384)
        self.assertLess(spi_capacity_hz(windows["large"], 4), 500)
        self.assertGreater(spi_capacity_hz(windows["small"], 4), 500)
        self.assertGreater(spi_capacity_hz(windows["medium"], 4), 500)
        self.assertGreater(spi_capacity_hz(windows["large"], 8), 500)

        # Ten MHz is useful only as a margin comparison, never a required gate.
        self.assertGreater(
            spi_capacity_hz(windows["large"], 10),
            spi_capacity_hz(windows["large"], 8),
        )

    def test_09_audio_epochs_never_cross_pair_after_delay_or_gap(self) -> None:
        assembler = AudioEpochAssembler()
        assembler.add_haptics(1, b"h1", 0)
        assembler.add_haptics(2, b"h2", 10_000)
        assembler.add_speaker(2, b"s2", 15_000, True)
        self.assertEqual(assembler.assemble(), [])

        assembler.add_speaker(1, b"s1", 20_000, True)
        reports = assembler.assemble()
        self.assertEqual([(a.epoch, b.epoch) for a, b in reports], [(1, 2)])
        self.assertEqual(reports[0][0].haptics, b"h1")
        self.assertEqual(reports[0][0].speaker, b"s1")

        for epoch in (4, 5):
            assembler.add_haptics(epoch, f"h{epoch}".encode(), epoch * 10_000)
            assembler.add_speaker(
                epoch, f"s{epoch}".encode(), epoch * 10_000, True
            )
        reports = assembler.assemble()
        self.assertEqual([(a.epoch, b.epoch) for a, b in reports], [(1, 2), (4, 5)])
        self.assertEqual(assembler.discontinuities, 1)
        for first, second in reports:
            self.assertEqual(first.epoch + 1, second.epoch)

    def test_10_telemetry_flood_is_one_replaceable_snapshot(self) -> None:
        scheduler = BtInterruptScheduler()
        control = ControlQueue()
        telemetry = TelemetrySnapshot()
        scheduler.rt.publish(Item(1, 1, 0))
        control.submit(Item(1, 2, 0))

        for sequence in range(100_000):
            telemetry.publish(Item(1, sequence, sequence))

        self.assertEqual(telemetry.pending, 1)
        self.assertEqual(telemetry.highwater, 1)
        self.assertEqual(telemetry.coalesced, 99_999)
        self.assertEqual(scheduler.rt.metrics.admission_rejected, 0)
        self.assertEqual(control.metrics.admission_rejected, 0)
        self.assertEqual(scheduler.rt.metrics.pending, 1)
        self.assertEqual(control.metrics.pending, 1)

    def test_11_usb_control_work_is_deferred_out_of_isr(self) -> None:
        model = DeferredUsbControl()
        self.assertTrue(model.isr_callback(bytes(range(100))))
        self.assertEqual(model.descriptors[0], bytes(range(64)))
        self.assertEqual(model.worker_actions, [])
        self.assertEqual(model.isr_actions, ["bounded_copy", "rearm", "notify"])
        self.assertNotIn("flash", model.isr_actions)
        self.assertNotIn("config", model.isr_actions)
        self.assertNotIn("reconnect", model.isr_actions)

        model.worker()
        self.assertEqual(model.descriptors, deque())
        self.assertEqual(model.worker_actions, ["config", "reconnect", "flash"])

    def test_12_can_send_latch_and_hci_failure_are_accounted(self) -> None:
        scheduler = BtInterruptScheduler()
        scheduler.rt.publish(Item(1, 1, 0))
        self.assertTrue(scheduler.request_can_send())
        self.assertFalse(scheduler.request_can_send())
        self.assertEqual(scheduler.can_send_requests, 1)

        l2cap = HciL2capModel(scheduler, hci_buffer_available=False)
        self.assertEqual(l2cap.callback(1_000), "rt")
        self.assertEqual(l2cap.failed_by_class, {"rt": 1})
        self.assertEqual(scheduler.rt.metrics.transport_failed, 1)
        self.assertEqual(scheduler.rt.metrics.pending, 0)
        scheduler.rt.assert_invariants()

        scheduler.state31.publish(Item(1, 2, 2_000))
        scheduler.request_can_send()
        self.assertEqual(l2cap.callback(3_000), "state31")
        self.assertEqual(l2cap.failed_by_class, {"rt": 1, "state31": 1})
        self.assertTrue(scheduler.state31.dirty)

    def test_13_response_classes_make_bounded_progress_under_flood(self) -> None:
        slots = ("input", "mic", "input", "link", "input", "mic", "status", "input", "telemetry")
        control_burst = 0
        cursor = 0
        selected = []

        for _ in range(54):
            if control_burst < 2:
                selected.append("control")
                control_burst += 1
                continue

            selected.append(slots[cursor])
            cursor = (cursor + 1) % len(slots)
            control_burst = 0

        for response_class in ("control", "input", "mic", "link", "status", "telemetry"):
            positions = [
                index for index, value in enumerate(selected) if value == response_class
            ]
            self.assertGreaterEqual(len(positions), 2)
            self.assertLessEqual(max(b - a for a, b in zip(positions, positions[1:])), 27)


if __name__ == "__main__":
    unittest.main(verbosity=2)
