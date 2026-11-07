// smartkitchen_daily_import_rulechain_20261103_v1 - Generator node (ThingsBoard rule engine, JavaScript)
// Runs once a day (86 400 s). Asks SmartKitchen for the last 7 days, today included (dates in UTC).
// Rule chain: Generator -> REST API Call -> Transformation script -> Save Time Series (device SK-01).
// The REST API Call node uses the header  permanent-token: ${secret:smartkitchen_token;type:TEXT}
// The token itself is kept in ThingsBoard Secrets storage and is not in this repository.
var DAY = 86400000;
var now = Date.now();
function isoDay(ms) { return new Date(ms).toISOString().substring(0, 10); }
var body = { time: { start: isoDay(now - 7 * DAY), end: isoDay(now) } };
return { msg: body, metadata: {}, msgType: "POST_TELEMETRY_REQUEST" };
