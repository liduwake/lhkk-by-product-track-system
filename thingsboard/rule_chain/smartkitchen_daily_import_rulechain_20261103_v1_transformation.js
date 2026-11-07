// smartkitchen_daily_import_rulechain_20261103_v1 - Transformation script node (ThingsBoard rule engine, JavaScript)
// SmartKitchen response -> one telemetry entry per waste day for SK-01.
// Keys as in tools/smartkitchen_to_thingsboard_20261101_v2; ts = 12:00 UTC of the day.
var locations = Array.isArray(msg) ? msg : [msg];
var days = {};
function categoryKey(name) {
  return String(name).toLowerCase()
    .replace(/ä/g, "a").replace(/ö/g, "o").replace(/å/g, "a")
    .replace(/[^a-z0-9]+/g, "_") + "_g";
}
for (var i = 0; i < locations.length; i++) {
  var cats = locations[i].categories || [];
  for (var c = 0; c < cats.length; c++) {
    var k = categoryKey(cats[c].category_name);
    var items = cats[c].items || [];
    for (var j = 0; j < items.length; j++) {
      var data = items[j].data || [];
      for (var n = 0; n < data.length; n++) {
        var e = data[n];
        var w = Math.round(Number(e.weight) || 0);
        var d = days[e.date];
        if (!d) { d = { daily_total_g: 0, entries: 0, ewc_code_missing: 0 }; days[e.date] = d; }
        d.daily_total_g += w;
        d.entries += 1;
        if (e.code === null || e.code === undefined || String(e.code).trim() === "") { d.ewc_code_missing += 1; }
        d[k] = (d[k] || 0) + w;
      }
    }
  }
}
var out = [];
for (var date in days) {
  out.push({ ts: Date.parse(date + "T12:00:00Z"), values: days[date] });
}
return { msg: out, metadata: metadata, msgType: "POST_TELEMETRY_REQUEST" };
