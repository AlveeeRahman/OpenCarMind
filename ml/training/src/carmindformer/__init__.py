"""CarMind-Former: a channel-agnostic patch transformer for in-vehicle time series.

Any set of named channels (OBD-II PIDs from CarMind, HCRL Driving Dataset signals, per-CAN-ID
message rates) is turned into patch tokens that carry a channel embedding, so one model trains on
several cars and sensors and transfers to a car it has never seen. Self-supervised objectives:
masked patch reconstruction, a change-score head for sudden regime changes, and a window-level
contrastive embedding. Optional supervised probes: driver identity and CAN attack flags.
"""
__version__ = "0.1.0"
