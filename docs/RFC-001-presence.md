# RFC-001 Presence Automation

Status: Draft for comment

Audience:

- backend team
- future mobile app team
- device / firmware team
- design / product-firmware team

## Purpose

This RFC captures the current collaborative design direction for presence-driven automations in the rebooter-droids platform.

This is not the final design. It is a redline document intended to absorb comments from all teams before implementation begins.

## Current Direction

Presence automation should be a central platform capability, not a device-firmware responsibility.

Proposed flow:

```text
presence source event
-> central presence API
-> append-only presence_events queue
-> async rule engine
-> commands queue
-> device polls and executes
```

## Goals

1. Keep device firmware local-first.
2. Avoid battery-heavy mobile behavior.
3. Minimize privacy exposure.
4. Make automations auditable.
5. Support group-oriented actions, not only per-device actions.

## Non-Goals For v1

1. Firmware-based WLAN sniffing or MAC presence inference on the Sonoff S31
2. Continuous GPS trail collection
3. Central server directly crawling home/office routers for MAC tables
4. Multi-service distributed rule execution

## Presence Sources

### v1

- phone geofence events only

### Later

- optional router/LAN presence via a dedicated router agent
- app heartbeat / presence capability state
- activity context signals such as driving arrival/departure

## Data Model Direction

### places

Store:

- `place_id`
- `name`
- `lat`
- `lon`
- `radius_m`

Do not store address strings as canonical identifiers.

### subjects

Store:

- `subject_id`
- `user_id`
- `subject_kind`
- `display_name`
- `privacy_class`

Allowed `subject_kind` values:

- `phone`
- `laptop`
- `vehicle`
- `network_presence_source`

### presence_events

Append-only.

Never mutate rows after insert.

Suggested fields:

- `event_id`
- `subject_id`
- `place_id`
- `event_type`
- `received_at`
- `client_reported_at`
- `payload`

Allowed initial `event_type` values:

- `entered_place`
- `exited_place`
- `presence_capability_state`

### subject_presence

Derived current-state table maintained by the rule engine.

Used for:

- current known state per subject
- health of the presence source
- uncertainty handling

### occupancy_state

Should be derived on read from `subject_presence`, not persisted as the source of truth in v1.

## API Direction

Recommended namespace:

- `/rebooter/api/v1/presence/*`

Examples:

- `POST /rebooter/api/v1/presence/events`
- `GET /rebooter/api/v1/presence/places`
- `POST /rebooter/api/v1/presence/places`
- `GET /rebooter/api/v1/presence/rules`
- `POST /rebooter/api/v1/presence/rules`

Presence event schemas should be versioned separately from device command schemas.

## Rule Engine Direction

Recommended implementation:

- async event ingestion
- queue-backed processing
- rule engine inside the current backend container for now

Rule engine should emit commands into the existing commands queue.

Every presence-triggered command should be tagged in audit logs with:

- `triggered_by = "automation:<rule_id>"`

## Example Automations

1. When user A enters Home, turn on Arrival Group.
2. When all tracked users leave Home for 10 minutes, turn off Away Group.
3. When presence source becomes unhealthy, suppress automated off-actions.

## Initial Command Scope

Presence automations should initially issue only:

- `relay_on`
- `relay_off`
- `relay_toggle`
- `device_restart`

Advanced command types such as `apply_config` or `set_mode` should remain available to admins but are not the initial presence-automation target.

## Battery Guidance

Mobile app should prefer:

- OS geofence APIs
- event delivery over continuous polling
- minimal background work

Server-side debounce is preferred over pushing complexity onto the phone client.

## Privacy Guidance

Default policy should be:

- store enter/exit events
- do not store continuous location trails
- retain raw presence events for a bounded TTL, default suggestion `90 days`
- allow optional longer retention only by explicit admin setting

## Failure Modes

1. GPS permission revoked
2. mobile event buffered and delivered late
3. phone clock skew
4. multi-user uncertain occupancy
5. geofence count limits on iOS
6. conflicting or dangerous rules
7. central multi-node failover while processing event streams

## Initial Semantics Suggestions

1. If a tracked subject has no credible presence update for more than 5 minutes, that subject becomes `unknown`, not `away`.
2. New rules should support a dry-run mode for the first 24 hours.
3. Group/device command cooldown should exist to prevent repeated relay cycling storms.

## Firmware Implications

Firmware should:

1. keep local control independent of presence features
2. accept group/device commands from central as it already does for other automation sources
3. report local relay-state changes through heartbeat/event reporting so central state does not drift
4. support ordered `central_base_urls` for future multi-node failover

Firmware should not:

1. decide human presence
2. monitor arbitrary WLAN MAC presence directly

## Open Questions For Teams

### Backend

1. Is event queue + in-container rule engine still the preferred first shape?
2. What command cooldown policy should be default?
3. Should dry-run mode exist at the rule record level or deployment level?

### Mobile

1. Which platform should ship first?
2. How should active geofence rotation be handled if place count exceeds iOS limits?
3. What presence capability heartbeat shape is most realistic?

### Firmware

1. What is the minimum safe cooldown between relay-cycle style actions on the same device/group?
2. Which local relay state changes should be echoed immediately to central?

### Design/Product

1. Should v1 be single-user first or multi-user first?
2. Should presence automations initially be limited to group on/off flows only?

## Recommended Next Steps

1. Review this RFC across teams.
2. Redline semantics before implementation.
3. Lock the presence event schema separately from device command schema.
4. Implement `places`, `subjects`, `presence_events`, `presence_rules`, and `subject_credentials` on the backend.
5. Build one-platform, single-user, geofence-only mobile proof of concept first.
