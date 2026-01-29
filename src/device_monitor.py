"""Device monitor with low-frequency polling and change-only notifications."""

from __future__ import annotations

import threading
from collections.abc import Callable, Iterable

DeviceLister = Callable[[], Iterable[str]]
ChangeHandler = Callable[[set[str], set[str]], None]


class DeviceMonitor:
    """Monitors device list changes using low-frequency polling.

    This class intentionally avoids any image/video stream handling and
    focuses purely on device enumeration.
    """

    def __init__(
        self,
        list_devices: DeviceLister,
        on_change: ChangeHandler,
        poll_interval_s: float = 2.0,
    ) -> None:
        if poll_interval_s < 1.0:
            raise ValueError("poll_interval_s must be at least 1 second.")
        self._list_devices = list_devices
        self._on_change = on_change
        self._poll_interval_s = poll_interval_s
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_devices: set[str] | None = None

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self, timeout_s: float | None = None) -> None:
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=timeout_s)

    def snapshot(self) -> set[str]:
        return set(self._list_devices())

    def _run(self) -> None:
        while not self._stop_event.is_set():
            self.check_for_changes()
            self._stop_event.wait(self._poll_interval_s)

    def check_for_changes(self) -> None:
        current = self.snapshot()
        if self._last_devices is None:
            self._last_devices = current
            return
        if current != self._last_devices:
            previous = self._last_devices
            self._last_devices = current
            self._on_change(previous, current)

    @property
    def poll_interval_s(self) -> float:
        return self._poll_interval_s
