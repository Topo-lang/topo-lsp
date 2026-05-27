"""Fixture module exercised by PyrightBridge integration tests."""


class Widget:
    """A simple widget with an integer state."""

    def __init__(self, x: int) -> None:
        self.x = x

    def compute(self) -> int:
        """Return the widget's value plus one."""
        return self.x + 1


def make_widget(x: int) -> Widget:
    """Factory for a :class:`Widget`."""
    return Widget(x)


def sum_widgets(count: int) -> int:
    """Sum the computed values of ``count`` widgets."""
    total = 0
    for i in range(count):
        total += make_widget(i).compute()
    return total
