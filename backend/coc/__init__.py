"""CoC (Center of Control) mode — workstation that aggregates events
from one or more upstream Predator-RF backends instead of (or alongside)
talking to local Kujhad nodes directly. Used when a TOC has multiple
deployed field stations and wants a single fused picture."""
from .aggregator import CoCAggregator

__all__ = ["CoCAggregator"]
