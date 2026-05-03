from backend.models.sensor_node import SensorNodeTrust
from backend.sensor.modes import ModeConfig, SURVEY_MODE, MONITOR_MODE, ANALYSIS_MODE


class AdaptiveModeSelector:
    """Select the optimal sensing mode for a node given context."""

    def recommend_mode(self, node: SensorNodeTrust,
                       track_priority: str,
                       cpu_available_percent: float,
                       power_available_percent: float = 100.0) -> ModeConfig:
        """
        Returns the recommended ModeConfig.

        Priority levels: CRITICAL / HIGH / NORMAL / LOW
        """
        # No hardware info → fall back to survey
        if not node.hardware_capabilities:
            return SURVEY_MODE

        # Resource constrained → survey
        if cpu_available_percent < 20 or power_available_percent < 30:
            return SURVEY_MODE

        if track_priority == 'CRITICAL':
            # TDOA-capable hardware → deep analysis
            if node.hardware_capabilities.supports_tdoa:
                return ANALYSIS_MODE
            return MONITOR_MODE

        if track_priority in ('HIGH', 'NORMAL'):
            if cpu_available_percent > 50:
                return MONITOR_MODE
            return SURVEY_MODE

        # LOW or default → background survey
        return SURVEY_MODE
