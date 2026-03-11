#!/bin/sh
# =============================================================================
# init-dynsec.sh
# Runs inside the mosquitto container on first start.
# Initialises dynsec.json with roles and static users using mosquitto_ctrl.
#
# Roles:
#   provisioning-role  — admin access to dynsec control topics
#   bootstrap-role     — publish to $provision/register only
#   subscriber-role    — subscribe read-only to all topics
#   device-role        — scoped to devices/%c/# (client ID substitution)
#
# Static users (created here, passwords from env):
#   provisioning-service
#   bootstrap
#   subscriber-service
#
# Device users are created at runtime by the provisioning service.
# =============================================================================

DYNSEC_FILE="/mosquitto/config/dynsec.json"

if [ -f "$DYNSEC_FILE" ]; then
  echo "dynsec.json already exists — skipping init"
  exit 0
fi

echo "Initialising dynsec.json..."

# Validate required env vars are set
if [ -z "$PROVISIONING_PASSWORD" ] || [ -z "$BOOTSTRAP_PASSWORD" ] || [ -z "$SUBSCRIBER_PASSWORD" ]; then
  echo "ERROR: PROVISIONING_PASSWORD, BOOTSTRAP_PASSWORD and SUBSCRIBER_PASSWORD must be set"
  exit 1
fi

# Initialise the dynsec file with the admin user (provisioning-service)
mosquitto_ctrl dynsec init "$DYNSEC_FILE" provisioning-service "$PROVISIONING_PASSWORD"

# Helper — run dynsec commands against the local file (no broker needed)
ctrl() {
  mosquitto_ctrl --config-file "$DYNSEC_FILE" dynsec "$@"
}

# --- Roles ---

ctrl createRole provisioning-role
# dynsec admins need access to the control topics
ctrl addRoleACL provisioning-role publishClientSend   '$CONTROL/dynamic-security/#' allow
ctrl addRoleACL provisioning-role subscribePattern    '$CONTROL/dynamic-security/#' allow
ctrl addRoleACL provisioning-role subscribePattern    '$provision/register'          allow
ctrl addRoleACL provisioning-role publishClientSend   '$provision/response/#'        allow

ctrl createRole bootstrap-role
ctrl addRoleACL bootstrap-role publishClientSend '$provision/register'     allow
ctrl addRoleACL bootstrap-role subscribePattern  '$provision/response/#'   allow

ctrl createRole subscriber-role
ctrl addRoleACL subscriber-role subscribePattern '#' allow

ctrl createRole device-role
# %c is substituted with the connecting client's username at runtime
ctrl addRoleACL device-role publishClientSend  'devices/%c/#' allow
ctrl addRoleACL device-role subscribePattern   'devices/%c/#' allow
ctrl addRoleACL device-role subscribePattern   '$provision/response/%c' allow

# --- Static users ---

ctrl createClient bootstrap "$BOOTSTRAP_PASSWORD"
ctrl addClientRole bootstrap bootstrap-role

ctrl createClient subscriber-service "$SUBSCRIBER_PASSWORD"
ctrl addClientRole subscriber-service subscriber-role

# provisioning-service was created by init, just assign its role
ctrl addClientRole provisioning-service provisioning-role

echo "dynsec.json initialised successfully"