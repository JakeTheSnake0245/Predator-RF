"""Predator RF — Reticulum Network Stack (RNS) transport bridge.

This package adds RNS as a parallel transport for CoT/XML alongside the
existing TAK UDP/TCP path. See backend/rns/README.md for architecture.
"""
from .schema import (
    INTERFACE_TYPES,
    DEVICE_LOCAL_FIELDS,
    validate_interface,
    validate_config,
    placeholder_paths,
    SchemaError,
)
from .token import (
    export_token,
    import_token,
    mint_replication_token,
    TokenError,
    TOKEN_PREFIX,
)
from .envelope import wrap_cot, unwrap_cot, EnvelopeError

__all__ = [
    "INTERFACE_TYPES",
    "DEVICE_LOCAL_FIELDS",
    "validate_interface",
    "validate_config",
    "placeholder_paths",
    "SchemaError",
    "export_token",
    "import_token",
    "mint_replication_token",
    "TokenError",
    "TOKEN_PREFIX",
    "wrap_cot",
    "unwrap_cot",
    "EnvelopeError",
]
