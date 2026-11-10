// Home Assistant support / config librrary
#include "revk.h"
#ifdef CONFIG_REVK_HALIB
#include "halib.h"

const char *ha_config_opts(const char *config, ha_config_t h)
{
    if (!h.id)
        return "No name";
    char *topic;
    if (asprintf(&topic, "%s/%s/%s%s/config", topicha, config, revk_id, h.id) < 0)
        return "malloc fail";
    char *hastate = revk_topic(topicstate, NULL, NULL);
    char *hacommand = revk_topic(topiccommand, NULL, NULL);
    char *hainfo = revk_topic(topicinfo, NULL, NULL);
    jo_t j = jo_object_alloc();
    void addpath(const char *tag, const char *base, const char *path)
    { // Allow path. NULL is base, /suffix is after base, non / is absolute path
        if (!path || !*path)
            jo_string(j, tag, base);
        else if (*path == '/')
            jo_stringf(j, tag, "%s%s", base, path);
        else
            jo_string(j, tag, path);
    }
    jo_stringf(j, "unique_id", "%s%s", revk_id, h.id);
    jo_object(j, "dev");
    jo_array(j, "ids");
    jo_string(j, NULL, revk_id);
    jo_close(j);
#ifdef CONFIG_MDNS_MAX_INTERFACES
    jo_string(j, "name", *instance ? instance : hostname);
#else
    jo_string(j, "name", hostname);
#endif
    jo_string(j, "mdl", revk_app);
    jo_string(j, "sw", revk_version);
    jo_string(j, "mf", "www.me.uk");
    jo_close(j);
    if (h.diag)
        jo_string(j, "ent_cat", "diagnostic");
    if (h.icon)
        jo_string(j, "icon", h.icon);
    if (h.type)
        jo_string(j, "dev_cla", h.type);
    if (h.name)
        jo_string(j, "name", h.name);
    if (h.cmd)
        addpath("cmd_t", hacommand, h.cmd);
    if (h.stateclass)
        jo_string(j, "state_class", h.stateclass);
    if (!strcmp(config, "device_automation")) {
        jo_string(j, "platform", config);
        jo_string(j, "automation_type", "trigger");
        jo_string(j, "type", h.type ?: "button_short_press");
        jo_string(j, "subtype", h.subtype ?: "button_1");
        addpath("topic", hainfo, h.info);
        if (h.payload)
            jo_string(j, "payload", h.payload);
    }
    char binary = !strcmp(config, "binary_sensor");
    if (h.stat || h.field) {
        addpath("stat_t", hastate, h.stat);
        if (!h.field || *h.field) {
            if (h.fallback)
                jo_stringf(
                    j, "val_tpl",
                    "{%% if value_json.%s is defined %%}{{value_json.%s%s}}{%% else %%}{{this.state}}{%% endif %%}",
                    h.field ?: h.id, h.field ?: h.id, binary ? " | abs" : "");
            else
                jo_stringf(j, "val_tpl", "{{value_json.%s%s}}", h.field ?: h.id, binary ? " | abs" : "");
        }
    }
    if (!strcmp(config, "sensor")) { // Sensor
        if (h.unit)
            jo_string(j, "unit_of_meas", h.unit);
    } else if (!strcmp(config, "light")) {
        jo_string(j, "schema", "json");
    } else if (binary) {
        jo_int(j, "pl_off", 0);
        jo_int(j, "pl_on", 1);
    }
    // Availability
    jo_string(j, "avty_t", hastate);
    jo_string(j, "avty_tpl", "{{value_json.up}}");
    jo_bool(j, "pl_avail", 1);
    jo_bool(j, "pl_not_avail", 0);
    free(hastate);
    free(hacommand);
    free(hainfo);
    revk_mqtt_send(NULL, 1, topic, h.delete ? NULL : &j);
    free(topic);
    return NULL;
}

#endif
