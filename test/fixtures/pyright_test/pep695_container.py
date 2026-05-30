"""PEP 695 generic class fixture for PyrightBridge.findTypeDefinition.

This file exercises the regex follow-set that historically did not
accept `[`. The class declaration has no `(...)` inheritance list and
no `:` immediately after the name — only `[T]` — so the pre-fix
regex's `[\\s(:]` follow-set fails to match.
"""


class Container[T]:
    """A PEP 695 generic container — Python 3.12+."""

    def __init__(self, value: T) -> None:
        self.value = value

    def get(self) -> T:
        return self.value


class Pair[K, V]:
    """Two type parameters, no `(...)` inheritance."""

    def __init__(self, key: K, value: V) -> None:
        self.key = key
        self.value = value


class BoundedBox[T: int]:
    """PEP 696 bound — `class Foo[T: Bound]` form."""

    def __init__(self, value: T) -> None:
        self.value = value


class DefaultedBox[T = int]:
    """PEP 696 default — `class Foo[T = Default]` form."""

    def __init__(self, value: T) -> None:
        self.value = value
