R"LIVING({
  "schema_version": "earth.world-handoff/v1",
  "request_id": "cb9ad714-e3c6-4c5a-8bd3-85add9656769",
  "world_binding": {
    "schema_version": "earth.world-binding/v1",
    "address": "usap://mutsea/entity/f539fb5d-82b5-414c-8563-a612cd0bff1b",
    "authority": "mutsea",
    "world_id": "6a9f81e5-60d2-4f51-9667-9ead80a7a6eb",
    "region_id": "f539fb5d-82b5-414c-8563-a612cd0bff1b",
    "anchor": {
      "longitude_degrees": -73.55767849652506,
      "latitude_degrees": 45.505789448877024,
      "height_metres": -31.511247952468693
    },
    "frame": {
      "axes": [
        "east",
        "north",
        "up"
      ],
      "unit": "metre"
    },
    "heading_degrees": 19.315532505681574,
    "scale": 1,
    "version": "854f90aeec4b704e62a7bcdddad7b2ee0fbecf88917c678f0f6e3c150559008c"
  },
  "world_entry": {
    "schema_version": "earth.world-entry/v1",
    "address": "usap://spatialdb/entity/500e9e1d-9014-516c-819a-8a735700c2db",
    "authority": "mutsea",
    "world_id": "6a9f81e5-60d2-4f51-9667-9ead80a7a6eb",
    "region_id": "f539fb5d-82b5-414c-8563-a612cd0bff1b",
    "anchor": {
      "longitude_degrees": -73.55278678594807,
      "latitude_degrees": 45.506864606990256,
      "height_metres": -14.42434980534017
    },
    "authorization": "required",
    "binding_relation": "geographic_region"
  },
  "publication": {
    "graph_id": "a2cbf7d5-4fff-5219-86a5-85e8fcde2752",
    "version": "9d29e9ed5aa6ba05b6c38d075ad5c02541243f7729161b9fe1698a81c4ca6fd8",
    "root_sha256": "46af71114c2d361f2154fb847bca74111ac2dc908a50179eb2ffdc784340f020",
    "frame_id": "montreal-old-port-jacques-cartier.eus/01873c70f04efef3",
    "origin": {
      "height_metres": -12.43081833422184,
      "latitude_degrees": 45.507201203216475,
      "longitude_degrees": -73.55350350168884
    },
    "axes": [
      "east",
      "up",
      "south"
    ],
    "unit": "metre"
  },
  "connection": {
    "protocol": "opensim.login/v1",
    "login_uri": "http://127.0.0.1:19080/",
    "start": "uri:Old Montreal&321&239&19"
  },
  "return_to": {
    "origin": "http://127.0.0.1:18648",
    "destination": {
      "schema_version": "earth.navigation/v1",
      "kind": "geographic",
      "position": {
        "longitude_degrees": -73.55278678594807,
        "latitude_degrees": 45.506864606990256,
        "height_metres": -14.42434980534017
      }
    }
  }
}
)LIVING"
