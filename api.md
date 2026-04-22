# Set a row (0–5)
curl -X POST http://192.168.100.23/row/0 -d "GATE CHANGE B12"
curl -X POST http://192.168.100.23/row/3 -d "DELAYED 20 MIN"

# Clear a row
curl -X DELETE http://192.168.100.23/row/2/clear

# Get status JSON
curl http://192.168.100.23/status
The status endpoint returns something like:


{"wifi":"Meraki","ip":"192.168.100.23","rssi":-62,"bars":2,"free_heap":214320,"min_heap":201440,"uptime_s":47}