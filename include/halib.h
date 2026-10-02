// Home Assistant support / config librrary
#ifdef	CONFIG_REVK_HALIB

 // Path: NULL is base, /suffix is after base, non / is absolute path

typedef struct
{
   const char *id;              // tag/id no spaces
   const char *name;            // name
   const char *type;            // type
   const char *subtype;         // sub type
   const char *unit;            // unit
   const char *stat;            // stat topic (default main status)
   const char *cmd;             // command topic (default main command)
   const char *info;            // info topic (default main info)
   const char *field;           // field name (default id)
   const char *payload;         // payload
   const char *icon;            // icon
   const char *stateclass;	// state_class
   uint8_t diag:1;		// diagnostic sensor
   uint8_t fallback:1;		// fallback value_template
   uint8_t delete:1;            // deleted entry
} ha_config_t;

// Sensor
#define ha_config_binary(...)  ha_config_opts("binary_sensor",(ha_config_t){__VA_ARGS__})
#define ha_config_sensor(...)  ha_config_opts("sensor",(ha_config_t){__VA_ARGS__})
#define ha_config_switch(...)  ha_config_opts("switch",(ha_config_t){__VA_ARGS__})
#define ha_config_light(...)  ha_config_opts("light",(ha_config_t){__VA_ARGS__})
#define ha_config_trigger(...)  ha_config_opts("device_automation",(ha_config_t){__VA_ARGS__})
const char *ha_config_opts (const char *, ha_config_t);

#endif
