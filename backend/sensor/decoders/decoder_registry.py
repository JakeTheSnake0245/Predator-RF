import logging
import subprocess
import asyncio
from abc import ABC, abstractmethod
from typing import Dict, List, Optional, Type

logger = logging.getLogger(__name__)


class SignalDecoder(ABC):
    """Base class for signal decoders."""

    @abstractmethod
    async def decode(self, center_freq: float, iq_stream: bytes) -> Optional[dict]:
        """Attempt to decode signal. Returns payload dict or None."""
        pass

    @abstractmethod
    def get_capability(self) -> dict:
        pass


class P25Decoder(SignalDecoder):
    """P25 digital voice via DSD-FME subprocess bridge."""

    async def decode(self, center_freq: float, iq_stream: bytes) -> Optional[dict]:
        # DSD-FME is invoked by the C++ decoder_ingest module.
        # This Python class provides metadata and coordination only.
        return None

    def get_capability(self) -> dict:
        return {
            'name': 'p25',
            'frequency_bands': ['vhf', 'uhf'],
            'processing_latency_ms': 100,
            'cpu_percent': 30,
            'external_dependency': 'dsd-fme',
        }


class DMRDecoder(SignalDecoder):
    """DMR digital voice decoder."""

    async def decode(self, center_freq: float, iq_stream: bytes) -> Optional[dict]:
        return None

    def get_capability(self) -> dict:
        return {
            'name': 'dmr',
            'frequency_bands': ['vhf', 'uhf'],
            'processing_latency_ms': 80,
            'cpu_percent': 25,
        }


class RTL433Decoder(SignalDecoder):
    """ISM band sensor data via rtl_433 subprocess."""

    async def decode(self, center_freq: float, iq_stream: bytes) -> Optional[dict]:
        return None

    def get_capability(self) -> dict:
        return {
            'name': 'rtl433',
            'frequency_bands': ['uhf'],
            'processing_latency_ms': 200,
            'cpu_percent': 15,
            'external_dependency': 'rtl_433',
        }


class FMDecoder(SignalDecoder):
    """Wide FM demodulation (broadcast, public safety)."""

    async def decode(self, center_freq: float, iq_stream: bytes) -> Optional[dict]:
        return {'modulation': 'wfm', 'frequency': center_freq}

    def get_capability(self) -> dict:
        return {
            'name': 'fm',
            'frequency_bands': ['vhf', 'uhf'],
            'processing_latency_ms': 50,
            'cpu_percent': 10,
        }


class DecoderRegistry:
    """Plugin registry for signal decoders."""

    def __init__(self):
        self._decoders: Dict[str, Type[SignalDecoder]] = {}
        self._register_defaults()

    def _register_defaults(self):
        self.register('p25', P25Decoder)
        self.register('dmr', DMRDecoder)
        self.register('rtl433', RTL433Decoder)
        self.register('fm', FMDecoder)

    def register(self, name: str, cls: Type[SignalDecoder]):
        self._decoders[name] = cls

    def get_decoder(self, name: str) -> Optional[SignalDecoder]:
        cls = self._decoders.get(name)
        return cls() if cls else None

    def get_decoders_for_frequency(self, center_freq: float) -> List[SignalDecoder]:
        band = self._freq_to_band(center_freq)
        result = []
        for name, cls in self._decoders.items():
            inst = cls()
            if band in inst.get_capability().get('frequency_bands', []):
                result.append(inst)
        return result

    @staticmethod
    def _freq_to_band(freq_hz: float) -> str:
        if freq_hz < 30e6:
            return 'hf'
        elif freq_hz < 300e6:
            return 'vhf'
        elif freq_hz < 3e9:
            return 'uhf'
        return 'shf'

    def list_decoders(self) -> List[str]:
        return list(self._decoders.keys())


decoder_registry = DecoderRegistry()
