"""Unified audit CSV: node summaries followed by event-resolution records."""

NODE_FIELDS = [
    "site",
    "id",
    "node_type",
    "label",
    "creation_timestamp_missing",
    "creation_evidence",
    "creation_evidence_label",
    "sent",
    "received",
    "sent_to_resolved_target",
    "sent_to_unresolved_or_ambiguous_target",
    "received_interval_exact",
    "received_interval_and_current_username_same",
    "received_current_username_no_interval_conflict",
    "received_current_username_no_creation_timestamp",
    "received_other_resolved",
    "received_synthetic_missing_no_identity_evidence",
    "received_synthetic_ambiguous_multiple_active_intervals",
    "received_synthetic_missing_current_created_after_thank",
    "received_synthetic_missing_interval_account_created_after_thank",
    "received_synthetic_other_unresolved",
]
EVENT_FIELDS = [
    "site",
    "logid",
    "timestamp",
    "source",
    "target",
    "observed_target_label",
    "target_label_subtype",
    "target_resolution",
    "current_name_account_id",
    "current_name_creation_timestamp",
    "resolved_account_creation_timestamp",
    "interval_evidence",
    "requires_historical_review",
]
AUDIT_FIELDS = [
    "record_type",
    "site",
    "id",
    "node_type",
    "label",
    "creation_timestamp_missing",
    "creation_evidence",
    "creation_evidence_label",
    "sent",
    "received",
    "sent_to_resolved_target",
    "sent_to_unresolved_or_ambiguous_target",
    "received_interval_exact",
    "received_interval_and_current_username_same",
    "received_current_username_no_interval_conflict",
    "received_current_username_no_creation_timestamp",
    "received_other_resolved",
    "received_synthetic_missing_no_identity_evidence",
    "received_synthetic_ambiguous_multiple_active_intervals",
    "received_synthetic_missing_current_created_after_thank",
    "received_synthetic_missing_interval_account_created_after_thank",
    "received_synthetic_other_unresolved",
    "logid",
    "timestamp",
    "source",
    "target",
    "observed_target_label",
    "target_label_subtype",
    "target_resolution",
    "current_name_account_id",
    "current_name_creation_timestamp",
    "resolved_account_creation_timestamp",
    "interval_evidence",
    "requires_historical_review",
]

EMPTY_FIELDS = {
    kind: tuple(k for k in AUDIT_FIELDS if k not in fields and k != "record_type")
    for kind, fields in (("node", NODE_FIELDS), ("event", EVENT_FIELDS))
}


def records(reader, kind):
    """Stream one record type; legacy event-only audits remain readable."""
    if "record_type" not in reader.fieldnames:
        if kind == "event":
            yield from reader
        return
    event_seen = False
    for row in reader:
        if row.get("record_type") not in ("node", "event"):
            raise ValueError("Unknown audit record_type")
        if row["record_type"] == "node" and event_seen:
            raise ValueError("Node record after event records")
        event_seen |= row["record_type"] == "event"
        if any(row.get(k) != "" for k in EMPTY_FIELDS[row["record_type"]]):
            raise ValueError("Nonempty field belonging to other audit record type")
        if row["record_type"] == kind:
            yield row
        elif kind == "node":
            return  # Canonical audit ordering: all nodes precede all events.
